/*
 * Broadcom ControlVault fingerprint sensor driver for libfprint
 *
 * Copyright (C) 2026 Erik Håkansson <erikhakan@gmail.com>
 *
 * Reverse-engineered from the proprietary TOD plugin.
 * Supports Broadcom ControlVault 3 (BCM5882/Citadel) USB fingerprint sensors
 * with PIDs 0x5840-0x5845.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "broadcom"

#include "drivers_api.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "broadcom_msg.h"

G_DECLARE_FINAL_TYPE (FpiDeviceBroadcom, fpi_device_broadcom, FPI, DEVICE_BROADCOM, FpDevice)

/* Minimum firmware versions allowed with this driver.
 * AAI (Application Image): e.g. "00515010" -> 515010, the first three components of firmware 5.15.010.0.
 * SBI (Secondary Boot Image): independent bootloader version, decimal, e.g. 229.
 *
 * With BCM_FW_VERSION_STRICT set, old AAI firmware fails probe so fprintd does
 * not expose a device that may fail later through PAM.
 */
#define BCM_MIN_FW_VERSION 515010
#define BCM_MIN_SBI_VERSION 229
#define BCM_FW_VERSION_STRICT 1

/* USB endpoints */
#define BCM_EP_OUT (1 | FPI_USB_ENDPOINT_OUT)          // 0x01 - bulk OUT
#define BCM_EP_IN (1 | FPI_USB_ENDPOINT_IN)            // 0x81 - bulk IN
#define BCM_EP_INTERRUPT (5 | FPI_USB_ENDPOINT_IN)     // 0x85 - interrupt IN

/* Timeouts in ms.
 * BCM_COMMAND_TIMEOUT / BCM_DATA_TIMEOUT are kept short (5 s) so that when the
 * USB device vanishes (e.g. after a fatal firmware crash) the stacked USB
 * transfers fail quickly and PAM falls back to password without a long wait.
 * Normal device responses arrive in ~20 ms so 5 s is ample margin.
 * BCM_FINGER_TIMEOUT must stay long - we wait for the user to place a finger. */
#define BCM_COMMAND_TIMEOUT (5000)
#define BCM_DATA_TIMEOUT (5000)
#define BCM_FINGER_TIMEOUT (30000)

/* Enrollment stages - device signals completion via progress_state&0xFF==1, but needs
 * up to 12 good captures. Report 12 so libfprint never warns about stage counts
 * exceeding nr_enroll_stages. The progress_state flag drives early exit. */
#define BCM_ENROLL_STAGES (12)

/* USB IDs */
#define BCM_USB_VID (0x0a5c)

/* Working PIDs at the time of writing */
static const FpIdEntry id_table[] = {
  {.vid = BCM_USB_VID, .pid = 0x5840},
  {.vid = BCM_USB_VID, .pid = 0x5841},
  {.vid = BCM_USB_VID, .pid = 0x5842},
  {.vid = BCM_USB_VID, .pid = 0x5843},
  {.vid = BCM_USB_VID, .pid = 0x5844},
  {.vid = BCM_USB_VID, .pid = 0x5845},
  {.vid = 0, .pid = 0},
};

/* Max response buffer size (GET_VERSION response is ~928 bytes) */
#define BCM_MAX_RESPONSE (1024)

/* Response status codes (u32 LE at offset 20) */
#define BCM_STATUS_OK 0x00000000
#define BCM_STATUS_SESSION_STUCK 0x00000017         // OPEN: session-ID pool exhausted / stale sessions left behind
#define BCM_STATUS_FAILED 0x00000047
#define BCM_STATUS_RETRY_REMOVE 0x000000a4          // finger not read correctly - remove and retry
#define BCM_STATUS_RETRY_OFF_CENTER 0x00000089      // off-center or finger moved - center finger
#define BCM_STATUS_DELETE_TEMPLATE_NOT_FOUND 0x0000001b
#define BCM_STATUS_REQUESTED_TEMPLATE_NOT_FOUND 0x00000059

/* Firmware SRAM session pool.
 *
 * Session IDs seem to start at a fixed value and then iterate +0x70 bytes for the next ID.
 * Thus, there are 20 fixed SRAM session slots. After they are exhausted, from e.g. failed
 * session closing, the firmware starts to return 0x17 errors for every attempt to start a
 * new session.
 * This can be recovered from by simply iterating over all possible session IDs and attempt
 * to close all sessions.
 */
#define BCM_SESSION_ID_POOL_START (0x2402c730u)
#define BCM_SESSION_ID_POOL_STRIDE (0x70u)
#define BCM_SESSION_ID_POOL_COUNT (20u)
#define BCM_SESSION_OPEN_RETRIES (5u)

/* Interrupt notification status (u32 LE at offset 0 of 8-byte interrupt).
 * 0x00000000 means response ready on bulk IN and accompanies normal command completion.
 * 0x00000003 means finger-on-sensor and is used by the finger-wait path.
 */
#define BCM_INTERRUPT_READY 0x00000000
#define BCM_INTERRUPT_FINGER 0x00000003

/* Enroll SSM - each operation opens/closes its own protocol session */
typedef enum {
  BCM_ENROLL_CLOSE_STALE = 0, // CLOSE any cached stale session from a previous crash
  BCM_ENROLL_CANCEL_INIT, // send CANCEL_CAPTURE to clear any stale capture state
  BCM_ENROLL_OPEN,
  BCM_ENROLL_START,
  BCM_ENROLL_GET_CHALLENGE,
  BCM_ENROLL_WAIT_FINGER, // poll interrupt endpoint until status=3 finger event
  BCM_ENROLL_UPDATE, // send ENROLL_UPDATE after finger event; device responds immediately
  BCM_ENROLL_COMMIT,
  BCM_ENROLL_CLOSE,
  BCM_ENROLL_NUM_STATES,
} BcmEnrollState;

/* Verify SSM - each operation opens/closes its own protocol session */
typedef enum {
  BCM_VERIFY_CLOSE_STALE = 0, // CLOSE any cached stale session from a previous crash
  BCM_VERIFY_CANCEL_INIT, // send CANCEL_CAPTURE to clear any stale capture state
  BCM_VERIFY_OPEN,
  BCM_VERIFY_GET_CHALLENGE,
  BCM_VERIFY_WAIT_FINGER, // poll interrupt endpoint until status=3 finger event
  BCM_VERIFY_CAPTURE, // send CAPTURE after finger event; device responds immediately
  BCM_VERIFY_CANCEL,
  BCM_VERIFY_CLOSE,
  BCM_VERIFY_NUM_STATES,
} BcmVerifyState;

/* Identify SSM - sends all N template IDs in a single CAPTURE command;
 * device performs on-device 1:N matching and returns the matched template. */
typedef enum {
  BCM_IDENTIFY_CLOSE_STALE = 0, // CLOSE any cached stale session from a previous crash
  BCM_IDENTIFY_CANCEL_INIT, // clear any stale capture state
  BCM_IDENTIFY_OPEN,
  BCM_IDENTIFY_GET_CHALLENGE,
  BCM_IDENTIFY_WAIT_FINGER,
  BCM_IDENTIFY_CAPTURE, // single CAPTURE with all template IDs
  BCM_IDENTIFY_CANCEL,
  BCM_IDENTIFY_CLOSE,
  BCM_IDENTIFY_NUM_STATES,
} BcmIdentifyState;

/* Delete SSM - opens a session, sends DELETE_OBJECT, closes. */
typedef enum {
  BCM_DELETE_CLOSE_STALE = 0, // CLOSE any cached stale session from a previous crash
  BCM_DELETE_CANCEL_INIT, // clear any stale capture state
  BCM_DELETE_OPEN,
  BCM_DELETE_DELETE, // send DELETE_OBJECT with the template_id
  BCM_DELETE_CLOSE,
  BCM_DELETE_NUM_STATES,
} BcmDeleteState;

/* Command SSM - handles the send -> interrupt -> receive pattern */
typedef enum {
  BCM_COMMAND_SEND = 0,
  BCM_COMMAND_WAIT_INTERRUPT,
  BCM_COMMAND_RECEIVE,
  BCM_COMMAND_NUM_STATES,
} BcmCommandState;

/* Do not send this command. On this device family, 0x71 hangs the firmware
 * hard enough that a reboot may be required to recover the sensor. */
static const guint8 BCM_COMMAND_ENUMERATE_OBJECTS_DIRECT = 0x71;

/* Callback type for command completion */
typedef void (*BcmCommandCallback)(FpiDeviceBroadcom *self,
                                   GError            *error);

struct _FpiDeviceBroadcom
{
  FpDevice           parent;

  guint32            session_id;
  guint8             challenge_hash[BCM_HASH_SIZE];
  guint32            template_id;
  gint               enroll_stage;

  FpiSsm            *task_ssm;
  FpiSsm            *command_ssm;

