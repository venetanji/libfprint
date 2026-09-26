/*
 * Main driver for Validity/Synaptics VCSFW fingerprint sensors
 *
 * Copyright (C) 2024 libfprint contributors
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

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity_pack.h"
#include "validity.h"
#include "validity_data.h"
#include "validity_fwext.h"
#include "validity_pair.h"
#include "validity_tls.h"
#include "vcsfw_protocol.h"

G_DEFINE_TYPE (FpiDeviceValidity, fpi_device_validity, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x138A, .pid = 0x0090, .driver_data = VALIDITY_DEV_90 },
  { .vid = 0x138A, .pid = 0x0097, .driver_data = VALIDITY_DEV_97 },
  { .vid = 0x06CB, .pid = 0x009A, .driver_data = VALIDITY_DEV_9A },
  { .vid = 0x138A, .pid = 0x009D, .driver_data = VALIDITY_DEV_9D },
  { .vid = 0x138A, .pid = 0x00AB, .driver_data = VALIDITY_DEV_AB },
  { .vid = 0x06CB, .pid = 0x00CB, .driver_data = VALIDITY_DEV_CB },
  { .vid = 0x06CB, .pid = 0x00B7, .driver_data = VALIDITY_DEV_B7 },
  { .vid = 0x06CB, .pid = 0x00D2, .driver_data = VALIDITY_DEV_D2 },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

/* Map a USB "no device" error -- the sensor dropped off the bus after one of its
 * intermittent re-enumerations -- to FP_DEVICE_ERROR_REMOVED, so fprintd treats
 * it as a device removal (re-detecting the re-enumerated device and retrying)
 * rather than a generic protocol failure. Takes ownership of @error and returns
 * the (possibly replaced) error. */
GError *
validity_map_removed_error (GError *error)
{
  if (error &&
      g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE))
    {
      fp_info ("Device dropped off the bus (re-enumeration) -- reporting as removed");
      g_error_free (error);
      return fpi_device_error_new (FP_DEVICE_ERROR_REMOVED);
    }
  return error;
}

/* Classify a transport/protocol error into a recovery class. See the header.
 * This only maps the signals distinguishable today; the TLS-alert ->
 * SESSION_LOST refinement lands with the recovery wiring, so a chip-reset TLS
 * alert currently classifies as FATAL (aborts, as before). */
ValidityErrClass
validity_classify_error (const GError *error,
                         gboolean      device_alive)
{
  if (error == NULL)
    return VALIDITY_ERR_NONE;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_CANCELLED))
    return VALIDITY_ERR_CANCELLED;

  if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED))
    return VALIDITY_ERR_REMOVED;

  if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE))
    return device_alive ? VALIDITY_ERR_SESSION_LOST : VALIDITY_ERR_REMOVED;

  /* Transient transport blips where the device is still present on the bus. */
  if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_IO) ||
      g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
    return VALIDITY_ERR_PIPE_STALL;

  return VALIDITY_ERR_FATAL;
}

static void validity_release_state (FpiDeviceValidity *self);

/* ================================================================
 * Probe
 * ================================================================
 *
 * Probe is done synchronously (like synaptics driver):
 *   1) Open USB device
 *   2) Send GET_VERSION (cmd 0x01)
 *   3) Read response, parse firmware info
 *   4) Close USB device
 *   5) Report probe complete
 */

static void
dev_probe (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  GUsbDevice *usb_dev;

  g_autoptr(FpiUsbTransfer) transfer = NULL;
  GError *error = NULL;
  guint16 status;
  g_autofree gchar *serial = NULL;

  G_DEBUG_HERE ();

  self->dev_type = fpi_device_get_driver_data (device);

  usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL,
                                 validity_map_removed_error (error));
      return;
    }

  if (!g_usb_device_reset (usb_dev, &error))
    {
      fp_dbg ("USB reset failed: %s", error->message);
      goto err_close;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    goto err_close;

  /* Send GET_VERSION (cmd 0x01) */
  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT,
                              VALIDITY_USB_SEND_HEADER_LEN);
  transfer->short_is_error = TRUE;
  validity_pack (transfer->buffer, VALIDITY_USB_SEND_HEADER_LEN,
                 "b", VCSFW_CMD_GET_VERSION);
  if (!fpi_usb_transfer_submit_sync (transfer, VALIDITY_USB_TIMEOUT, &error))
    goto err_close;

  /* Read response */
  g_clear_pointer (&transfer, fpi_usb_transfer_unref);
  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  if (!fpi_usb_transfer_submit_sync (transfer, VALIDITY_USB_TIMEOUT, &error))
    goto err_close;

  /* Parse status */
  if (!validity_unpack (transfer->buffer, transfer->actual_length, "h", &status))
    {
      g_warning ("GET_VERSION response too short");
      error = fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
      goto err_close;
    }

  if (status != VCSFW_STATUS_OK)
    {
      g_warning ("GET_VERSION returned error status: 0x%04x", status);
      error = fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
      goto err_close;
    }

  /* Parse version info (data after the 2-byte status) */
  if (!vcsfw_parse_version (transfer->buffer + 2,
                            transfer->actual_length - 2,
                            &self->version_info))
    {
      g_warning ("Failed to parse GET_VERSION response");
      error = fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
      goto err_close;
    }

  fp_dbg ("Validity sensor firmware:");
  fp_dbg ("  Version: %d.%d",
          self->version_info.version_major,
          self->version_info.version_minor);
  fp_dbg ("  Product: %d", self->version_info.product);
  fp_dbg ("  Build Num: %d", self->version_info.build_num);
  fp_dbg ("  Build Time: %u", self->version_info.build_time);

  /* Build a serial string from the serial number bytes */
  serial = g_strdup_printf ("%02x%02x%02x%02x%02x%02x",
                            self->version_info.serial_number[0],
                            self->version_info.serial_number[1],
                            self->version_info.serial_number[2],
                            self->version_info.serial_number[3],
                            self->version_info.serial_number[4],
                            self->version_info.serial_number[5]);

  g_usb_device_release_interface (usb_dev, 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);

  fpi_device_probe_complete (device, serial, NULL, NULL);
  return;