  guint8            *command_buffer;
  gsize              command_length;
  BcmCommand         command;
  guint8             response_buffer[BCM_MAX_RESPONSE];
  gsize              response_length;
  guint8             interrupt_buf[BCM_INTERRUPT_SIZE];

  BcmCommandCallback command_callback;

  /* Stashed error for the close-on-error path */
  GError  *task_error;

  gint     verify_match_result; // -1 = unset, 0 = no-match, 1 = match
  FpPrint *identify_match; // matched template (NULL = no match)
  FpPrint *identify_scan; // new scan print (NULL = no match)

  guint    open_retry_count;
  guint    blind_close_index; // index into BCM_SESSION_POOL during blind recovery

  /* Session cache fd. Held open with LOCK_EX while session is active.
   * Released (and file unlinked) on session close. OS releases lock on
   * crash, allowing the next instance to detect and close the stale session. */
  gint cache_fd;
};

/* GVariant type / GObject property strings */
static const gchar *const variant_type_uint32 = "(u)";
static const gchar *const fpi_data_property_name = "fpi-data";

/* Device identification */
static const gchar *const broadcom_id = "broadcom";
static const gchar *const broadcom_full_name = "Broadcom ControlVault Fingerprint Sensor";
static const gchar *const serial_format = "BCM-%04x";

/* Session cache constants */
static const gchar *const session_cache_path_format = "%s/broadcom-%04x-%08x.session";
static const gchar *const session_cache_default_directory = "/var/lib/fprint/session-cache";
static const gchar *const session_cache_environment_variable = "FP_BROADCOM_SESSION_DIR";

G_DEFINE_TYPE (FpiDeviceBroadcom, fpi_device_broadcom, FP_TYPE_DEVICE)

/* Driver entry points */
static void enroll_ssm_run_state (FpiSsm   *ssm,
                                  FpDevice *device);
static void enroll_ssm_done (FpiSsm   *ssm,
                             FpDevice *device,
                             GError   *error);
static void verify_ssm_run_state (FpiSsm   *ssm,
                                  FpDevice *device);
static void verify_ssm_done (FpiSsm   *ssm,
                             FpDevice *device,
                             GError   *error);
static void identify_ssm_run_state (FpiSsm   *ssm,
                                    FpDevice *device);
static void identify_ssm_done (FpiSsm   *ssm,
                               FpDevice *device,
                               GError   *error);
static void delete_ssm_run_state (FpiSsm   *ssm,
                                  FpDevice *device);
static void delete_ssm_done (FpiSsm   *ssm,
                             FpDevice *device,
                             GError   *error);

/* Shared helpers */
static void send_command (FpiDeviceBroadcom *self,
                          guint8            *buffer,
                          gsize              length,
                          BcmCommand         command,
                          BcmCommandCallback callback);
static void close_stale_state (FpiDeviceBroadcom *self);
static GError *check_response_status_from_header (const FpiDeviceBroadcom *self,
                                                  const BcmMessageHeader  *header,
                                                  guint32                 *status_out);
static GError *check_response_status_from_buffer (const FpiDeviceBroadcom *self,
                                                  guint32                 *status_out);
static guint32 session_cache_read (FpiDeviceBroadcom *self);
static void session_cache_write (FpiDeviceBroadcom *self,
                                 guint32            session_id);
static void session_cache_delete (FpiDeviceBroadcom *self);
static void session_cache_release (FpiDeviceBroadcom *self);

/******************************************************************************
 *
 *  Probe
 *
 *  Opens the USB device, builds a stable synthetic serial, and performs the
 *  sessionless GET_VERSION firmware check used to reject known-unsupported
 *  firmware versions before normal driver operations are exposed.
 *
 *****************************************************************************/
static void
probe (FpDevice *device)
{
  GError *error = NULL;
  g_autofree gchar *serial = NULL;
  g_autofree guint8 *command_buffer = NULL;
  guint8 interrupt_buf[BCM_INTERRUPT_SIZE] = { 0 };
  guint8 response_buffer[BCM_MAX_RESPONSE] = { 0 };
  gsize command_length;
  gsize actual_length;
  BcmInterrupt interrupt;
  guint32 firmware_version = 0;
  guint32 sbi_version = 0;

  GUsbDevice *usb_device = fpi_device_get_usb_device (device);

  if (!g_usb_device_open (usb_device, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  fp_info ("Broadcom ControlVault sensor detected (PID: 0x%04x)", g_usb_device_get_pid (usb_device));
  serial = g_strdup_printf (serial_format, g_usb_device_get_pid (usb_device));

  /* Claim the interface to run GET_VERSION and check the firmware version.
   * On any transport or parse error we log a warning and allow probe to succeed. We only reject the device when we can positively confirm the
   * firmware is too old.  GET_VERSION is sessionless, so no stale-session risk. */
  if (!g_usb_device_claim_interface (usb_device, 0, 0, &error))
    {
      fp_warn ("Could not claim interface: %s", error->message);
      g_clear_error (&error);
      goto done;
    }

  command_buffer = bcm_build_get_version (&command_length);
  if (!g_usb_device_bulk_transfer (usb_device, BCM_EP_OUT, command_buffer, command_length, &actual_length, BCM_COMMAND_TIMEOUT, NULL, &error))
    {
      fp_warn ("GET_VERSION send failed: %s", error->message);
      g_clear_error (&error);
      goto release;
    }

  if (!g_usb_device_interrupt_transfer (usb_device, BCM_EP_INTERRUPT, interrupt_buf, BCM_INTERRUPT_SIZE, &actual_length, BCM_COMMAND_TIMEOUT, NULL, &error))
    {
      fp_warn ("GET_VERSION interrupt read failed: %s", error->message);
      g_clear_error (&error);
      goto release;
    }
  if (!bcm_parse_interrupt (interrupt_buf, actual_length, &interrupt))
    {
      fp_warn ("GET_VERSION bad interrupt packet");
      goto release;
    }
  if (interrupt.status != BCM_INTERRUPT_READY || interrupt.response_length == 0 || interrupt.response_length > BCM_MAX_RESPONSE)
    {
      fp_warn ("GET_VERSION bad interrupt (status=0x%08x, response_len=%u)", interrupt.status, interrupt.response_length);
      goto release;
    }

  if (!g_usb_device_bulk_transfer (usb_device, BCM_EP_IN, response_buffer, interrupt.response_length, &actual_length, BCM_DATA_TIMEOUT, NULL, &error))
    {
      fp_warn ("GET_VERSION response read failed: %s", error->message);
      g_clear_error (&error);
      goto release;
    }
  if (!bcm_parse_version_response (response_buffer, actual_length, &firmware_version, &sbi_version))
    {
      fp_warn ("Could not parse GET_VERSION response, skipping version check");
      goto release;
    }

  // AAI (Application Image) version check.
  fp_info ("Broadcom AAI firmware version %08u", firmware_version);
  if (firmware_version < BCM_MIN_FW_VERSION)
    {
      fp_warn (
        "Device supported but AAI firmware version %08u is below the minimum supported %08u. "
        "Install the proprietary driver on Windows or the proprietary driver from "
        "https://packages.broadcom.com/artifactory/dell-controlvault-drivers/ (5.xx tree, not 6.xx) or Ubuntu OEM libfprint-2-tod1-broadcom Launchpad to update firmware.",
        firmware_version, (guint32) BCM_MIN_FW_VERSION);
#if BCM_FW_VERSION_STRICT
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "AAI firmware version %08u below minimum %08u", firmware_version, (guint32) BCM_MIN_FW_VERSION);
#endif
    }
  else
    {
      fp_dbg ("Broadcom AAI firmware %08u >= minimum %08u", firmware_version, (guint32) BCM_MIN_FW_VERSION);
    }

  /* SBI version check.
   * GET_VERSION exposes a separate SBI version field, but this driver does not use SBI for any runtime protocol decisions.
   * Until we have stronger evidence for what low SBI affects, this check can remain warn-only.
   */
  if (sbi_version == 0)
    fp_warn ("Broadcom SBI version unknown, skipping SBI version check");
  else if (sbi_version < BCM_MIN_SBI_VERSION)
    fp_warn ("Broadcom SBI (Secondary Boot Image) version %u is below minimum %u; firmware upgrades via the proprietary driver are recommended", sbi_version,
             (guint32) BCM_MIN_SBI_VERSION);
  else
    fp_dbg ("Broadcom SBI version %u >= minimum %u", sbi_version, (guint32) BCM_MIN_SBI_VERSION);

release:
  g_usb_device_release_interface (usb_device, 0, 0, NULL);
done:
  g_usb_device_close (usb_device, NULL);
  if (error)
    fpi_device_probe_complete (device, NULL, NULL, error);
  else
    fpi_device_probe_complete (device, serial, NULL, NULL);
}

/******************************************************************************
 *
 *  Driver entry points
 *
 *****************************************************************************/
static void
device_open (FpDevice *device)
{
  GUsbDevice *usb_device = fpi_device_get_usb_device (device);
  GError *error = NULL;

  /* Reset the device to clear any stale session state from a previous
   * operation that may not have closed cleanly. */
  if (!g_usb_device_reset (usb_device, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_device, 0, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  fpi_device_open_complete (device, NULL);
}

static void
device_close (FpDevice *device)
{
  g_autoptr(GError) release_error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (device), 0, 0, &release_error);
  fp_dbg ("Broadcom device closed");
  fpi_device_close_complete (device, g_steal_pointer (&release_error));
}

/*
 * Enroll flow:
 *
 *   enroll()
 *     -> SSM: CLOSE_STALE -> CANCEL_INIT -> OPEN -> START -> GET_CHALLENGE -> WAIT_FINGER -> UPDATE -> [loop back to GET_CHALLENGE] -> COMMIT -> CLOSE
 *
 *   Callbacks per state:
 *     START         -> enroll_command_done
 *     GET_CHALLENGE -> enroll_challenge_done
 *     UPDATE        -> enroll_update_done  (loops or jumps to COMMIT)
 *     COMMIT        -> enroll_commit_done
 *     CLOSE         -> task_close_done     (shared)
 *
 *   On error: task_error() -> jump to CLOSE -> task_close_done
 *   SSM done: enroll_ssm_done
 */
static void
enroll (FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->enroll_stage = 0;
  self->template_id = 0;
  self->session_id = 0;
  g_clear_error (&self->task_error);

  self->task_ssm = fpi_ssm_new_full (device, enroll_ssm_run_state, BCM_ENROLL_NUM_STATES, BCM_ENROLL_NUM_STATES, "ENROLL");
  fpi_ssm_start (self->task_ssm, enroll_ssm_done);
}

/*
 * Verify flow:
 *
 *   verify()
 *     -> SSM: CLOSE_STALE -> CANCEL_INIT -> OPEN -> GET_CHALLENGE -> WAIT_FINGER -> CAPTURE -> CANCEL -> CLOSE
 *
 *   Callbacks per state:
 *     GET_CHALLENGE -> verify_challenge_done
 *     CAPTURE       -> verify_capture_done
 *     CANCEL        -> verify_cancel_done
 *     CLOSE         -> task_close_done   (shared)
 *
 *   On error: task_error() -> jump to CLOSE -> task_close_done
 *   SSM done: verify_ssm_done
 */
static void
verify (FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->session_id = 0;
  self->verify_match_result = -1;
  g_clear_error (&self->task_error);

  self->task_ssm = fpi_ssm_new_full (device, verify_ssm_run_state, BCM_VERIFY_NUM_STATES, BCM_VERIFY_NUM_STATES, "VERIFY");
  fpi_ssm_start (self->task_ssm, verify_ssm_done);
}

/*
 * Identify flow:
 *
 *   identify()
 *     -> SSM: CLOSE_STALE -> CANCEL_INIT -> OPEN -> GET_CHALLENGE -> WAIT_FINGER -> CAPTURE -> CANCEL -> CLOSE
 *
 *   Callbacks per state:
 *     GET_CHALLENGE -> identify_challenge_done
 *     CAPTURE       -> identify_capture_done  (1:N match; all template IDs in one CAPTURE)
 *     CANCEL        -> identify_cancel_done
 *     CLOSE         -> task_close_done         (shared)
 *
 *   On error: task_error() -> jump to CLOSE -> task_close_done
 *   SSM done: identify_ssm_done
 */
static void
identify (FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->session_id = 0;
  g_clear_object (&self->identify_match);
  g_clear_object (&self->identify_scan);
  g_clear_error (&self->task_error);

  self->task_ssm = fpi_ssm_new_full (device, identify_ssm_run_state, BCM_IDENTIFY_NUM_STATES, BCM_IDENTIFY_NUM_STATES, "IDENTIFY");
  fpi_ssm_start (self->task_ssm, identify_ssm_done);
}

/*
 * Delete flow:
 *
 *   delete_fingerprint()
 *     -> SSM: CLOSE_STALE -> CANCEL_INIT -> OPEN -> DELETE -> CLOSE
 *
 *   Callbacks per state:
 *     DELETE -> delete_done
 *     CLOSE  -> task_close_done  (shared)
 *
 *   On error: task_error() -> jump to CLOSE -> task_close_done
 *   SSM done: delete_ssm_done
 */
static void
delete_fingerprint (FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  FpPrint *print = NULL;
  GVariant *data = NULL;

  fpi_device_get_delete_data (device, &print);
  g_assert (print != NULL);
  g_object_get (print, fpi_data_property_name, &data, NULL);

  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE (variant_type_uint32)))
    {
      g_clear_pointer (&data, g_variant_unref);
      fpi_device_delete_complete (device, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "Invalid print data"));
      return;
    }

  g_variant_get (data, variant_type_uint32, &self->template_id);
  g_variant_unref (data);

  fp_info ("Removing template 0x%08x from device", self->template_id);

  self->session_id = 0;
  g_clear_error (&self->task_error);

  self->task_ssm = fpi_ssm_new_full (device, delete_ssm_run_state, BCM_DELETE_NUM_STATES, BCM_DELETE_NUM_STATES, "DELETE");
  fpi_ssm_start (self->task_ssm, delete_ssm_done);
}

/******************************************************************************
 *
 *  These callbacks are used by all four operation SSMs (enroll, verify,
 *  identify, delete).
 *  They are defined here once, before the individual operation sections.
 *
 *  Operation flow skeleton (same for every operation):
 *
 *    CLOSE_STALE -> close_stale_state (closes cached stale sessions)
 *         |
 *    CANCEL_INIT -> cancel_init_done
 *         |
 *       OPEN -> task_open_done  (retries on 0x17 via pool sweep)
 *         |
 *    [operation-specific states]
 *         |
 *      CLOSE -> task_close_done
 *         |
 *    SSM done -> {operation}_ssm_done
 *
 *  A stale session is a firmware session that may have been left open by a
 *  previous process after its lock file was released by process exit or crash.
 *  CLOSE_STALE closes stale sessions found in the cache. If OPEN still returns
 *  0x17, task_open_done falls back to sweeping the fixed firmware session pool.
 *
 *  On any mid-flow error, task_error() jumps directly to the CLOSE state,
 *  stashing the error in self->task_error for task_close_done to propagate.
 *
 *******************************************************************************/

/*
 * Stash an error and jump to the appropriate CLOSE state so the
 * protocol session is always cleaned up before the task SSM finishes.
 * The close state's done callback calls fpi_ssm_mark_completed(), and the
 * task SSM done handler propagates task_error to libfprint.
 *
 * If the error is a cancellation, skip the CLOSE attempt. The cancellable
 * is already cancelled so any USB transfer would fail immediately anyway.
 * The next operation begins with CANCEL_INIT which resets device state.
 */
static void
task_error (FpiDeviceBroadcom *self, GError *error, gint close_state)
{
  g_clear_error (&self->task_error);
  self->task_error = error;

  if (self->session_id != 0 && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    fpi_ssm_jump_to_state (self->task_ssm, close_state);
  else
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&self->task_error));
}

/*
 * Callback for the CLOSE_STALE state: result of closing one cached stale session.
 * Deletes the cache file for the just-closed session, then re-enters
 * close_stale_state to handle any remaining stale sessions before advancing.
 */
static void
close_stale_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    {
      fp_warn ("CLOSE of stale cached session failed: %s", error->message);
    }
  else
    {
      fp_info ("Successfully closed stale cached session");
      session_cache_delete (self);
    }
  g_clear_error (&error);

  self->session_id = 0;
  close_stale_state (self);
}

/*
 * Shared state handler for the CLOSE_STALE step in any task SSM.
 * Scans the cache directory; for each unlocked (stale) session found, sends
 * CLOSE and loops back via close_stale_done until none remain, then
 * advances to the next state.
 */