err_close:
  g_usb_device_release_interface (usb_dev, 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);
  fpi_device_probe_complete (device, NULL, NULL, error);
}

/* ================================================================
 * Open
 * ================================================================
 *
 * Open claims the USB interface and sends the init sequence:
 *   1) GET_VERSION (0x01)
 *   2) UNKNOWN_INIT (0x19)
 *   3) GET_FW_INFO (0x43 0x02) — check if fwext loaded
 *   4) Send init_hardcoded blob (per-device, via HAL)
 *   5) If no fwext: send init_hardcoded_clean_slate blob
 *   6) Pairing check
 *   7) TLS handshake (works without fwext — uses partition 1 keys)
 *   8) Upload firmware extension via TLS (if not loaded)
 *
 * Open fails if no TLS session was established, except in emulation:
 * without one the device cannot enroll, verify, list or delete. It also
 * fails if sensor calibration fails; emulation never gets that far, as it
 * skips TLS and jumps to OPEN_DONE.
 */

typedef enum {
  OPEN_GET_VERSION = 0,
  OPEN_RECV_VERSION,
  OPEN_SEND_CMD19,
  OPEN_RECV_CMD19,
  OPEN_SEND_GET_FW_INFO,
  OPEN_RECV_GET_FW_INFO,
  OPEN_LOAD_DATA,
  OPEN_SEND_INIT_HARDCODED,
  OPEN_RECV_INIT_HARDCODED,
  OPEN_SEND_INIT_CLEAN_SLATE,
  OPEN_RECV_INIT_CLEAN_SLATE,
  OPEN_PAIR,
  OPEN_TLS_READ_FLASH,
  OPEN_TLS_DERIVE_PSK,
  OPEN_TLS_HANDSHAKE,
  OPEN_UPLOAD_FWEXT,
  OPEN_SENSOR_IDENTIFY,
  OPEN_SENSOR_IDENTIFY_RECV,
  OPEN_SENSOR_FACTORY_BITS,
  OPEN_SENSOR_FACTORY_BITS_RECV,
  OPEN_CAPTURE_SETUP,
  OPEN_CALIBRATE_BUILD,
  OPEN_CALIBRATE_SEND,
  OPEN_CALIBRATE_SEND_RECV,
  OPEN_CALIBRATE_READ_DATA,
  OPEN_CALIBRATE_LOOP,
  OPEN_DONE,
  OPEN_NUM_STATES,
} ValidityOpenSsmState;

/* Track whether fw extension is loaded */
typedef struct
{
  gboolean fwext_loaded;
} OpenSsmData;

/* Callback for GET_FW_INFO response — check if fwext is loaded */
static void
fw_info_recv_cb (FpiUsbTransfer *transfer,
                 FpDevice       *device,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* GET_FW_INFO response: first 2 bytes are status.
   * 0x0000 = fwext loaded, 0xb004 = no fwext, others = error */
  if (transfer->actual_length >= 2)
    {
      guint16 status = 0;
      validity_unpack (transfer->buffer, transfer->actual_length, "h", &status);
      if (status == VCSFW_STATUS_OK)
        {
          self->fwext_loaded = TRUE;
          fp_info ("Firmware extension is loaded");
        }
      else
        {
          self->fwext_loaded = FALSE;
          fp_info ("Firmware extension not loaded (status=0x%04x)", status);
        }
    }
  else
    {
      self->fwext_loaded = FALSE;
      fp_warn ("GET_FW_INFO response too short");
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* Callback for optional flash-read child SSM */
static void
flash_read_ssm_done (FpiSsm   *ssm,
                     FpDevice *dev,
                     GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      /* No keys means no TLS session, and every enroll, verify, list and
       * delete runs over TLS: fail the open rather than report a device
       * that cannot do anything. */
      fp_warn ("TLS flash read failed: %s", error->message);
      g_prefix_error (&error, "Could not read the sensor's TLS keys: ");
      fpi_ssm_mark_failed (self->open_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->open_ssm);
}

/* Callback for fwext upload child SSM */
static void
fwext_upload_ssm_done (FpiSsm   *ssm,
                       FpDevice *dev,
                       GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      fp_warn ("Firmware extension upload failed: %s", error->message);
      /* Non-fatal: skip only the fwext step and continue with sensor
       * identification and calibration -- jumping all the way to OPEN_DONE
       * would skip OPEN_SENSOR_IDENTIFY, leaving self->sensor.type_info unset
       * and asserting later in validity_capture_build_cmd_02(). TLS is
       * already up here: OPEN_TLS_HANDSHAKE runs before this state, the
       * upload only starts with a session, and its first two commands (the
       * HW register write/read) go over TLS before it looks for the file.
       * This path is unconditional for devices whose .xpfwext is not
       * distributed (e.g. 06cb:009a reports no fwext loaded on every open),
       * and enroll and verify were reported working there, over the session
       * set up by OPEN_TLS_HANDSHAKE. */
      g_clear_error (&error);
      fpi_ssm_jump_to_state (self->open_ssm, OPEN_SENSOR_IDENTIFY);
      return;
    }

  /* After successful upload and reboot, the device will re-enumerate
   * on USB. We cannot continue the open sequence — report an error
   * that tells fprintd to retry. */
  fp_info ("Firmware extension uploaded successfully — device rebooting");
  fpi_ssm_mark_failed (self->open_ssm,
                       fpi_device_error_new_msg (FP_DEVICE_ERROR_REMOVED,
                                                 "Device rebooting after firmware upload"));
}

/* Callback for pairing child SSM */
static void
pair_ssm_done (FpiSsm   *ssm,
               FpDevice *dev,
               GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      /* The pair SSM refused: partition 1 of a partitioned sensor is
       * blank, partial or unreadable, partition_flash returned 0x0404, or
       * a reconnect would need pairing. Its error says why; fail the open
       * with it rather than carry on with a sensor we cannot talk to. */
      if (self->pair_state.refused)
        {
          fp_warn ("Pairing refused: %s", error->message);
          validity_pair_state_free (&self->pair_state);
          fpi_ssm_mark_failed (self->open_ssm, error);
          return;
        }

      /* After reboot, USB transfers fail — this is expected */
      if (self->pair_state.reboot_pending)
        {
          fp_info ("Device rebooting after pairing (USB error expected)");
          g_clear_error (&error);
        }
      else if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED))
        {
          fp_info ("Device rebooting after pairing");
          fpi_ssm_mark_failed (self->open_ssm, error);
          return;
        }
      else
        {
          /* Not a refusal (e.g. GET_FLASH_INFO failed). Carry on to the TLS
           * key read: if no session can be established, the open fails
           * there or in OPEN_DONE. */
          fp_warn ("Pairing failed: %s -- continuing to the TLS key read",
                   error->message);
          g_clear_error (&error);
        }
    }

  /* Check if pairing caused a reboot */
  if (self->pair_state.priv_blob != NULL ||
      self->pair_state.reboot_pending)
    {
      /* Pairing was performed and device is rebooting.
       * Signal to fprintd to retry the open. */
      fp_info ("Pairing complete — device rebooting, signalling removal");
      validity_pair_state_free (&self->pair_state);
      fpi_ssm_mark_failed (self->open_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_REMOVED,
                                                     "Device rebooting after pairing"));
      return;
    }

  /* Keys present (pairing not needed), or a pairing error that was not a
   * refusal -- continue */
  validity_pair_state_free (&self->pair_state);
  fpi_ssm_next_state (self->open_ssm);
}