static void
close_stale_state (FpiDeviceBroadcom *self)
{
  const guint32 stale_session_id = session_cache_read (self);

  if (stale_session_id != 0)
    {
      gsize length;

      fp_info ("Found cached stale session 0x%08x, sending CLOSE...", stale_session_id);
      self->session_id = stale_session_id;
      BcmCommand command = {0};
      guint8 *buffer = bcm_build_close (stale_session_id, &length, &command);
      send_command (self, buffer, length, command, close_stale_done);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

/*
 * Callback after sending CLOSE to clear a stale session during OPEN recovery.
 * Ignores the CLOSE result and restarts the SSM from CANCEL_INIT (state 1).
 */
static void
open_stale_close_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    fp_warn ("CLOSE of stale session failed: %s", error->message);
  g_clear_error (&error);

  self->session_id = 0;
  // Jump to CANCEL_INIT (state 1), skipping CLOSE_STALE (state 0)
  fpi_ssm_jump_to_state (self->task_ssm, 1);
}

/*
 * Callback after each CLOSE during the pool-sweep 0x17 recovery.
 * Sends CLOSE for the next slot in the firmware session pool, or finishes
 * by jumping to CANCEL_INIT once all 20 slots have been tried.
 */
static void
open_blind_close_done (FpiDeviceBroadcom *self, GError *error)
{
  gsize length;
  BcmCommand command = {0};

  g_clear_error (&error);

  self->blind_close_index++;

  if (self->blind_close_index >= BCM_SESSION_ID_POOL_COUNT)
    {
      fp_dbg ("Session pool sweep complete; retrying OPEN");
      self->session_id = 0;
      // Jump to CANCEL_INIT (state 1), skipping CLOSE_STALE (state 0)
      fpi_ssm_jump_to_state (self->task_ssm, 1);
      return;
    }

  const guint32 session_id = BCM_SESSION_ID_POOL_START + self->blind_close_index * BCM_SESSION_ID_POOL_STRIDE;
  fp_dbg ("Session pool sweep: sending CLOSE for slot %u/%u (session_id=0x%08x)", self->blind_close_index + 1, BCM_SESSION_ID_POOL_COUNT, session_id);
  guint8 *buffer = bcm_build_close (session_id, &length, &command);
  send_command (self, buffer, length, command, open_blind_close_done);
}

/*
 * Shared callback for protocol OPEN inside a task SSM.
 * Parses session_id and advances to the next state.
 *
 * If the device returns 0x17 (session-ID pool exhausted), extracts any session_id from the response header,
 * sends CLOSE to end it, then restarts the SSM from CANCEL_INIT.
 * Falls back to pool sweep if no session_id can be extracted.
 */
static void
task_open_done (FpiDeviceBroadcom *self, GError *error)
{
  BcmMessageHeader header;
  guint32 status = BCM_STATUS_OK;
  BcmCommand command = {0};

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (!bcm_parse_header (self->response_buffer, self->response_length, &header))
    {
      fpi_ssm_mark_failed (self->task_ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse OPEN response header"));
      return;
    }

  error = check_response_status_from_header (self, &header, &status);
  if (error)
    {
      if (status == BCM_STATUS_SESSION_STUCK && self->open_retry_count < BCM_SESSION_OPEN_RETRIES)
        {
          gsize length;
          guint8 *buffer;

          self->open_retry_count++;

          /* Try to extract session_id from the error response and CLOSE it */
          if (bcm_parse_header (self->response_buffer, self->response_length, &header) && header.session_id != 0)
            {
              fp_warn ("OPEN failed (%s), closing stale session 0x%08x (retry %u/%u)...", error->message, header.session_id, self->open_retry_count,
                       BCM_SESSION_OPEN_RETRIES);
              g_clear_error (&error);

              buffer = bcm_build_close (header.session_id, &length, &command);
              send_command (self, buffer, length, command, open_stale_close_done);
              return;
            }

          /* No session_id in the 0x17 response; the pool is full.
           * Sweep all 20 firmware session slots and close whichever is stuck. */
          fp_warn ("OPEN failed (%s); session pool full (0x17). Sweeping all %u firmware session slots to close any stuck sessions (retry %u/%u)...", error->message,
                   BCM_SESSION_ID_POOL_COUNT, self->open_retry_count, BCM_SESSION_OPEN_RETRIES);
          g_clear_error (&error);

          self->blind_close_index = 0;
          fp_dbg ("Pool sweep: sending CLOSE for slot %u/%u (session_id=0x%08x)", 1u, BCM_SESSION_ID_POOL_COUNT, (guint32) BCM_SESSION_ID_POOL_START);
          buffer = bcm_build_close (BCM_SESSION_ID_POOL_START, &length, &command);
          send_command (self, buffer, length, command, open_blind_close_done);
          return;
        }
      self->open_retry_count = 0;
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  self->open_retry_count = 0;

  if (!bcm_validate_open_response (self->response_buffer, self->response_length, &header))
    {
      fpi_ssm_mark_failed (self->task_ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to validate OPEN response"));
      return;
    }

  self->session_id = header.session_id;
  fp_dbg ("Broadcom session opened: 0x%08x", self->session_id);
  session_cache_write (self, self->session_id);
  fpi_ssm_next_state (self->task_ssm);
}

/*
 * Shared callback for protocol CLOSE inside a task SSM.
 * If task_error is set (error-path close), marks the SSM failed with
 * that error; otherwise marks it completed.
 */
static void
task_close_done (FpiDeviceBroadcom *self, GError *error)
{
  // Any transport-level errors?
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fp_warn ("Protocol close failed: %s", error->message);
      /* CLOSE failed. Release the lock without deleting the cache file.
       * The unlocked file will be found as a stale session on the next
       * operation and closed then. */
      session_cache_release (self);
    }
  else
    {
      // We may still have protocol-level errors.
      error = check_response_status_from_buffer (self, NULL);
      if (error)
        {
          fp_warn ("Protocol close returned device error: %s", error->message);
          /* CLOSE failed. Release the lock without deleting the cache file.
           * The unlocked file will be found as a stale session on the next
           * operation and closed then. */
          session_cache_release (self);
        }
      else
        {
          fp_dbg ("Broadcom session closed: 0x%08x", self->session_id);
          session_cache_delete (self);
        }
    }
  g_clear_error (&error);
  self->session_id = 0;

  fp_dbg ("task_close_done: task_error=%s", self->task_error ? self->task_error->message : "NULL");
  if (self->task_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&self->task_error));
  else
    fpi_ssm_mark_completed (self->task_ssm);
}

/*
 * Callback for the init-cancel step: ignore all errors and just advance.
 * The device may return an error if there is nothing to cancel.
 */
static void
cancel_init_done (FpiDeviceBroadcom *self, GError *error)
{
  g_clear_error (&error);
  fpi_ssm_next_state (self->task_ssm);
}

/******************************************************************************
 *
 *  Finger-wait helper
 *
 *  Polls the interrupt endpoint until the device sends a status=3 finger
 *  event, then advances the task SSM.  The caller is responsible for
 *  sending the next command (ENROLL_UPDATE / CAPTURE) in the following state.
 *
 *****************************************************************************/
static void schedule_finger_poll (FpiDeviceBroadcom *self);

static void
wait_finger_interrupt_callback (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data G_GNUC_UNUSED, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  BcmInterrupt interrupt;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (transfer->actual_length < BCM_INTERRUPT_SIZE || !bcm_parse_interrupt (transfer->buffer, transfer->actual_length, &interrupt))
    {
      fpi_ssm_mark_failed (self->task_ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse finger-wait interrupt"));
      return;
    }

  fp_dbg ("Finger-wait interrupt: status=0x%08x", interrupt.status);

  if (interrupt.status == BCM_INTERRUPT_FINGER)
    {
      fp_dbg ("Finger detected; advancing to send command");
      fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  /* Any other interrupt while waiting for the finger: keep polling */
  schedule_finger_poll (self);
}

static void
schedule_finger_poll (FpiDeviceBroadcom *self)
{
  FpDevice *device = FP_DEVICE (self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (device);

  fpi_usb_transfer_fill_interrupt (transfer, BCM_EP_INTERRUPT, BCM_INTERRUPT_SIZE);
  fpi_usb_transfer_submit (transfer, BCM_FINGER_TIMEOUT, fpi_device_get_cancellable (device), wait_finger_interrupt_callback, NULL);
}

/*
 * Start polling the interrupt endpoint for a finger event.
 * Call from a task-SSM state handler; the SSM advances when the finger lands.
 */
static void
wait_finger (FpiDeviceBroadcom *self)
{
  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_NONE);
  schedule_finger_poll (self);
}

/******************************************************************************
 *
 *  Enroll
 *
 *****************************************************************************/
static void
enroll_command_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
enroll_challenge_done (FpiDeviceBroadcom *self, GError *error)
{
  guint32 status = 0;

  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  error = check_response_status_from_buffer (self, &status);
  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  if (!bcm_parse_challenge_response (self->response_buffer, self->response_length, self->challenge_hash))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse GET_CHALLENGE response"), BCM_ENROLL_CLOSE);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
enroll_update_done (FpiDeviceBroadcom *self, GError *error)
{
  guint32 status = 0;
  guint32 progress_state;
  guint8 hash[BCM_HASH_SIZE];

  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_PRESENT);

  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  error = check_response_status_from_buffer (self, &status);

  if (status == BCM_STATUS_RETRY_REMOVE || status == BCM_STATUS_RETRY_OFF_CENTER)
    {
      FpDeviceRetry retry_code = (status == BCM_STATUS_RETRY_OFF_CENTER) ? FP_DEVICE_RETRY_CENTER_FINGER : FP_DEVICE_RETRY_REMOVE_FINGER;
      fp_dbg ("Enroll update: retry requested (status=0x%02x)", status);
      g_clear_error (&error);
      fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, fpi_device_retry_new (retry_code));
      fpi_ssm_jump_to_state (self->task_ssm, BCM_ENROLL_GET_CHALLENGE);
      return;
    }

  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  if (!bcm_parse_enroll_update_response (self->response_buffer, self->response_length, &progress_state, hash))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse ENROLL_UPDATE response"), BCM_ENROLL_CLOSE);
      return;
    }

  /* Store the hash for the next command */
  memcpy (self->challenge_hash, hash, BCM_HASH_SIZE);

  self->enroll_stage++;
  fp_dbg ("Enroll update: stage %d complete, progress_state=0x%08x", self->enroll_stage, progress_state);
  fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);

  /* Check if enrollment is complete (low byte of progress_state = 0x01) */
  if ((progress_state & 0xFF) == 0x01)
    {
      fp_info ("Enrollment stages complete after %d stages", self->enroll_stage);
      fpi_ssm_jump_to_state (self->task_ssm, BCM_ENROLL_COMMIT);
      return;
    }

  /* More stages needed; loop back to GET_CHALLENGE */
  fpi_ssm_jump_to_state (self->task_ssm, BCM_ENROLL_GET_CHALLENGE);
}

static void
enroll_commit_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_ENROLL_CLOSE);
      return;
    }

  if (!bcm_parse_enroll_commit_response (self->response_buffer, self->response_length, &self->template_id))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse ENROLL_COMMIT response"), BCM_ENROLL_CLOSE);
      return;
    }

  fp_info ("Enrollment committed, template_id=0x%08x", self->template_id);
  fpi_ssm_next_state (self->task_ssm);
}