/* Callback for optional TLS handshake child SSM */
static void
tls_handshake_ssm_done (FpiSsm   *ssm,
                        FpDevice *dev,
                        GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      /* Fail here rather than rely on the OPEN_DONE check: secure_rx is set
       * when the server's ChangeCipherSpec arrives, before its Finished is
       * verified, so a handshake that failed late can leave it TRUE. */
      fp_warn ("TLS handshake failed: %s", error->message);
      g_prefix_error (&error, "TLS handshake with the sensor failed: ");
      fpi_ssm_mark_failed (self->open_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->open_ssm);
}

/* Callback for calibration EP 0x82 bulk read — saves data for processing */
static void
calib_bulk_read_cb (FpiUsbTransfer *transfer,
                    FpDevice       *dev,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Save the raw calibration data for processing in the next state */
  g_clear_pointer (&self->bulk_data, g_free);
  self->bulk_data = g_memdup2 (transfer->buffer, transfer->actual_length);
  self->bulk_data_len = transfer->actual_length;

  fpi_ssm_next_state (transfer->ssm);
}

static void
open_get_version (FpiSsm            *ssm,
                  FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT,
                              VALIDITY_USB_SEND_HEADER_LEN);
  validity_pack (transfer->buffer, VALIDITY_USB_SEND_HEADER_LEN,
                 "b", VCSFW_CMD_GET_VERSION);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_recv_version (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_send_cmd19 (FpiSsm            *ssm,
                 FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT,
                              VALIDITY_USB_SEND_HEADER_LEN);
  validity_pack (transfer->buffer, VALIDITY_USB_SEND_HEADER_LEN,
                 "b", VCSFW_CMD_UNKNOWN_INIT);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_recv_cmd19 (FpiSsm            *ssm,
                 FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_send_get_fw_info (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  guint8 cmd[] = { VCSFW_CMD_GET_FW_INFO, 0x02 };

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT,
                              sizeof (cmd));
  memcpy (transfer->buffer, cmd, sizeof (cmd));
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_recv_get_fw_info (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fw_info_recv_cb, NULL);
}

static void
open_load_data (FpiSsm            *ssm,
                FpiDeviceValidity *self)
{
  const ValidityDeviceDesc *desc;
  GError *error = NULL;

  /* In emulation mode, skip loading external data files —
   * the emulated device uses in-memory store, not real blobs */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    {
      fp_dbg ("Emulation mode — skipping data file loading");
      fpi_ssm_next_state (ssm);
      return;
    }

  desc = validity_hal_device_lookup (self->dev_type);
  if (!desc)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  validity_data_store_init (&self->device_data);
  validity_data_store_init (&self->common_data);

  /* Load per-device blobs */
  if (!validity_data_load_device (&self->device_data,
                                  desc->vid, desc->pid, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* Load common data (partition sigs, CA cert, TLS keys, etc.) */
  if (!validity_data_load_common (&self->common_data, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* Populate TLS state key pointers from common data store */
  self->tls.password = validity_data_get_bytes (&self->common_data,
                                                VALIDITY_DATA_TLS_PASSWORD,
                                                &self->tls.password_len);
  self->tls.gwk_sign = validity_data_get_bytes (&self->common_data,
                                                VALIDITY_DATA_GWK_SIGN,
                                                &self->tls.gwk_sign_len);
  self->tls.fw_pubkey_x = validity_data_get_bytes (&self->common_data,
                                                   VALIDITY_DATA_FW_PUBKEY_X,
                                                   &self->tls.fw_pubkey_x_len);
  self->tls.fw_pubkey_y = validity_data_get_bytes (&self->common_data,
                                                   VALIDITY_DATA_FW_PUBKEY_Y,
                                                   &self->tls.fw_pubkey_y_len);

  fp_info ("Loaded external data files for %04x:%04x", desc->vid, desc->pid);
  fpi_ssm_next_state (ssm);
}

static void
open_send_init_hardcoded (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;
  gsize init_len;
  const guint8 *init_data;

  /* In emulation mode, skip raw USB blobs */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    {
      fp_dbg ("Emulation mode — skipping init_hardcoded");
      fpi_ssm_jump_to_state (ssm, OPEN_UPLOAD_FWEXT);
      return;
    }

  init_data = validity_data_get_bytes (&self->device_data,
                                       VALIDITY_DATA_INIT, &init_len);

  if (!init_data || init_len == 0)
    {
      fp_warn ("No init_hardcoded data loaded — skipping");
      fpi_ssm_jump_to_state (ssm, OPEN_UPLOAD_FWEXT);
      return;
    }

  fp_dbg ("Sending init_hardcoded (%zu bytes)", init_len);
  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT, init_len);
  memcpy (transfer->buffer, init_data, init_len);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_recv_init_hardcoded (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_send_init_clean_slate (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;
  gsize cs_len;
  const guint8 *cs_data;

  /* clean_slate is only sent when fwext is NOT loaded */
  if (self->fwext_loaded)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  cs_data = validity_data_get_bytes (&self->device_data,
                                     VALIDITY_DATA_INIT_CLEAN_SLATE,
                                     &cs_len);

  if (!cs_data || cs_len == 0)
    {
      fp_dbg ("No init_clean_slate data — skipping");
      fpi_ssm_next_state (ssm);
      return;
    }

  fp_info ("Fwext not loaded — sending init_clean_slate (%zu bytes)", cs_len);
  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT, cs_len);
  memcpy (transfer->buffer, cs_data, cs_len);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_recv_init_clean_slate (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  /* If fwext loaded, we skipped the send — just advance */
  if (self->fwext_loaded)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
open_upload_fwext (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);


  /* If fwext is already loaded, skip upload */
  if (self->fwext_loaded)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  /* In emulation mode, skip upload — no real device */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    {
      fp_dbg ("Emulation mode — skipping fwext upload");
      fpi_ssm_next_state (ssm);
      return;
    }

  /* Fwext upload requires a TLS session (flash writes need TLS).
   * If TLS handshake failed/skipped, we can't upload. */
  if (!self->tls.secure_rx)
    {
      fp_warn ("No TLS session — cannot upload firmware extension "
               "(device may need pairing first)");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  fp_info ("Firmware extension not loaded — starting upload via TLS");

  self->open_ssm = ssm;
  FpiSsm *fwext_ssm = validity_fwext_upload_ssm_new (dev);
  fpi_ssm_start (fwext_ssm, fwext_upload_ssm_done);
}

static void
open_pair (FpiSsm            *ssm,
           FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);


  /* In emulation mode, skip pairing */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    {
      fp_dbg ("Emulation mode — skipping pairing check");
      fpi_ssm_next_state (ssm);
      return;
    }

  fp_info ("Starting pairing check…");
  validity_pair_state_init (&self->pair_state);
  /* The in-driver reconnect re-runs this sequence to restore a session with
   * a sensor that was working moments ago. It must never pair (pairing
   * erases flash): run the read-only check, and let the pair SSM refuse,
   * failing the reconnect, if pairing would be needed. */
  self->pair_state.check_only = self->in_recovery;
  self->open_ssm = ssm;
  FpiSsm *pair_ssm = validity_pair_ssm_new (dev);
  fpi_ssm_start (pair_ssm, pair_ssm_done);
}

static void
open_tls_read_flash (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* In emulation mode (tests), skip TLS — no real device to talk to */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    {
      fp_dbg ("Emulation mode — skipping TLS flash read");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  /* Read flash partition 1 to get TLS keys.
   * TLS works independently of fwext (partition 2). */
  self->open_ssm = ssm;
  FpiSsm *flash_ssm = fpi_ssm_new (dev,
                                   validity_tls_flash_read_run_state,
                                   TLS_FLASH_READ_NUM_STATES);
  fpi_ssm_start (flash_ssm, flash_read_ssm_done);
}

static void
open_tls_derive_psk (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  /* Derive PSK from hardware identity (DMI) */
  validity_tls_derive_psk (&self->tls);

  /* Flash response format (after 2-byte status already stripped):
   *   [size:4 LE][unknown:2][flash_data:size]
   * See python-validity: sz, = unpack('<xxLxx', rsp[:8]) */
  GError *error = NULL;
  if (self->cmd_response_data && self->cmd_response_len > 6)
    {
      guint32 flash_sz = 0;
      validity_unpack (self->cmd_response_data, self->cmd_response_len,
                       "wxx", &flash_sz);
      const guint8 *flash_data = self->cmd_response_data + 6;
      gsize flash_avail = self->cmd_response_len - 6;

      if (flash_sz > flash_avail)
        flash_sz = flash_avail;

      fp_dbg ("TLS flash: %u bytes of data (response had %zu)",
              flash_sz, self->cmd_response_len);

      if (!validity_tls_parse_flash (&self->tls,
                                     flash_data,
                                     flash_sz,
                                     &error))
        {
          fp_warn ("TLS flash parse failed: %s", error->message);
          /* Keys wrapped under another host identity's PSK: never re-pair
           * implicitly, since that erases the sensor. */
          if (self->tls.priv_key_mismatch)
            {
              g_clear_error (&error);
              error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                "The sensor is paired to a different "
                                                "host identity: another OS on this "
                                                "machine (such as Windows), another "
                                                "machine, or a changed or unreadable "
                                                "DMI product name or serial number. "
                                                "Re-pairing it would erase the "
                                                "fingerprints stored on the sensor, "
                                                "so it is not done automatically");
            }
          else
            {
              g_prefix_error (&error, "The sensor's TLS keys are unusable: ");
            }
          fpi_ssm_mark_failed (ssm, error);
          return;
        }
    }
  else
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "The sensor returned no TLS "
                                                     "key data"));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
open_tls_handshake (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* Not expected: a successful parse always loads the keys */
  if (!self->tls.keys_loaded)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "TLS keys not loaded"));
      return;
    }

  self->open_ssm = ssm;
  FpiSsm *tls_ssm = fpi_ssm_new (dev,
                                 validity_tls_handshake_run_state,
                                 TLS_HS_NUM_STATES);
  fpi_ssm_start (tls_ssm, tls_handshake_ssm_done);
}

static void
open_sensor_identify (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  /* Without TLS, sensor identification is not possible. Only emulation gets
   * here without a session (it skips pairing and TLS); anything else is
   * failed by the check in OPEN_DONE. */
  if (!self->tls.secure_rx)
    {
      fp_info ("No TLS session — skipping sensor identification");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  /* Send cmd 0x75 (identify_sensor) via TLS.
   * NULL callback: subsm auto-advances, response stashed in
   * self->cmd_response_data for the RECV state. */
  guint8 cmd[] = { VCSFW_CMD_IDENTIFY_SENSOR };
  vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
}

static void
open_sensor_identify_recv (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("identify_sensor failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  {
    GString *hex = g_string_new ("identify_sensor raw: ");
    for (gsize i = 0; i < self->cmd_response_len; i++)
      g_string_append_printf (hex, "%02x ", self->cmd_response_data[i]);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }

  if (!validity_sensor_parse_identify (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &self->sensor.ident))
    {
      fp_warn ("identify_sensor: response too short");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  fp_info ("Sensor hardware: major=0x%04x version=0x%04x",
           self->sensor.ident.hw_major,
           self->sensor.ident.hw_version);

  /* Look up device info and sensor type */
  self->sensor.device_info = validity_device_info_lookup (
    self->sensor.ident.hw_major,
    self->sensor.ident.hw_version);

  if (self->sensor.device_info)
    {
      fp_info ("Device: %s (type=0x%04x)",
               self->sensor.device_info->name,
               self->sensor.device_info->type);

      self->sensor.type_info = validity_sensor_type_info_lookup (
        self->sensor.device_info->type);

      /* Pick the true silicon type for the finger-down interrupt path, which
       * keys off the real type. FM-154/FM-3439 units are 0x969 silicon exposed
       * under the 0x199 profile alias; key off the stable device name so the
       * type-3-as-finger-down handling is not bypassed. Genuine 0x199 chips
       * (FM-3367, FM-155, FM-3380) do not match and keep their type. The name
       * comes from device_info_table, which spells this family "FM-154" and
       * "FM-3439" (python-validity keys off the sensor's own string, spaced as
       * "FM- 154"; here we match our table's unspaced spelling). */
      self->sensor.real_sensor_type = self->sensor.device_info->type;
      if (self->sensor.device_info->type == 0x0199)
        {
          const gchar *name = self->sensor.device_info->name;

          if (g_strstr_len (name, -1, "FM-3439") ||
              g_strstr_len (name, -1, "FM-154"))
            {
              fp_info ("Sensor %s reports type 0x199, treating as 0x969", name);
              self->sensor.real_sensor_type = 0x0969;
            }
        }

      if (self->sensor.type_info)
        {
          fp_info ("Sensor type: 0x%04x, %u bytes/line, %ux repeat",
                   self->sensor.type_info->sensor_type,
                   self->sensor.type_info->bytes_per_line,
                   self->sensor.type_info->repeat_multiplier);
        }
      else
        {
          fp_warn ("Unknown sensor type 0x%04x",
                   self->sensor.device_info->type);
        }
    }
  else
    {
      fp_warn ("Unknown hardware major=0x%04x version=0x%04x",
               self->sensor.ident.hw_major,
               self->sensor.ident.hw_version);
    }

  fpi_ssm_next_state (ssm);
}

static void
open_sensor_factory_bits (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  /* Factory bits are needed for calibration. If sensor wasn't
   * identified, skip this step. */
  if (!self->sensor.device_info)
    {
      fp_info ("No sensor info — skipping factory bits");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  /* Build and send cmd 0x6f (GET_FACTORY_BITS) with tag 0x0e00 */
  guint8 cmd[9];
  validity_sensor_build_factory_bits_cmd (0x0e00, cmd, sizeof (cmd));
  vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
}

static void
open_sensor_factory_bits_recv (FpiSsm            *ssm,
                               FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("get_factory_bits failed: status=0x%04x",
               self->cmd_response_status);
      /* Non-fatal: calibration will have to work without factory data */
      fpi_ssm_next_state (ssm);
      return;
    }

  /* Store raw factory bits for calibration (iter 5) */
  g_clear_pointer (&self->sensor.factory_bits, g_free);
  if (self->cmd_response_data && self->cmd_response_len > 0)
    {
      self->sensor.factory_bits = g_memdup2 (self->cmd_response_data,
                                             self->cmd_response_len);
      self->sensor.factory_bits_len = self->cmd_response_len;
      fp_info ("Factory bits: %zu bytes", self->sensor.factory_bits_len);
    }

  fpi_ssm_next_state (ssm);
}

static void
open_capture_setup (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  /* Initialize capture state from sensor identification and factory bits.
   * Requires: sensor.type_info and sensor.device_info from IDENTIFY,
   * sensor.factory_bits from FACTORY_BITS. */
  if (!self->sensor.type_info || !self->sensor.device_info)
    {
      fp_info ("No sensor type info — skipping capture setup");
      fpi_ssm_next_state (ssm);
      return;
    }

  validity_capture_state_init (&self->capture);

  if (!validity_capture_state_setup (&self->capture,
                                     self->sensor.type_info,
                                     self->sensor.device_info->type,
                                     self->version_info.version_major,
                                     self->version_info.version_minor,
                                     self->sensor.factory_bits,
                                     self->sensor.factory_bits_len))
    {
      fp_warn ("Capture state setup failed — "
               "enrollment/verification will not be available");
      /* Non-fatal: device can still be used for identification
       * if calibration data exists on flash */
    }
  else
    {
      fp_info ("Capture state: %u bytes/line, %u lines/frame, "
               "type1=%d",
               self->capture.bytes_per_line,
               self->capture.lines_per_frame,
               self->capture.is_type1_device);
    }

  fpi_ssm_next_state (ssm);
}

static void
open_calibrate_build (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  /* Run calibration captures to establish sensor finger-detect baseline.
   * PY: sensor.calibrate() — 3 iterations of CALIBRATE capture.
   * Without this, chunk 0x26 (Finger Detect) always triggers. */
  if (!self->sensor.type_info ||
      self->capture.bytes_per_line == 0)
    {
      fp_info ("No capture state — skipping calibration");
      fpi_ssm_jump_to_state (ssm, OPEN_DONE);
      return;
    }

  self->calib_iteration = 0;
  g_clear_pointer (&self->capture.calib_data, g_free);
  self->capture.calib_data_len = 0;

  fp_info ("Starting sensor calibration (%u iterations)",
           self->capture.calibration_iterations);
  fpi_ssm_next_state (ssm);
}

static void
open_calibrate_send (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd;

  cmd = validity_capture_build_cmd_02 (&self->capture,
                                       self->sensor.type_info,
                                       VALIDITY_CAPTURE_CALIBRATE,
                                       &cmd_len);
  if (!cmd)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Sensor calibration failed: "
                                                     "could not build the "
                                                     "calibration capture command"));
      return;
    }

  fp_dbg ("Calibration iteration %u/%u",
          self->calib_iteration + 1,
          self->capture.calibration_iterations);
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
open_calibrate_send_recv (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* PY: calibrate() asserts the status, so a failed calibration capture
   * fails the open. Carrying on would leave the sensor with partial or no
   * calibration, and without it chunk 0x26 (Finger Detect) always triggers. */
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("Calibration capture failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Sensor calibration failed: "
                                                     "calibration capture %u/%u "
                                                     "returned status 0x%04x",
                                                     self->calib_iteration + 1,
                                                     self->capture.calibration_iterations,
                                                     self->cmd_response_status));
      return;
    }

  /* Read calibration data from EP 0x82.
   * PY: usb.read_82() — reads all bulk data from the sensor. */
  {
    gsize expected_size = (gsize) (self->capture.calibration_frames *
                                   self->capture.lines_per_frame + 1) *
                          self->capture.bytes_per_line;
    FpiUsbTransfer *xfer = fpi_usb_transfer_new (dev);

    fp_dbg ("Reading calibration data: %zu bytes from EP 0x82",
            expected_size);
    xfer->ssm = ssm;
    fpi_usb_transfer_fill_bulk (xfer, VALIDITY_EP_DATA_IN,
                                expected_size);
    fpi_usb_transfer_submit (xfer, 5000, NULL,
                             calib_bulk_read_cb, NULL);
  }
}

static void
open_calibrate_read_data (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  if (self->bulk_data && self->bulk_data_len > 0)
    {
      /* Average the raw calibration frames */
      gsize averaged_len = 0;
      guint8 *averaged = validity_capture_average_frames (
        self->bulk_data,
        self->bulk_data_len,
        self->capture.lines_per_frame,
        self->capture.bytes_per_line,
        self->sensor.type_info->lines_per_calibration_data,
        self->capture.calibration_frames,
        &averaged_len);

      if (averaged && averaged_len > 0)
        {
          /* Process calibration: scale and accumulate into calib_data */
          validity_capture_process_calibration (
            &self->capture.calib_data,
            &self->capture.calib_data_len,
            averaged,
            averaged_len,
            self->capture.bytes_per_line);

          fp_dbg ("Calibration iteration %u complete: "
                  "averaged %zu bytes, calib_data %zu bytes",
                  self->calib_iteration + 1,
                  averaged_len,
                  self->capture.calib_data_len);
          g_free (averaged);
        }
      else
        {
          fp_warn ("Calibration iteration %u: averaging failed",
                   self->calib_iteration + 1);
          g_free (averaged);
        }

      g_clear_pointer (&self->bulk_data, g_free);
      self->bulk_data_len = 0;
    }
  else
    {
      fp_warn ("Calibration iteration %u: no bulk data",
               self->calib_iteration + 1);
    }

  fpi_ssm_next_state (ssm);
}