static void
enroll_ssm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  gsize length;
  guint8 *buffer;
  BcmCommand command = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case BCM_ENROLL_CLOSE_STALE:
      fp_dbg ("ENROLL - state %d: CLOSE_STALE", BCM_ENROLL_CLOSE_STALE);
      close_stale_state (self);
      break;

    case BCM_ENROLL_CANCEL_INIT:
      fp_dbg ("ENROLL - state %d: CANCEL_INIT", BCM_ENROLL_CANCEL_INIT);
      /* Send CANCEL_CAPTURE to clear any stale pending-capture state (0x85).
       * Ignore the response status. The device may return an error if there
       * is nothing to cancel. */
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, cancel_init_done);
      break;


    case BCM_ENROLL_OPEN:
      fp_dbg ("ENROLL - state %d: OPEN", BCM_ENROLL_OPEN);
      buffer = bcm_build_open (&length, &command);
      send_command (self, buffer, length, command, task_open_done);
      break;

    case BCM_ENROLL_START:
      fp_dbg ("ENROLL - state %d: START", BCM_ENROLL_START);
      buffer = bcm_build_start_enroll (&length, &command);
      send_command (self, buffer, length, command, enroll_command_done);
      break;

    case BCM_ENROLL_GET_CHALLENGE:
      fp_dbg ("ENROLL - state %d: GET_CHALLENGE", BCM_ENROLL_GET_CHALLENGE);
      buffer = bcm_build_get_challenge (self->session_id, &length, &command);
      send_command (self, buffer, length, command, enroll_challenge_done);
      break;

    case BCM_ENROLL_WAIT_FINGER:
      fp_dbg ("ENROLL - state %d: WAIT_FINGER", BCM_ENROLL_WAIT_FINGER);
      /* Poll interrupt endpoint; advance to BCM_ENROLL_UPDATE on finger event */
      wait_finger (self);
      break;

    case BCM_ENROLL_UPDATE:
      fp_dbg ("ENROLL - state %d: UPDATE", BCM_ENROLL_UPDATE);
      /* Finger is on sensor; send ENROLL_UPDATE. The device responds immediately. */
      buffer = bcm_build_enroll_update (self->session_id, self->challenge_hash, &length, &command);
      send_command (self, buffer, length, command, enroll_update_done);
      break;

    case BCM_ENROLL_COMMIT:
      fp_dbg ("ENROLL - state %d: COMMIT", BCM_ENROLL_COMMIT);
      buffer = bcm_build_enroll_commit (self->session_id, self->challenge_hash, &length, &command);
      send_command (self, buffer, length, command, enroll_commit_done);
      break;

    case BCM_ENROLL_CLOSE:
      fp_dbg ("ENROLL - state %d: CLOSE", BCM_ENROLL_CLOSE);
      buffer = bcm_build_close (self->session_id, &length, &command);
      send_command (self, buffer, length, command, task_close_done);
      break;
    }
}

static void
enroll_ssm_done (FpiSsm *ssm G_GNUC_UNUSED, FpDevice *device, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  FpPrint *print;
  g_autofree gchar *description = NULL;

  self->task_ssm = NULL;

  if (error)
    {
      fpi_device_enroll_complete (device, NULL, error);
      return;
    }

  /* Populate the FpPrint that fprintd passed in (preserves username etc.) */
  fpi_device_get_enroll_data (device, &print);
  GVariant *data = g_variant_new (variant_type_uint32, self->template_id);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, fpi_data_property_name, data, NULL);
  description = fpi_print_generate_user_id (print);
  g_object_set (print, "description", description, NULL);

  fp_info ("Enrollment complete, template_id=0x%08x", self->template_id);
  fpi_device_enroll_complete (device, g_object_ref (print), NULL);
}

/******************************************************************************
 *
 *  Verify
 *
 *****************************************************************************/
static void
verify_challenge_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    {
      task_error (self, error, BCM_VERIFY_CLOSE);
      return;
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_VERIFY_CLOSE);
      return;
    }

  if (!bcm_parse_challenge_response (self->response_buffer, self->response_length, self->challenge_hash))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse challenge"), BCM_VERIFY_CLOSE);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
verify_capture_done (FpiDeviceBroadcom *self, GError *error)
{
  guint32 match_result, template_id;
  BcmMessageHeader header;

  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_PRESENT);

  if (error)
    {
      task_error (self, error, BCM_VERIFY_CLOSE);
      return;
    }

  if (bcm_parse_header (self->response_buffer, self->response_length, &header))
    {
      /* 0x59 = template not on device; store no-match and continue to cleanup */
      if (header.status == BCM_STATUS_REQUESTED_TEMPLATE_NOT_FOUND)
        {
          fp_dbg ("CAPTURE: template 0x%08x not on device (0x59); reporting no-match", self->template_id);
          self->verify_match_result = 0;
          fpi_ssm_next_state (self->task_ssm);
          return;
        }

      /* Finger not read correctly; ask user to reposition and retry */
      if (header.status == BCM_STATUS_RETRY_REMOVE || header.status == BCM_STATUS_RETRY_OFF_CENTER)
        {
          const FpDeviceRetry retry_code = (header.status == BCM_STATUS_RETRY_OFF_CENTER) ? FP_DEVICE_RETRY_CENTER_FINGER : FP_DEVICE_RETRY_REMOVE_FINGER;
          fp_dbg ("CAPTURE: retry requested (status=0x%02x)", header.status);
          fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_NONE);
          task_error (self, fpi_device_retry_new (retry_code), BCM_VERIFY_CANCEL);
          return;
        }
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_VERIFY_CLOSE);
      return;
    }

  if (!bcm_parse_capture_response (self->response_buffer, self->response_length, &match_result, &template_id))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse CAPTURE response"), BCM_VERIFY_CLOSE);
      return;
    }

  fp_dbg ("Verify capture: match_result=%u, template_id=0x%08x", match_result, template_id);

  self->verify_match_result = match_result;
  fpi_ssm_next_state (self->task_ssm);
}

static void
verify_cancel_done (FpiDeviceBroadcom *self, GError *error)
{
  /* Ignore cancel errors */
  g_clear_error (&error);
  fpi_ssm_next_state (self->task_ssm);
}