static void
open_calibrate_loop (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  self->calib_iteration++;
  if (self->calib_iteration < self->capture.calibration_iterations)
    {
      fpi_ssm_jump_to_state (ssm, OPEN_CALIBRATE_SEND);
    }
  else if (!self->capture.calib_data || self->capture.calib_data_len == 0)
    {
      /* No iteration produced usable data (each one warned above) */
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Sensor calibration failed: "
                                                     "no calibration data was "
                                                     "captured"));
    }
  else
    {
      fp_info ("Sensor calibration complete");
      fpi_ssm_next_state (ssm);
    }
}

static void
open_done (FpiSsm            *ssm,
           FpiDeviceValidity *self)
{
  /* Every enroll, verify, list and delete runs over TLS, so an open without
   * a session only defers the failure to the first action, where it shows up
   * as an unrelated error. The steps above report their own cause when TLS
   * cannot be set up; this catches any other path that gets here without a
   * session. Emulation (umockdev) skips pairing and TLS by design and uses
   * an in-memory print store instead. */
  if (!self->tls.secure_rx &&
      g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") != 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "No TLS session with the "
                                                     "sensor was established"));
      return;
    }

  /* All init commands sent. Mark open complete. */
  fpi_ssm_mark_completed (ssm);
}

static void
open_run_state (FpiSsm   *ssm,
                FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_VERSION:
      open_get_version (ssm, self);
      break;

    case OPEN_RECV_VERSION:
      open_recv_version (ssm, self);
      break;

    case OPEN_SEND_CMD19:
      open_send_cmd19 (ssm, self);
      break;

    case OPEN_RECV_CMD19:
      open_recv_cmd19 (ssm, self);
      break;

    case OPEN_SEND_GET_FW_INFO:
      open_send_get_fw_info (ssm, self);
      break;

    case OPEN_RECV_GET_FW_INFO:
      open_recv_get_fw_info (ssm, self);
      break;

    case OPEN_LOAD_DATA:
      open_load_data (ssm, self);
      break;

    case OPEN_SEND_INIT_HARDCODED:
      open_send_init_hardcoded (ssm, self);
      break;

    case OPEN_RECV_INIT_HARDCODED:
      open_recv_init_hardcoded (ssm, self);
      break;

    case OPEN_SEND_INIT_CLEAN_SLATE:
      open_send_init_clean_slate (ssm, self);
      break;

    case OPEN_RECV_INIT_CLEAN_SLATE:
      open_recv_init_clean_slate (ssm, self);
      break;

    case OPEN_UPLOAD_FWEXT:
      open_upload_fwext (ssm, self);
      break;

    case OPEN_PAIR:
      open_pair (ssm, self);
      break;

    case OPEN_TLS_READ_FLASH:
      open_tls_read_flash (ssm, self);
      break;

    case OPEN_TLS_DERIVE_PSK:
      open_tls_derive_psk (ssm, self);
      break;

    case OPEN_TLS_HANDSHAKE:
      open_tls_handshake (ssm, self);
      break;

    case OPEN_SENSOR_IDENTIFY:
      open_sensor_identify (ssm, self);
      break;

    case OPEN_SENSOR_IDENTIFY_RECV:
      open_sensor_identify_recv (ssm, self);
      break;

    case OPEN_SENSOR_FACTORY_BITS:
      open_sensor_factory_bits (ssm, self);
      break;

    case OPEN_SENSOR_FACTORY_BITS_RECV:
      open_sensor_factory_bits_recv (ssm, self);
      break;

    case OPEN_CAPTURE_SETUP:
      open_capture_setup (ssm, self);
      break;

    case OPEN_CALIBRATE_BUILD:
      open_calibrate_build (ssm, self);
      break;

    case OPEN_CALIBRATE_SEND:
      open_calibrate_send (ssm, self);
      break;

    case OPEN_CALIBRATE_SEND_RECV:
      open_calibrate_send_recv (ssm, self);
      break;

    case OPEN_CALIBRATE_READ_DATA:
      open_calibrate_read_data (ssm, self);
      break;

    case OPEN_CALIBRATE_LOOP:
      open_calibrate_loop (ssm, self);
      break;

    case OPEN_DONE:
      open_done (ssm, self);
      break;
    }
}

static void
open_ssm_done (FpiSsm   *ssm,
               FpDevice *dev,
               GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  self->cmd_ssm = NULL;

  if (error)
    {
      validity_release_state (self);
      g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, NULL);
      fpi_device_open_complete (dev, validity_map_removed_error (error));
      return;
    }

  fp_info ("Validity sensor opened successfully");
  fpi_device_open_complete (dev, NULL);
}

/* ================================================================
 * Tier 0 in-driver recovery
 *
 * Recovery inside the driver, with no libfprint core changes, from a sensor
 * reset that drops the TLS session but leaves the device on the bus: re-run
 * the open sequence on the same GUsbDevice. The TLS session, any on-chip
 * enroll or match session and the calibration do not survive the reset, so
 * the open sequence re-reads the keys from flash, redoes the handshake and
 * recalibrates, and an interrupted operation restarts from its beginning
 * rather than resuming. A true re-enumeration leaves a dead GUsbDevice and
 * still reports FP_DEVICE_ERROR_REMOVED; recovering from that needs core
 * support, proposed in libfprint issue #852:
 * https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/852
 * ================================================================ */

gboolean
validity_probe_alive (FpiDeviceValidity *self)
{
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (self));

  g_autoptr(GError) error = NULL;

  if (usb == NULL)
    return FALSE;

  /* A live device answers a GET_CONFIGURATION control request; one that has
   * left the bus errors (NO_DEVICE). */
  g_usb_device_get_configuration (usb, &error);
  return error == NULL;
}