static void
verify_ssm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  gsize length;
  guint8 *buffer;
  BcmCommand command = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case BCM_VERIFY_CLOSE_STALE:
      fp_dbg ("VERIFY - state %d: CLOSE_STALE", BCM_VERIFY_CLOSE_STALE);
      close_stale_state (self);
      break;

    case BCM_VERIFY_CANCEL_INIT:
      fp_dbg ("VERIFY - state %d: CANCEL_INIT", BCM_VERIFY_CANCEL_INIT);
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, cancel_init_done);
      break;


    case BCM_VERIFY_OPEN:
      fp_dbg ("VERIFY - state %d: OPEN", BCM_VERIFY_OPEN);
      buffer = bcm_build_open (&length, &command);
      send_command (self, buffer, length, command, task_open_done);
      break;

    case BCM_VERIFY_GET_CHALLENGE:
      fp_dbg ("VERIFY - state %d: GET_CHALLENGE", BCM_VERIFY_GET_CHALLENGE);
      {
        /* Extract template_id from the enrolled print up front */
        FpPrint *print = NULL;
        GVariant *data = NULL;

        fpi_device_get_verify_data (device, &print);
        g_assert (print != NULL);
        g_object_get (print, fpi_data_property_name, &data, NULL);

        if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE (variant_type_uint32)))
          {
            g_clear_pointer (&data, g_variant_unref);
            task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "Invalid print data"), BCM_VERIFY_CLOSE);
            return;
          }

        g_variant_get (data, variant_type_uint32, &self->template_id);
        g_variant_unref (data);

        buffer = bcm_build_get_challenge (self->session_id, &length, &command);
        send_command (self, buffer, length, command, verify_challenge_done);
      }
      break;

    case BCM_VERIFY_WAIT_FINGER:
      fp_dbg ("VERIFY - state %d: WAIT_FINGER", BCM_VERIFY_WAIT_FINGER);
      /* Poll interrupt endpoint; advance to BCM_VERIFY_CAPTURE on finger event */
      wait_finger (self);
      break;

    case BCM_VERIFY_CAPTURE:
      fp_dbg ("VERIFY - state %d: CAPTURE", BCM_VERIFY_CAPTURE);
      /* Finger is on sensor; send CAPTURE. The device responds immediately. */
      buffer = bcm_build_capture (self->session_id, &self->template_id, 1, &length, &command);
      send_command (self, buffer, length, command, verify_capture_done);
      break;

    case BCM_VERIFY_CANCEL:
      fp_dbg ("VERIFY - state %d: CANCEL", BCM_VERIFY_CANCEL);
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, verify_cancel_done);
      break;

    case BCM_VERIFY_CLOSE:
      fp_dbg ("VERIFY - state %d: CLOSE", BCM_VERIFY_CLOSE);
      buffer = bcm_build_close (self->session_id, &length, &command);
      send_command (self, buffer, length, command, task_close_done);
      break;
    }
}

static void
verify_ssm_done (FpiSsm *ssm G_GNUC_UNUSED, FpDevice *device, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->task_ssm = NULL;

  /* verify_match_result is initialized to -1 (unset) at the start of every verify
   * and is only written by verify_capture_done().  If the SSM completed without
   * error but the field is still -1, a success path bypassed the capture callback
   * entirely. Treat that as a protocol/state-machine error rather than silently
   * reporting a false no-match, which would be indistinguishable from a real
   * failed comparison. */
  if (!error && self->verify_match_result == -1)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Verify completed but match result was never set");

  if (!error)
    {
      FpPrint *print = NULL;
      fpi_device_get_verify_data (device, &print);
      /* The device returns match_result == 1 for a positive match; anything else
       * (0 = no-match, any unexpected value) is treated as a non-match. */
      if (self->verify_match_result == 1)
        fpi_device_verify_report (device, FPI_MATCH_SUCCESS, print, NULL);
      else
        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);
    }
  fpi_device_verify_complete (device, error);
}

/******************************************************************************
 *
 *  Identify: single CAPTURE with all N template IDs (on-device 1:N matching)
 *
 *  The driver sends one CAPTURE containing all enrolled template IDs
 *  (TLV[4]: tag=2, length=N*4, values=[tid_0..tid_N-1]).  The
 *  device returns match_result and the matched template_id.
 *
 *****************************************************************************/
static void
identify_challenge_done (FpiDeviceBroadcom *self, GError *error)
{
  if (error)
    {
      task_error (self, error, BCM_IDENTIFY_CLOSE);
      return;
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_IDENTIFY_CLOSE);
      return;
    }

  if (!bcm_parse_challenge_response (self->response_buffer, self->response_length, self->challenge_hash))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse challenge"), BCM_IDENTIFY_CLOSE);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
identify_capture_done (FpiDeviceBroadcom *self, GError *error)
{
  guint32 match_result, template_id;
  GPtrArray *templates = NULL;
  FpPrint *match = NULL;
  BcmMessageHeader header;

  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_PRESENT);

  if (error)
    {
      task_error (self, error, BCM_IDENTIFY_CLOSE);
      return;
    }

  /* 0x59 = none of the templates found on device; store no-match and clean up */
  if (bcm_parse_header (self->response_buffer, self->response_length, &header) && header.status == BCM_STATUS_REQUESTED_TEMPLATE_NOT_FOUND)
    {
      fp_dbg ("CAPTURE: gallery templates not on device (0x59); reporting no-match");
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_IDENTIFY_CLOSE);
      return;
    }

  if (!bcm_parse_capture_response (self->response_buffer, self->response_length, &match_result, &template_id))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse CAPTURE response"), BCM_IDENTIFY_CLOSE);
      return;
    }

  fp_dbg ("Identify capture: match_result=%u, matched_tid=0x%08x", match_result, template_id);

  if (match_result == 1)
    {
      /* Find the matching FpPrint by template_id returned from the device */
      fpi_device_get_identify_data (FP_DEVICE (self), &templates);
      for (guint i = 0; i < templates->len; i++)
        {
          FpPrint *print = g_ptr_array_index (templates, i);
          GVariant *data = NULL;
          guint32 print_template_id;

          g_object_get (print, fpi_data_property_name, &data, NULL);
          if (data && g_variant_is_of_type (data, G_VARIANT_TYPE (variant_type_uint32)))
            {
              g_variant_get (data, variant_type_uint32, &print_template_id);
              if (print_template_id == template_id)
                match = print;
            }
          g_clear_pointer (&data, g_variant_unref);
          if (match)
            break;
        }

      FpPrint *new_scan = fp_print_new (FP_DEVICE (self));
      GVariant *scan_data = g_variant_new (variant_type_uint32, template_id);
      fpi_print_set_type (new_scan, FPI_PRINT_RAW);
      fpi_print_set_device_stored (new_scan, FALSE);
      g_object_set (new_scan, fpi_data_property_name, scan_data, NULL);

      self->identify_match = match ? g_object_ref (match) : NULL;
      self->identify_scan = g_object_ref_sink (new_scan);
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
identify_cancel_done (FpiDeviceBroadcom *self, GError *error)
{
  g_clear_error (&error);
  fpi_ssm_next_state (self->task_ssm);
}

static void
identify_ssm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  gsize length;
  guint8 *buffer;
  BcmCommand command = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case BCM_IDENTIFY_CLOSE_STALE:
      fp_dbg ("IDENTIFY - state %d: CLOSE_STALE", BCM_IDENTIFY_CLOSE_STALE);
      close_stale_state (self);
      break;

    case BCM_IDENTIFY_CANCEL_INIT:
      fp_dbg ("IDENTIFY - state %d: CANCEL_INIT", BCM_IDENTIFY_CANCEL_INIT);
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, cancel_init_done);
      break;


    case BCM_IDENTIFY_OPEN:
      fp_dbg ("IDENTIFY - state %d: OPEN", BCM_IDENTIFY_OPEN);
      buffer = bcm_build_open (&length, &command);
      send_command (self, buffer, length, command, task_open_done);
      break;

    case BCM_IDENTIFY_GET_CHALLENGE:
      fp_dbg ("IDENTIFY - state %d: GET_CHALLENGE", BCM_IDENTIFY_GET_CHALLENGE);
      buffer = bcm_build_get_challenge (self->session_id, &length, &command);
      send_command (self, buffer, length, command, identify_challenge_done);
      break;

    case BCM_IDENTIFY_WAIT_FINGER:
      fp_dbg ("IDENTIFY - state %d: WAIT_FINGER", BCM_IDENTIFY_WAIT_FINGER);
      wait_finger (self);
      break;

    case BCM_IDENTIFY_CAPTURE:
      fp_dbg ("IDENTIFY - state %d: CAPTURE", BCM_IDENTIFY_CAPTURE);
      {
        GPtrArray *templates = NULL;
        g_autofree guint32 *template_ids = NULL;

        fpi_device_get_identify_data (device, &templates);
        const guint number_of_templates = templates ? templates->len : 0;

        if (number_of_templates == 0)
          {
            fpi_ssm_jump_to_state (ssm, BCM_IDENTIFY_CANCEL);
            return;
          }

        template_ids = g_new (guint32, number_of_templates);
        for (guint i = 0; i < number_of_templates; i++)
          {
            FpPrint *print = g_ptr_array_index (templates, i);
            GVariant *data = NULL;
            guint32 template_id;

            g_object_get (print, fpi_data_property_name, &data, NULL);
            if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE (variant_type_uint32)))
              {
                g_clear_pointer (&data, g_variant_unref);
                task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "Invalid print data in gallery"), BCM_IDENTIFY_CLOSE);
                return;
              }
            g_variant_get (data, variant_type_uint32, &template_id);
            g_variant_unref (data);
            template_ids[i] = template_id;
          }

        buffer = bcm_build_capture (self->session_id, template_ids, number_of_templates, &length, &command);
        send_command (self, buffer, length, command, identify_capture_done);
      }
      break;

    case BCM_IDENTIFY_CANCEL:
      fp_dbg ("IDENTIFY - state %d: CANCEL", BCM_IDENTIFY_CANCEL);
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, identify_cancel_done);
      break;

    case BCM_IDENTIFY_CLOSE:
      fp_dbg ("IDENTIFY - state %d: CLOSE", BCM_IDENTIFY_CLOSE);
      buffer = bcm_build_close (self->session_id, &length, &command);
      send_command (self, buffer, length, command, task_close_done);
      break;
    }
}