static void
reconnect_ssm_done (FpiSsm   *ssm,
                    FpDevice *dev,
                    GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);
  ValidityReconnectCb cb = self->reconnect_cb;
  gpointer cb_data = self->reconnect_cb_data;
  gboolean ok = (error == NULL);

  self->cmd_ssm = NULL;
  self->in_recovery = FALSE;
  self->reconnect_cb = NULL;
  self->reconnect_cb_data = NULL;

  if (error)
    {
      fp_warn ("Reconnect failed: %s", error->message);
      g_error_free (error);
    }
  else
    {
      fp_info ("Reconnect succeeded -- session re-established");
    }

  if (cb)
    cb (self, ok, cb_data);
}

void
validity_reconnect_start (FpiDeviceValidity  *self,
                          ValidityReconnectCb cb,
                          gpointer            user_data)
{
  FpDevice *dev = FP_DEVICE (self);
  GUsbDevice *usb = fpi_device_get_usb_device (dev);

  g_autoptr(GError) error = NULL;
  FpiSsm *ssm;

  self->in_recovery = TRUE;
  self->reconnect_cb = cb;
  self->reconnect_cb_data = user_data;
  self->reconnect_attempts++;

  fp_info ("Reconnect attempt %u: re-running open on the same device",
           self->reconnect_attempts);

  /* Free the session-lost state and re-initialise it -- a close+open in place.
   * The open sequence started below reloads the data package, re-reads the
   * flash keys, re-runs the TLS handshake and recalibrates. */
  validity_release_state (self);
  validity_tls_init (&self->tls);
  validity_sensor_state_init (&self->sensor);
  validity_capture_state_init (&self->capture);

  /* Soft-reset and re-claim interface 0, mirroring dev_open. If the reset
  * caused a true re-enumeration the handle is dead and the claim fails --
  * report failure so the caller falls back to FP_DEVICE_ERROR_REMOVED. */
  g_usb_device_reset (usb, &error);
  g_clear_error (&error);
  g_usb_device_release_interface (usb, 0, 0, NULL);
  if (!g_usb_device_claim_interface (usb, 0, 0, &error))
    {
      fp_warn ("Reconnect: re-claim failed (device gone): %s", error->message);
      self->in_recovery = FALSE;
      self->reconnect_cb = NULL;
      self->reconnect_cb_data = NULL;
      if (cb)
        cb (self, FALSE, user_data);
      return;
    }

  ssm = fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES);
  self->open_ssm = ssm;
  fpi_ssm_start (ssm, reconnect_ssm_done);
}