static void
identify_ssm_done (FpiSsm *ssm G_GNUC_UNUSED, FpDevice *device, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->task_ssm = NULL;
  if (!error)
    /* On no-match both identify_match and identify_scan are NULL, which is
     * intentional: the device returns template_id=0 on a non-match so there is no
     * meaningful scan print to report. */
    fpi_device_identify_report (device, self->identify_match, self->identify_scan, NULL);
  g_clear_object (&self->identify_match);
  g_clear_object (&self->identify_scan);
  fpi_device_identify_complete (device, error);
}

/******************************************************************************
 *
 *  Delete
 *
 *****************************************************************************/
static void
delete_done (FpiDeviceBroadcom *self, GError *error)
{
  BcmMessageHeader header;

  if (error)
    {
      task_error (self, error, BCM_DELETE_CLOSE);
      return;
    }

  if (!bcm_parse_header (self->response_buffer, self->response_length, &header))
    {
      task_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse response header"), BCM_DELETE_CLOSE);
      return;
    }

  /* 0x1b = template not on device; idempotent success */
  if (header.status == BCM_STATUS_DELETE_TEMPLATE_NOT_FOUND)
    {
      fp_info ("DELETE_OBJECT: template 0x%08x not on device (0x1b); treating as success", self->template_id);
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  error = check_response_status_from_buffer (self, NULL);
  if (error)
    {
      task_error (self, error, BCM_DELETE_CLOSE);
      return;
    }

  fp_info ("DELETE_OBJECT: template 0x%08x deleted from device", self->template_id);
  fpi_ssm_next_state (self->task_ssm);
}

static void
delete_ssm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  gsize length;
  guint8 *buffer;
  BcmCommand command = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case BCM_DELETE_CLOSE_STALE:
      fp_dbg ("DELETE - state %d: CLOSE_STALE", BCM_DELETE_CLOSE_STALE);
      close_stale_state (self);
      break;

    case BCM_DELETE_CANCEL_INIT:
      fp_dbg ("DELETE - state %d: CANCEL_INIT", BCM_DELETE_CANCEL_INIT);
      buffer = bcm_build_cancel_capture (&length, &command);
      send_command (self, buffer, length, command, cancel_init_done);
      break;

    case BCM_DELETE_OPEN:
      fp_dbg ("DELETE - state %d: OPEN", BCM_DELETE_OPEN);
      buffer = bcm_build_open (&length, &command);
      send_command (self, buffer, length, command, task_open_done);
      break;

    case BCM_DELETE_DELETE:
      fp_dbg ("DELETE - state %d: DELETE", BCM_DELETE_DELETE);
      buffer = bcm_build_delete_object (self->session_id, self->template_id, &length, &command);
      send_command (self, buffer, length, command, delete_done);
      break;

    case BCM_DELETE_CLOSE:
      fp_dbg ("DELETE - state %d: CLOSE", BCM_DELETE_CLOSE);
      buffer = bcm_build_close (self->session_id, &length, &command);
      send_command (self, buffer, length, command, task_close_done);
      break;
    }
}

static void
delete_ssm_done (FpiSsm *ssm G_GNUC_UNUSED, FpDevice *device, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  self->task_ssm = NULL;
  if (!error)
    fp_info ("delete: template 0x%08x successfully removed", self->template_id);
  fpi_device_delete_complete (device, error);
}

/******************************************************************************
 *
 *  The device has a tendency to leave hanging sessions. Especially if quick
 *  or parallel operations happen. When that happens, any new session openings
 *  fail with error 0x17.
 *  However, if the session id is stored in cache, it can be closed post-crash
 *  and device operations can resume.
 *
 *  Session cache: persist session_id to /var/lib/fprint/session-cache so we
 *  can CLOSE stale sessions after a crash.
 *
 *  Each session gets a unique file: broadcom-{pid}-{session_id}.session.
 *  An exclusive flock is held on the file for the lifetime of the session.
 *  Before each operation opens a new session, the directory is scanned for
 *  files matching this device's PID.
 *  For each: LOCK_EX|LOCK_NB succeeds -> session is stale, close it;
 *            LOCK_EX|LOCK_NB fails (EWOULDBLOCK) -> active session, skip.
 *  The OS releases the lock automatically on process death, so crash recovery
 *  works without any explicit cleanup. Parallel sessions each have their own
 *  file and do not interfere with each other.
 *
 *  If for whatever reason the session cache fails, the device also seems to work
 *  on a fixed pool of session IDs. So if we do receive a 0x17 error after all,
 *  we attempt to recover by closing sessions of known session IDs.
 *
 *****************************************************************************/
static const gchar *
session_cache_dir (void)
{
  const gchar *cache_dir = g_getenv (session_cache_environment_variable);

  return cache_dir ? cache_dir : session_cache_default_directory;
}

static gchar *
session_cache_path (FpiDeviceBroadcom *self, guint32 session_id)
{
  GUsbDevice *usb_device = fpi_device_get_usb_device (FP_DEVICE (self));
  const guint16 pid = g_usb_device_get_pid (usb_device);

  return g_strdup_printf (session_cache_path_format, session_cache_dir (), pid, session_id);
}

static void
session_cache_write (FpiDeviceBroadcom *self, guint32 session_id)
{
  g_autofree gchar *path = session_cache_path (self, session_id);
  char contents[9];

  if (g_mkdir_with_parents (session_cache_dir (), 0755) != 0)
    {
      fp_warn ("Failed to create session cache directory %s: %s", session_cache_dir (), g_strerror (errno));
      return;
    }

  /* Release previous session's lock before taking a new one. */
  if (self->cache_fd != -1)
    {
      close (self->cache_fd);
      self->cache_fd = -1;
    }

  const int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      fp_warn ("Failed to write session cache to %s: %s", path, g_strerror (errno));
      return;
    }

  if (flock (fd, LOCK_EX) != 0)
    {
      fp_warn ("Failed to write session cache to %s: %s", path, g_strerror (errno));
      close (fd);
      return;
    }

  g_snprintf (contents, sizeof (contents), "%08x", session_id);
  if (write (fd, contents, 8) != 8)
    {
      fp_warn ("Failed to write session cache to %s: %s", path, g_strerror (errno));
      close (fd);
      return;
    }

  self->cache_fd = fd;
  fp_dbg ("Cached session 0x%08x to %s", session_id, path);
}

/* Scan the cache directory for a stale session belonging to this device.
 * Returns the first unlocked session_id found, or 0 if none. */
static guint32
session_cache_read (FpiDeviceBroadcom *self)
{
  GUsbDevice *usb_device = fpi_device_get_usb_device (FP_DEVICE (self));
  const guint16 pid = g_usb_device_get_pid (usb_device);
  g_autofree gchar *prefix = g_strdup_printf ("broadcom-%04x-", pid);
  const gchar *dir_path = session_cache_dir ();

  g_autoptr(GDir) dir = NULL;
  const gchar *name;

  dir = g_dir_open (dir_path, 0, NULL);
  if (!dir)
    return 0;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      guint32 session_id;

      if (!g_str_has_prefix (name, prefix) || !g_str_has_suffix (name, ".session"))
        continue;

      /* Require exactly 8 hex digits ending precisely at ".session". */
      {
        const gchar *hex_start = name + strlen (prefix);
        gchar *end;

        errno = 0;
        const gulong value = strtoul (hex_start, &end, 16);
        if (end != hex_start + 8 || strcmp (end, ".session") != 0 ||
            value == 0 || value > G_MAXUINT32 || errno != 0)
          continue;
        session_id = (guint32) value;
      }

      g_autofree gchar *path = g_strdup_printf ("%s/%s", dir_path, name);

      const int fd = open (path, O_RDONLY);
      if (fd < 0)
        continue;

      if (flock (fd, LOCK_EX | LOCK_NB) != 0)
        {
          /* Active session - do not disturb */
          close (fd);
          continue;
        }

      close (fd);
      return session_id;
    }

  return 0;
}

static void
session_cache_delete (FpiDeviceBroadcom *self)
{
  g_autofree gchar *path = session_cache_path (self, self->session_id);

  if (g_unlink (path) == 0)
    fp_dbg ("Deleted session cache %s", path);

  if (self->cache_fd != -1)
    {
      close (self->cache_fd);
      self->cache_fd = -1;
    }
}

/* Release the session cache lock without deleting the file.
 * Used when CLOSE fails: the unlocked file will be treated as a stale session
 * on the next startup, allowing recovery to be retried. */
static void
session_cache_release (FpiDeviceBroadcom *self)
{
  if (self->cache_fd != -1)
    {
      close (self->cache_fd);
      self->cache_fd = -1;
    }
}

/******************************************************************************
 *
 *  Command infrastructure: send -> interrupt -> receive
 *
 *****************************************************************************/
/*
 * The interrupt read callback needs to stash the data before the
 * generic fpi_ssm_usb_transfer_cb advances the state.
 * We override the callback for the interrupt state.
 */
static void
interrupt_callback (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data G_GNUC_UNUSED, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length < BCM_INTERRUPT_SIZE)
    {
      fpi_ssm_mark_failed (transfer->ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Short interrupt transfer"));
      return;
    }

  memcpy (self->interrupt_buf, transfer->buffer, BCM_INTERRUPT_SIZE);

  fpi_ssm_next_state (transfer->ssm);
}

/*
 * Bulk IN callback: stash response data.
 */
static void
receive_callback (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data G_GNUC_UNUSED, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  gsize bytes_to_copy = MIN ((gsize) transfer->actual_length, BCM_MAX_RESPONSE);
  memcpy (self->response_buffer, transfer->buffer, bytes_to_copy);
  self->response_length = bytes_to_copy;

  fp_dbg ("Received response: %zu bytes", bytes_to_copy);
  fpi_ssm_mark_completed (transfer->ssm);
}

/*
 * Command SSM handler: send -> wait interrupt -> receive response.
 */
static void
command_ssm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case BCM_COMMAND_SEND:
      fp_dbg ("CMD - state %d: BCM_COMMAND_SEND [%s (0x%02x), %zu bytes]", BCM_COMMAND_SEND, self->command.name, self->command.id, self->command_length);
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk_full (transfer, BCM_EP_OUT, g_steal_pointer (&self->command_buffer), self->command_length, g_free);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, BCM_COMMAND_TIMEOUT, fpi_device_get_cancellable (device), fpi_ssm_usb_transfer_cb, NULL);
      break;

    case BCM_COMMAND_WAIT_INTERRUPT:
      fp_dbg ("CMD - state %d: BCM_COMMAND_WAIT_INTERRUPT [%s (0x%02x)]", BCM_COMMAND_WAIT_INTERRUPT, self->command.name, self->command.id);
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_interrupt (transfer, BCM_EP_INTERRUPT, BCM_INTERRUPT_SIZE);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, BCM_DATA_TIMEOUT, fpi_device_get_cancellable (device), interrupt_callback, NULL);
      break;

    case BCM_COMMAND_RECEIVE:
      fp_dbg ("CMD - state %d: BCM_COMMAND_RECEIVE [%s (0x%02x)]", BCM_COMMAND_RECEIVE, self->command.name, self->command.id);
      {
        BcmInterrupt interrupt;

        if (!bcm_parse_interrupt (self->interrupt_buf, BCM_INTERRUPT_SIZE, &interrupt))
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse interrupt"));
            return;
          }

        fp_dbg ("Interrupt: status=0x%08x, response_len=%u", interrupt.status, interrupt.response_length);

        if (interrupt.status != BCM_INTERRUPT_READY)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Unexpected interrupt status 0x%08x", interrupt.status));
            return;
          }

        if (interrupt.response_length == 0 || interrupt.response_length > BCM_MAX_RESPONSE)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Invalid response length %u", interrupt.response_length));
            return;
          }

        self->response_length = interrupt.response_length;
        transfer = fpi_usb_transfer_new (device);
        fpi_usb_transfer_fill_bulk (transfer, BCM_EP_IN, self->response_length);
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, BCM_DATA_TIMEOUT, fpi_device_get_cancellable (device), receive_callback, NULL);
      }
      break;
    }
}

static void
command_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (device);
  const BcmCommandCallback callback = self->command_callback;

  self->command_ssm = NULL;
  self->command_callback = NULL;

  if (callback)
    callback (self, error);
  else
    g_clear_error (&error);
}

/*
 * Send a command and call the callback when the full
 * send -> interrupt -> receive cycle completes.
 */
static void
send_command (FpiDeviceBroadcom *self, guint8 *buffer, const gsize length, const BcmCommand command, const BcmCommandCallback callback)
{
  g_return_if_fail (command.name != NULL);
  g_return_if_fail (command.id != 0);

  FpDevice *device = FP_DEVICE (self);

  // Guard against forbidden commands that crash the device firmware.
  if (command.id == BCM_COMMAND_ENUMERATE_OBJECTS_DIRECT)
    {
      GError *error;

      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "BUG: attempt to send forbidden command 0x%02x (ENUMERATE_OBJECTS_DIRECT); would crash device firmware", command.id);
      fp_err ("%s", error->message);
      g_free (buffer);

      if (self->task_ssm)
        fpi_ssm_mark_failed (self->task_ssm, error);
      else
        g_clear_error (&error);
      return;
    }

  self->command_buffer = buffer;
  self->command_length = length;
  self->command = command;
  self->command_callback = callback;

  self->command_ssm = fpi_ssm_new_full (device, command_ssm_run_state, BCM_COMMAND_NUM_STATES, BCM_COMMAND_NUM_STATES, "CMD");
  fpi_ssm_start (self->command_ssm, command_ssm_done);
}

/*
 * Helper: check response header status and return error if non-zero.
 * Returns NULL on success, or a GError on failure.
 */
static GError *
check_response_status_from_header (const FpiDeviceBroadcom *self, const BcmMessageHeader *header, guint32 *status_out)
{
  if (status_out)
    *status_out = header->status;

  if (header->status != BCM_STATUS_OK && header->status != BCM_STATUS_RETRY_REMOVE && header->status != BCM_STATUS_RETRY_OFF_CENTER)
    {
      // Dump response buffer to log in case of error:
      fp_warn ("Error response for command 0x%02x (status=0x%08x), header dump:", header->command_id, header->status);
      for (gsize i = 0; i < BCM_MESSAGE_HEADER_SIZE && i < self->response_length; i += 4)
        {
          fp_warn ("  [%02zu] %02x %02x %02x %02x", i,
                   self->response_buffer[i],
                   (i + 1 < self->response_length) ? self->response_buffer[i + 1] : 0,
                   (i + 2 < self->response_length) ? self->response_buffer[i + 2] : 0,
                   (i + 3 < self->response_length) ? self->response_buffer[i + 3] : 0
                  );
        }
      if (self->response_length > BCM_MESSAGE_HEADER_SIZE)
        {
          fp_warn ("  TLV data (%zu bytes):", self->response_length - BCM_MESSAGE_HEADER_SIZE);
          for (gsize i = BCM_MESSAGE_HEADER_SIZE; i < self->response_length; i += 4)
            {
              fp_warn ("  [%02zu] %02x %02x %02x %02x", i, self->response_buffer[i], (i + 1 < self->response_length) ? self->response_buffer[i + 1] : 0,
                       (i + 2 < self->response_length) ? self->response_buffer[i + 2] : 0,
                       (i + 3 < self->response_length) ? self->response_buffer[i + 3] : 0);
            }
        }
      return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Device returned error 0x%08x for command 0x%02x", header->status, header->command_id);
    }

  return NULL;
}

static GError *
check_response_status_from_buffer (const FpiDeviceBroadcom *self, guint32 *status_out)
{
  BcmMessageHeader header;

  if (!bcm_parse_header (self->response_buffer, self->response_length, &header))
    return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Failed to parse response header");

  return check_response_status_from_header (self, &header, status_out);
}

/******************************************************************************
 *
 *  GObject boilerplate
 *
 *****************************************************************************/
static void
fpi_device_broadcom_init (FpiDeviceBroadcom *self)
{
  self->cache_fd = -1;
}

static void
fpi_device_broadcom_finalize (GObject *object)
{
  FpiDeviceBroadcom *self = FPI_DEVICE_BROADCOM (object);

  if (self->cache_fd != -1)
    {
      close (self->cache_fd);
      self->cache_fd = -1;
    }

  G_OBJECT_CLASS (fpi_device_broadcom_parent_class)->finalize (object);
}

static void
fpi_device_broadcom_class_init (FpiDeviceBroadcomClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_broadcom_finalize;

  device_class->id = broadcom_id;
  device_class->full_name = broadcom_full_name;
  device_class->type = FP_DEVICE_TYPE_USB;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;
  device_class->id_table = id_table;
  device_class->nr_enroll_stages = BCM_ENROLL_STAGES;

  device_class->probe = probe;
  device_class->open = device_open;
  device_class->close = device_close;
  device_class->enroll = enroll;
  device_class->verify = verify;
  device_class->identify = identify;
  device_class->delete = delete_fingerprint;

  fpi_device_class_auto_initialize_features (device_class);
  // We support on-device delete (STORAGE_DELETE is auto-set) but cannot list on-device templates (ENUMERATE_OBJECTS requires MGMT mode).
  // Manually set STORAGE so fprintd calls our delete vfunc during DeleteEnrolledFinger, WITHOUT setting STORAGE_LIST which would trigger
  // post-verify reconciliation that deletes every locally-stored print not returned by list().
  device_class->features |= FP_DEVICE_FEATURE_STORAGE;
}