gboolean
validity_recover_or_fail (FpiDeviceValidity  *self,
                          const GError       *error,
                          ValidityReconnectCb on_done,
                          gpointer            user_data)
{
  ValidityErrClass klass;
  gboolean alive;

  if (self->in_recovery)                 /* already recovering -- don't stack */
    return FALSE;
  if (self->reconnect_attempts >= VALIDITY_MAX_RECONNECTS)
    {
      fp_warn ("Reconnect budget (%d) exhausted -- giving up",
               VALIDITY_MAX_RECONNECTS);
      return FALSE;
    }

  /* Probe liveness: distinguishes a session-losing reset (device still on the
   * bus, recoverable) from a true re-enumeration (dead handle, not). */
  alive = validity_probe_alive (self);
  klass = validity_classify_error (error, alive);

  /* A chip that reset its TLS session surfaces as a generic PROTO error plus
   * the alert flag; treat that as a recoverable session loss if still alive. */
  if (klass == VALIDITY_ERR_FATAL && self->tls.session_broken && alive)
    klass = VALIDITY_ERR_SESSION_LOST;

  if (klass != VALIDITY_ERR_PIPE_STALL && klass != VALIDITY_ERR_SESSION_LOST)
    return FALSE;

  fp_info ("Recoverable device reset mid-operation (%s) -- reconnecting",
           klass == VALIDITY_ERR_PIPE_STALL ? "pipe stall" : "session lost");
  validity_reconnect_start (self, on_done, user_data);
  return TRUE;
}

static void
dev_open (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  GError *error = NULL;
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  validity_tls_init (&self->tls);
  validity_sensor_state_init (&self->sensor);
  validity_capture_state_init (&self->capture);

  /* Emulation mode: in-memory print store for virtual sensor */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    self->emulation_prints = g_ptr_array_new_with_free_func (g_object_unref);

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device), 0, 0, &error))
    {
      validity_release_state (self);
      fpi_device_open_complete (device, validity_map_removed_error (error));
      return;
    }

  ssm = fpi_ssm_new (device, open_run_state, OPEN_NUM_STATES);
  self->cmd_ssm = ssm;
  fpi_ssm_start (ssm, open_ssm_done);
}

/* ================================================================
 * Close
 * ================================================================ */

/* Release all per-open heap state. Safe to call more than once and on
 * partially-initialised state (every field is cleared to NULL/0). Called from
 * dev_close and from the open-failure paths, where the device layer never
 * invokes dev_close (is_open stays FALSE) so this is the only cleanup. */
static void
validity_release_state (FpiDeviceValidity *self)
{
  g_clear_pointer (&self->cmd_response_data, g_free);
  self->cmd_response_len = 0;

  g_clear_pointer (&self->enroll_template, g_free);
  self->enroll_template_len = 0;
  g_clear_pointer (&self->bulk_data, g_free);
  self->bulk_data_len = 0;
  validity_user_storage_clear (&self->list_storage);
  validity_user_clear (&self->verify_match_user);

  validity_capture_state_clear (&self->capture);
  validity_sensor_state_clear (&self->sensor);
  validity_pair_state_free (&self->pair_state);
  validity_tls_free (&self->tls);
  validity_data_store_free (&self->device_data);
  validity_data_store_free (&self->common_data);

  g_clear_pointer (&self->emulation_prints, g_ptr_array_unref);
}

static void
dev_close (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);

  g_autoptr(GError) error = NULL;

  G_DEBUG_HERE ();

  validity_release_state (self);

  g_usb_device_release_interface (fpi_device_get_usb_device (device), 0, 0, &error);

  fpi_device_close_complete (device, g_steal_pointer (&error));
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

static void
fpi_device_validity_init (FpiDeviceValidity *self)
{
}

static void
fpi_device_validity_class_init (FpiDeviceValidityClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Validity VCSFW Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = VALIDITY_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = dev_probe;
  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = validity_enroll;
  dev_class->verify = validity_verify;
  dev_class->identify = validity_identify;
  dev_class->delete = validity_delete;
  dev_class->list = validity_list;
  dev_class->clear_storage = validity_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);
}
