/*
 * Enrollment state machine for Validity/Synaptics VCSFW sensors
 *
 * Implements the FpDevice::enroll virtual method. The enrollment flow:
 *   1. Create enrollment session on sensor (cmd 0x69)
 *   2. Loop until the sensor returns a template id (tid):
 *      a. LED on, build capture cmd, send via TLS
 *      b. Wait for finger-down interrupt (EP 0x83)
 *      c. Wait for scan-complete interrupt
 *      d. Run enrollment_update_start (cmd 0x68)
 *      e. Send db_write_enable, enrollment_update (cmd 0x6B), cleanups
 *      f. Parse response for template/header/tid
 *      g. Report progress, enrollment_update_end (cmd 0x69 flag=0)
 *   3. When tid is returned: store user + finger in DB
 *   4. LED off, report FpPrint to libfprint
 *
 * Reference: python-validity sensor.py Sensor.enroll()
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
#include "fpi-print.h"
#include "validity.h"
#include "vcsfw_protocol.h"

/* Upper bound on frame retries (0x0526 rejections and captures the sensor
 * flagged as flawed) before giving up, so broken or hostile firmware that
 * rejects every frame cannot loop forever. Generous: a real enrollment never
 * rejects anywhere near this many frames. */
#define VALIDITY_ENROLL_MAX_REJECTS 30

/* ================================================================
 * Enroll progress
 *
 * The chip, not the host, decides when it has enough frames: it returns
 * the template id (tid) once it is satisfied, and python-validity simply
 * loops until the tid appears. Testers have seen it finish after 5, 6 and 8
 * frames, and nothing stops it asking for more than VALIDITY_ENROLL_STAGES.
 * nr_enroll_stages is therefore a nominal count.
 *
 * - While the chip still wants frames, reported progress is capped at
 *   VALIDITY_ENROLL_STAGES - 1: fprintd drops any report with
 *   completed_stages >= nr_enroll_stages (retries included), and "N of N"
 *   would claim a finished enrollment.
 * - Retry prompts repeat the last reported value, so they stay under the
 *   cap and still reach the user on an enrollment longer than N frames.
 * - On success the remaining stages are reported right before
 *   fpi_device_enroll_complete, after the print is stored, so a UI that
 *   counts stage-passed signals ends at 100% and a failure after the tid
 *   never shows a full bar first. The synaptics driver catches up the same
 *   way when its sensor's progress jumps.
 * - enroll_reported is a high-water mark for the whole action. The
 *   in-driver reconnect restarts the chip's enrollment from frame 0 but
 *   re-dispatches the same action, so the mark survives it and is reset
 *   only by a fresh enroll: stages the UI has already counted are not
 *   reported twice.
 * ================================================================ */

gint
validity_enroll_progress_target (gint     reported,
                                 guint    frames,
                                 gboolean complete)
{
  gint capped;

  if (complete)
    return VALIDITY_ENROLL_STAGES;

  capped = (gint) MIN (frames, (guint) VALIDITY_ENROLL_STAGES - 1);

  return MAX (reported, capped);
}

/* Report every stage from enroll_reported + 1 up to @target, one call each:
 * fprintd turns each report into one stage-passed signal, and UIs such as
 * GNOME's count those signals rather than read the value. */
static void
enroll_report_progress (FpiDeviceValidity *self,
                        gint               target)
{
  while (self->enroll_reported < target)
    {
      self->enroll_reported++;
      fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_reported,
                                  NULL, NULL);
    }
}

static void
enroll_report_retry (FpiDeviceValidity *self)
{
  fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_reported, NULL,
                              fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
}

/* ================================================================
 * Interrupt helpers — read from EP 0x83
 * ================================================================ */

static void
interrupt_cb (FpiUsbTransfer *transfer,
              FpDevice       *device,
              gpointer        user_data,
              GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm = user_data;
  guint8 int_type;
  int target_state = GPOINTER_TO_INT (fpi_ssm_get_data (ssm));

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
          g_error_free (error);
          return;
        }
      /* Scan-complete timeout — finger was likely stale (no fresh placement).
       * Stop capture, signal user to lift, then retry. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT) &&
          target_state == 3)
        {
          g_error_free (error);
          self->scan_incomplete_count++;
          fp_info ("Scan incomplete (attempt %u) — asking user to retry",
                   self->scan_incomplete_count);
          if (self->scan_incomplete_count > 3)
            {
              fp_warn ("Too many scan retries, giving up");
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
              return;
            }
          enroll_report_retry (self);
          /* Skip get_prg_status (only valid after complete scan) — go
           * straight to capture_stop → LED off → delay → LED on → retry */
          fpi_ssm_jump_to_state (ssm, ENROLL_CAPTURE_STOP);
          return;
        }
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (transfer->actual_length < 1)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  int_type = transfer->buffer[0];

  if (transfer->actual_length >= 5)
    {
      fp_dbg ("Interrupt: type=0x%02x bytes=[%02x %02x %02x %02x %02x] (len=%" G_GSSIZE_FORMAT ")",
              int_type, transfer->buffer[0], transfer->buffer[1],
              transfer->buffer[2], transfer->buffer[3], transfer->buffer[4],
              transfer->actual_length);
    }
  else
    {
      fp_dbg ("Interrupt: type=0x%02x (len=%" G_GSSIZE_FORMAT ")",
              int_type, transfer->actual_length);
    }

  /* Sensors that skip the type-2 finger-down interrupt (0x969/0xd51) jump
   * straight to type-3 capture events while we are still waiting for
   * finger-down. Accept the type-3 as the finger-down signal. Mirror
   * python-validity's saved_b: the same type-3 must also be tested for
   * scan-complete, because a single type-3 can carry both -- advancing to
   * ENROLL_WAIT_SCAN_COMPLETE (which issues a fresh read) would lose it and
   * hang. */
  if (target_state == VALIDITY_INT_FINGER_DOWN && int_type == 3 &&
      self->sensor.device_info &&
      validity_sensor_skips_finger_down (self->sensor.real_sensor_type))
    {
      fp_info ("Finger detected on sensor (type-3; this silicon omits type-2)");
      fpi_device_report_finger_status_changes (
        FP_DEVICE (self), FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);

      if (transfer->actual_length >= 3 &&
          (transfer->buffer[2] & VALIDITY_INT_SCAN_COMPLETE))
        {
          /* Finger-down and scan-complete arrived together -- skip the
           * separate scan-complete wait, which would block on a fresh read. */
          self->scan_incomplete_count = 0;
          fpi_ssm_jump_to_state (ssm, ENROLL_GET_PRG_STATUS);
        }
      else
        {
          /* Finger down, scan still in progress -- await the completing
           * type-3 in ENROLL_WAIT_SCAN_COMPLETE. */
          fpi_ssm_next_state (ssm);
        }
      return;
    }

  /* Check if this is the interrupt we're waiting for */
  if (int_type == (guint8) target_state)
    {
      /* Finger-down detected */
      if (int_type == 2)
        {
          fp_info ("Finger detected on sensor");
          fpi_device_report_finger_status_changes (
            FP_DEVICE (self), FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);
        }
      /* Check scan-complete bit if waiting for type 3 */
      if (int_type == 3 && transfer->actual_length >= 3)
        {
          if (!(transfer->buffer[2] & VALIDITY_INT_SCAN_COMPLETE))
            /* Not scan complete yet, keep waiting */
            goto read_again;
          /* Scan fully complete — reset retry counter */
          self->scan_incomplete_count = 0;
        }
      fpi_ssm_next_state (ssm);
      return;
    }

  /* Type 0 = capture started, type 3 without scan_complete = in progress */
  if (int_type == 0 || int_type == 3)
    goto read_again;

  /* Unexpected interrupt type, keep listening */
  fp_dbg ("Ignoring unexpected interrupt type 0x%02x", int_type);

read_again:
  {
    FpiUsbTransfer *new_transfer = fpi_usb_transfer_new (device);
    fpi_usb_transfer_fill_interrupt (new_transfer, VALIDITY_EP_INT_IN,
                                     VALIDITY_USB_INT_DATA_SIZE);
    /* 30s timeout for scan_complete; unlimited for finger-down */
    fpi_usb_transfer_submit (new_transfer,
                             (target_state == 3) ? 30000 : 0,
                             fpi_device_get_cancellable (FP_DEVICE (self)),
                             interrupt_cb, ssm);
  }
}

static void
start_interrupt_wait (FpiDeviceValidity *self,
                      FpiSsm            *ssm,
                      guint8             target_type)
{
  FpiUsbTransfer *transfer;

  /* Store the target interrupt type in ssm data temporarily.
   * We'll restore it after the wait. */
  fpi_ssm_set_data (ssm, GINT_TO_POINTER ((int) target_type), NULL);

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  fpi_usb_transfer_submit (transfer, 0,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           interrupt_cb, ssm);
}

/* Simple interrupt callback — accepts any interrupt and advances SSM.
 * Used between the two enrollment_update calls where PY does usb.wait_int(). */
static void
update_interrupt_cb (FpiUsbTransfer *transfer,
                     FpDevice       *device,
                     gpointer        user_data,
                     GError         *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
        }
      else
        {
          /* fpi_ssm_mark_failed takes ownership of the error (and here
           * synchronously frees the SSM); do not free it again. */
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        }
      return;
    }

  if (transfer->actual_length >= 1)
    fp_dbg ("Update interrupt: type=0x%02x (len=%" G_GSSIZE_FORMAT ")",
            transfer->buffer[0], transfer->actual_length);

  fpi_ssm_next_state (ssm);
}

/* ================================================================
 * Enrollment response parsing
 *
 * The enrollment_update (cmd 0x6B) response contains tagged fields:
 *   While data remains:
 *     tag(2LE) | len(2LE) | data[MAGIC_LEN + len]
 *   tag 0 → template, tag 1 → header, tag 3 → tid (enrollment complete)
 * ================================================================ */

void
enrollment_update_result_clear (EnrollmentUpdateResult *r)
{
  g_clear_pointer (&r->header, g_free);
  g_clear_pointer (&r->template_data, g_free);
  g_clear_pointer (&r->tid, g_free);
  memset (r, 0, sizeof (*r));
}

gboolean
parse_enrollment_update_response (const guint8           *data,
                                  gsize                   data_len,
                                  EnrollmentUpdateResult *result)
{
  FpiByteReader reader;
  guint16 declared_len;

  memset (result, 0, sizeof (*result));

  fpi_byte_reader_init (&reader, data, data_len);

  /* First 2 bytes are a length field (PY: l, = unpack('<H', res[:2])) */
  if (!fpi_byte_reader_get_uint16_le (&reader, &declared_len))
    return FALSE;

  if (declared_len != data_len - 2)
    fp_warn ("enrollment_update: declared len %u != actual %zu",
             declared_len, data_len - 2);

  while (fpi_byte_reader_get_remaining (&reader) >= 4)
    {
      guint16 tag, len;
      gsize block_size;
      guint block_pos = fpi_byte_reader_get_pos (&reader);

      if (!fpi_byte_reader_get_uint16_le (&reader, &tag) ||
          !fpi_byte_reader_get_uint16_le (&reader, &len))
        break;

      block_size = ENROLLMENT_MAGIC_LEN + len;

      fp_dbg ("enrollment_update: tag=%u len=%u block_size=%zu pos=%u",
              tag, len, block_size, block_pos);

      if (block_pos + block_size > data_len)
        break;

      if (tag == 0)
        {
          /* Template: first MAGIC_LEN + len bytes */
          result->template_data = g_memdup2 (&data[block_pos], block_size);
          result->template_len = block_size;
        }
      else if (tag == 1)
        {
          /* Header: data after MAGIC_LEN */
          if (len > 0)
            {
              result->header = g_memdup2 (&data[block_pos + ENROLLMENT_MAGIC_LEN], len);
              result->header_len = len;
            }
        }
      else if (tag == 3)
        {
          /* TID: data after MAGIC_LEN — enrollment is complete */
          if (len > 0)
            {
              result->tid = g_memdup2 (&data[block_pos + ENROLLMENT_MAGIC_LEN], len);
              result->tid_len = len;
            }
        }

      /* Advance past remaining block data (consumed 4 for tag+len) */
      if (block_size < 4 || !fpi_byte_reader_skip (&reader, block_size - 4))
        break;
    }

  return TRUE;
}

/* ================================================================
 * Enrollment SSM
 * ================================================================ */


static void
enroll_cleanup_stale (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  /* Close any stale enrollment session before starting fresh.
   * Firmware may have leftover state from a previous session
   * (e.g. if enrollment was interrupted). Send cmd 0x69 flag=0
   * (enrollment_update_end) — errors are expected and ignored. */
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_create_enrollment (FALSE, &cmd_len);

  self->enroll_key = 0;
  self->enroll_stage = 0;
  self->scan_incomplete_count = 0;
  self->enroll_frame_rejected = FALSE;
  self->enroll_reject_retries = 0;
  g_clear_pointer (&self->enroll_template, g_free);
  self->enroll_template_len = 0;
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
enroll_led_on_recv (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* LED is on — signal that we need a finger.
   * PY sends capture IMMEDIATELY after glow_start_scan(), no delay.
   * The ENROLL finger detect (chunk 0x26) needs to see the transition
   * from no-finger to finger-down to establish a proper baseline.
   * A delay would mean the finger is already on the sensor. */
  fpi_device_report_finger_status_changes (
    dev, FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_NONE);
  fpi_ssm_next_state (ssm);
}

static void
enroll_build_capture (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd = validity_capture_build_cmd_02 (&self->capture,
                                               self->sensor.type_info,
                                               VALIDITY_CAPTURE_ENROLL,
                                               &cmd_len);

  if (!cmd)
    {
      fp_warn ("Failed to build enroll capture command");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
enroll_capture_recv (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("Capture command failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Now wait for finger-down interrupt */
  fpi_ssm_next_state (ssm);
}

static void
enroll_wait_scan_complete (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  /* Wait for interrupt type 3 with scan_complete bit.
   * Use 30-second timeout: enroll mode scans need proper finger contact. */
  FpiUsbTransfer *transfer;

  fpi_ssm_set_data (ssm, GINT_TO_POINTER (3), NULL);
  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  fpi_usb_transfer_submit (transfer, 30000,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           interrupt_cb, ssm);
}

static void
enroll_capture_stop_recv (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  /* PY: no glow_end after capture — LED stays on. */
  if (self->enroll_frame_rejected)
    /* The sensor flagged the capture: skip enrollment_update_start and the
     * updates, so the frame never reaches the template. PY: capture()
     * raised, and enroll()'s finally still runs enrollment_update_end
     * before the loop re-captures, so go through ENROLL_UPDATE_END; its
     * LOOP_CHECK reports the retry and re-captures the same stage. */
    fpi_ssm_jump_to_state (ssm, ENROLL_UPDATE_END);
  else if (self->scan_incomplete_count > 0)
    /* Incomplete scan: retry after a brief delay.
     * glow_start at the top of the loop will reinitialize.
     * PY: in the except block, just retries the whole loop. */
    fpi_ssm_jump_to_state_delayed (ssm, ENROLL_LED_ON, 3000);
  else
    /* Good scan — proceed to enrollment_update_start */
    fpi_ssm_next_state (ssm);
}

static void
enroll_update_start_recv (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("enrollment_update_start failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Response: new_key(4LE) */
  if (self->cmd_response_data && self->cmd_response_len >= 4)
    validity_unpack (self->cmd_response_data, self->cmd_response_len,
                     "w", &self->enroll_key);

  fpi_ssm_next_state (ssm);
}

static void
enroll_wait_update_start_int (FpiSsm            *ssm,
                              FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* PY: usb.wait_int() inside enrollment_update_start() */
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  fpi_usb_transfer_submit (transfer, 0,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           update_interrupt_cb, ssm);
}

static void
enroll_append_image_recv (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  /* First enrollment_update call — just triggers firmware processing.
   * Response is status=OK with len=0; no data to parse here.
   * The actual result comes from the second call after the interrupt. */
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_info ("enrollment_update (trigger) rejected (0x%04x) -- cleanup and retry",
               self->cmd_response_status);
      /* Firmware rejected this frame (e.g. 0x0526). Do NOT skip straight to
       * update-end: python-validity commits call_cleanups in a finally even on
       * a rejected frame, and skipping it wedges the 06cb:00cb silicon (the chip
       * can drop off the bus mid-enroll). Mark the frame rejected and fall
       * through to ENROLL_CLEANUPS; ENROLL_CLEANUPS_RECV then jumps to
       * ENROLL_UPDATE_END -- skipping the wait-int and second update, which
       * python also skips once the first enrollment_update raised -- and
       * LOOP_CHECK retries the stage. */
      self->enroll_frame_rejected = TRUE;
      fpi_ssm_next_state (ssm);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
enroll_wait_update_int (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* PY: usb.wait_int() — wait for firmware to finish processing
   * the enrollment image before reading the result. */
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  fpi_usb_transfer_submit (transfer, 0,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           update_interrupt_cb, ssm);
}

static void
enroll_append_image_read_recv (FpiSsm            *ssm,
                               FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      /* Status 0x0526 = the sensor rejected this frame (typically a
       * near-duplicate from a finger that wasn't lifted between stages).
       * Not fatal: let the loop re-capture this stage, reporting a retry to
       * the user -- this mirrors python-validity's enroll loop, which retries
       * on any bad frame.
       *
       * Critically, still run call_cleanups (ENROLL_CLEANUPS_READ) before
       * ENROLL_UPDATE_END rather than jumping past it: python-validity commits
       * cleanups in a `finally`, so they run even when the frame is rejected.
       * Skipping them wedges the 06cb:00cb silicon -- the *next* stage's
       * enrollment_update (cmd 0x6b) bulk-OUT then times out, because pending
       * writes were never committed. 138a:00ab tolerates the skip; 06cb does
       * not. Fall through to the cleanups state to match python. */
      if (self->cmd_response_status == 0x0526)
        {
          fp_info ("enrollment_update rejected the frame (0x0526) -- retrying stage");
          self->enroll_frame_rejected = TRUE;
          fpi_ssm_next_state (ssm);
          return;
        }
      fp_warn ("enrollment_update (read) failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Parse the enrollment update response for template/header/tid */
  if (self->cmd_response_data && self->cmd_response_len > 0)
    {
      EnrollmentUpdateResult result;

      fp_info ("enrollment_update read response: len=%zu",
               self->cmd_response_len);

      if (parse_enrollment_update_response (self->cmd_response_data,
                                            self->cmd_response_len,
                                            &result))
        {
          /* Update template for next iteration */
          g_clear_pointer (&self->enroll_template, g_free);
          if (result.template_data)
            {
              self->enroll_template = g_steal_pointer (&result.template_data);
              self->enroll_template_len = result.template_len;
              fp_info ("  template: %zu bytes", self->enroll_template_len);
            }

          if (result.header)
            fp_info ("  header: %zu bytes", result.header_len);

          /* If tid is present, enrollment is complete */
          if (result.tid)
            {
              fp_info ("  tid: %zu bytes — enrollment complete!",
                       result.tid_len);
              g_clear_pointer (&self->bulk_data, g_free);
              self->bulk_data = g_steal_pointer (&result.tid);
              self->bulk_data_len = result.tid_len;
            }

          enrollment_update_result_clear (&result);
        }
    }
  else
    {
      fp_info ("enrollment_update read response: EMPTY (len=0)");
    }

  fpi_ssm_next_state (ssm);
}

static void
enroll_loop_check (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  /* The sensor rejected the last frame (0x0526), or flagged its capture as
   * flawed: report a retry and re-capture the SAME stage without advancing
   * progress, instead of failing the enroll.
   * This path deliberately skips the enroll_stage increment (and its upper
   * bound), so it needs its own cap: without one a device that returns 0x0526
   * on every frame would loop forever. A cancel still interrupts it, because
   * the next finger-wait is submitted on the action's GCancellable. */
  if (self->enroll_frame_rejected)
    {
      self->enroll_frame_rejected = FALSE;

      if (++self->enroll_reject_retries > VALIDITY_ENROLL_MAX_REJECTS)
        {
          fp_warn ("Enrollment rejected %u frames -- aborting",
                   self->enroll_reject_retries);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
          return;
        }

      enroll_report_retry (self);
      fp_info ("Enrollment frame rejected -- retrying stage %u", self->enroll_stage + 1);
      fpi_ssm_jump_to_state (ssm, ENROLL_LED_ON);
      return;
    }

  self->enroll_stage++;

  fp_info ("Enrollment frame %u accepted (nominal %u stages)",
           self->enroll_stage, VALIDITY_ENROLL_STAGES);

  /* If we have a TID, enrollment is complete. The remaining progress is
   * reported by enroll_ssm_done once the print is stored.
   * PY calls enrollment_update_end twice: once in the finally
   * block (ENROLL_UPDATE_END) and once more after the loop. */
  if (self->bulk_data && self->bulk_data_len > 0)
    {
      fpi_ssm_jump_to_state (ssm, ENROLL_UPDATE_END2);
      return;
    }

  enroll_report_progress (self,
                          validity_enroll_progress_target (self->enroll_reported,
                                                           self->enroll_stage,
                                                           FALSE));

  /* PY loops indefinitely until TID appears.  Use a generous
   * upper bound to avoid an infinite loop on broken firmware. */
  if (self->enroll_stage >= 30)
    {
      fp_warn ("Enrollment did not complete within %u stages",
               self->enroll_stage);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  /* Loop back for next stage */
  fpi_ssm_jump_to_state (ssm, ENROLL_LED_ON);
}

static void
enroll_get_storage_recv (FpiSsm            *ssm,
                         FpiDeviceValidity *self)
{
  /* PY: parse_user_storage returns None on 0x04b3 (not found).
   * init_db then creates the storage. We handle both cases. */
  if (self->cmd_response_status == 0x04b3)
    {
      fp_info ("StgWindsor storage not found — creating it");
      fpi_ssm_next_state (ssm); /* → ENROLL_INIT_STORAGE_WE */
      return;
    }

  ValidityUserStorage stg = { 0 };

  if (self->cmd_response_status != VCSFW_STATUS_OK ||
      !self->cmd_response_data ||
      !validity_db_parse_user_storage (self->cmd_response_data,
                                       self->cmd_response_len, &stg))
    {
      fp_warn ("get_user_storage failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  self->enroll_storage_dbid = stg.dbid;
  fp_info ("Storage dbid: %u", stg.dbid);
  validity_user_storage_clear (&stg);
  /* Skip storage creation states — jump to DB_WRITE_ENABLE2 */
  fpi_ssm_jump_to_state (ssm, ENROLL_DB_WRITE_ENABLE2);
}

static void
enroll_create_user (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* Create the user record via cmd 0x47 (new_record).
   * python-validity: usr = db.new_user(identity)
   * -> new_record(stg.dbid, 5, stg.dbid, identity_to_bytes(identity)) */

  FpPrint *print = NULL;
  g_autofree guint8 *identity = NULL;
  gsize identity_len;

  fpi_device_get_enroll_data (dev, &print);

  /* Derive the enrollment id with the shared helper, as the other
   * match-on-chip drivers do (e.g. synaptics). Each enrollment gets a fresh
   * id; the upper layer removes an existing finger before re-enrolling it, so
   * a new on-chip record per enrollment does not accumulate stale ones. */
  g_autofree gchar *user_id = fpi_print_generate_user_id (print);

  identity = validity_db_build_identity (user_id, &identity_len);
  if (!identity)
    {
      fp_warn ("Failed to build identity for user '%s'", user_id);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  /* Store user_id in print for later retrieval (e.g. delete) */
  GVariant *data = g_variant_new_string (user_id);
  g_object_set_data_full (G_OBJECT (print), "validity-user-id",
                          g_variant_ref_sink (data),
                          (GDestroyNotify) g_variant_unref);

  /* cmd 0x47: new_record(parent=storage_dbid, type=5=user, storage=storage_dbid, data=identity) */
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_new_record (
    self->enroll_storage_dbid,
    VALIDITY_DB_RECORD_TYPE_USER,
    self->enroll_storage_dbid,
    identity, identity_len,
    &cmd_len);
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
enroll_create_user_recv (FpiSsm            *ssm,
                         FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("create user failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Parse the new user record ID — stash for finger creation */
  guint16 user_dbid;
  if (self->cmd_response_data &&
      validity_db_parse_new_record_id (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &user_dbid))
    {
      fp_info ("Created user record: dbid=%u", user_dbid);
      self->enroll_user_dbid = user_dbid;
    }
  else
    {
      fp_warn ("Failed to parse new user record ID");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
enroll_create_finger (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpPrint *print = NULL;
  FpFinger finger;

  fpi_device_get_enroll_data (dev, &print);
  finger = fp_print_get_finger (print);

  guint16 subtype = validity_finger_to_subtype (finger);
  guint16 user_dbid = self->enroll_user_dbid;

  /* Build finger data from template + tid */
  gsize finger_data_len;
  guint8 *finger_data = validity_db_build_finger_data (
    subtype,
    self->enroll_template, self->enroll_template_len,
    self->bulk_data, self->bulk_data_len,
    &finger_data_len);

  /* cmd 0x47: new_record(parent=user, type=0x0b, storage=3, data=finger_data)
   * python-validity: type 0xb becomes 0x6 due to db_write_enable magic */
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_new_record (
    user_dbid,
    0x0b,  /* finger type: becomes 0x06 after db_write_enable */
    self->enroll_storage_dbid,
    finger_data, finger_data_len,
    &cmd_len);
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
  g_free (finger_data);
}

static void
enroll_create_finger_recv (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("create finger failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  guint16 finger_dbid;
  if (self->cmd_response_data &&
      validity_db_parse_new_record_id (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &finger_dbid))
    fp_info ("Created finger record: dbid=%u", finger_dbid);

  fpi_ssm_next_state (ssm);
}

static void
enroll_wait_finger_int (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* PY: usb.wait_int() after new_finger/cleanups — accept any interrupt */
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  fpi_usb_transfer_submit (transfer, 5000,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           update_interrupt_cb, ssm);
}

static void
enroll_run_state (FpiSsm   *ssm,
                  FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_CLEANUP_STALE:
      enroll_cleanup_stale (ssm, self);
      break;

    case ENROLL_CLEANUP_STALE_RECV:
      /* Ignore status -- no active session is fine (0x0405, etc.). Enroll adds
       * a new user+finger record and leaves any other enrolled fingers intact
       * (each finger is its own on-chip user record); fprintd deletes a finger
       * through the delete vfunc. */
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_DB_INFO:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_db_info (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_DB_INFO_RECV:
      {
        ValidityDbInfo info = { 0 };
        gboolean full = FALSE;

        /* Free-space guard: the DB partition never auto-compacts, so it fills
         * over enrollments and new_record then fails with an opaque 0x04b5.
         * Check free space up front and fail cleanly with DATA_FULL instead. */
        if (self->cmd_response_status == VCSFW_STATUS_OK &&
            self->cmd_response_data &&
            validity_db_parse_info (self->cmd_response_data,
                                    self->cmd_response_len, &info))
          {
            fp_info ("DB partition: free=%u used=%u total=%u records=%u",
                     info.free_space, info.used, info.total, info.records);
            full = info.free_space < VALIDITY_ENROLL_MIN_FREE_BYTES;
          }
        else
          {
            /* Could not read db_info -- do not block on the guard; a genuinely
            * full partition still surfaces (less cleanly) at create_finger. */
            fp_warn ("db_info query failed (status=0x%04x) -- skipping guard",
                     self->cmd_response_status);
          }
        validity_db_info_clear (&info);

        if (full)
          {
            fp_warn ("DB partition full -- refusing enroll (free < %u bytes)",
                     VALIDITY_ENROLL_MIN_FREE_BYTES);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_DATA_FULL,
                                   "Fingerprint storage is full; clear "
                                   "enrolled prints and retry"));
            break;
          }
        fpi_ssm_next_state (ssm);
      }
      break;

    case ENROLL_START:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_create_enrollment (TRUE, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_START_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("create_enrollment failed: status=0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_LED_ON:
      {
        gsize cmd_len;
        const guint8 *cmd = validity_capture_glow_start_cmd (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case ENROLL_LED_ON_RECV:
      enroll_led_on_recv (ssm, self);
      break;

    case ENROLL_WAIT_FINGER_DELAY:
      /* Pass-through (no delay needed — capture waits for finger via interrupts) */
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_BUILD_CAPTURE:
      enroll_build_capture (ssm, self);
      break;

    case ENROLL_CAPTURE_SEND:
      /* Capture command sent, now handled in RECV */
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_CAPTURE_RECV:
      enroll_capture_recv (ssm, self);
      break;

    case ENROLL_WAIT_FINGER:
      /* Wait for interrupt type 2 (finger down) */
      start_interrupt_wait (self, ssm, VALIDITY_INT_FINGER_DOWN);
      break;

    case ENROLL_WAIT_SCAN_COMPLETE:
      enroll_wait_scan_complete (ssm, self);
      break;

    case ENROLL_GET_PRG_STATUS:
      {
        const guint8 cmd[] = { 0x51, 0x00, 0x20, 0x00, 0x00 };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case ENROLL_GET_PRG_STATUS_RECV:
      /* Capture status: len(4) | x(2) | y(2) | w1(2) | w2(2) | error(4).
       * A non-zero error word means the sensor flagged the capture as
       * flawed. python-validity's capture() raises "Scanning problem" on it
       * and enroll() re-captures without submitting the frame; do the same.
       * Mark the frame rejected so ENROLL_CAPTURE_STOP_RECV skips the
       * enrollment update and ENROLL_LOOP_CHECK retries the stage. */
      if (self->cmd_response_data && self->cmd_response_len >= 16)
        {
          const guint8 *d = self->cmd_response_data;
          guint16 x  = d[4]  | (d[5] << 8);
          guint16 y  = d[6]  | (d[7] << 8);
          guint16 w1 = d[8]  | (d[9] << 8);
          guint16 w2 = d[10] | (d[11] << 8);
          guint32 err = d[12] | (d[13] << 8) | (d[14] << 16) | ((guint32) d[15] << 24);
          fp_dbg ("Enroll capture (stage %u): x=%u y=%u w1=%u w2=%u error=0x%08x",
                  self->enroll_stage, x, y, w1, w2, err);
          if (err != 0)
            {
              fp_info ("Enroll capture flagged by the sensor (error=0x%08x) -- "
                       "not submitting it", err);
              self->enroll_frame_rejected = TRUE;
            }
        }
      else
        {
          fp_dbg ("Enroll capture: short status response (len=%" G_GSIZE_FORMAT ")",
                  self->cmd_response_len);
        }
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_CAPTURE_STOP:
      {
        const guint8 cmd[] = { 0x04 };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case ENROLL_CAPTURE_STOP_RECV:
      enroll_capture_stop_recv (ssm, self);
      break;

    case ENROLL_UPDATE_START:
      {
        gsize cmd_len;
        guint8 *cmd = validity_pack_new (&cmd_len, "bww",
                                         VCSFW_CMD_ENROLLMENT_UPDATE_START,
                                         self->enroll_key, (guint32) 0);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_UPDATE_START_RECV:
      enroll_update_start_recv (ssm, self);
      break;

    case ENROLL_WAIT_UPDATE_START_INT:
      enroll_wait_update_start_int (ssm, self);
      break;

    case ENROLL_DB_WRITE_ENABLE:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case ENROLL_DB_WRITE_ENABLE_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        fp_warn ("db_write_enable (1st) failed: status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_APPEND_IMAGE:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_enrollment_update (
          self->enroll_template, self->enroll_template_len, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_APPEND_IMAGE_RECV:
      enroll_append_image_recv (ssm, self);
      break;

    case ENROLL_CLEANUPS:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_CLEANUPS_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK &&
          self->cmd_response_status != 0x0491)
        fp_warn ("call_cleanups (1st) failed: status=0x%04x",
                 self->cmd_response_status);
      /* If the trigger enrollment_update was rejected, cleanups have now run
       * (matching python's finally); skip the wait-int and second update and go
       * straight to update-end so LOOP_CHECK retries this stage. */
      if (self->enroll_frame_rejected)
        fpi_ssm_jump_to_state (ssm, ENROLL_UPDATE_END);
      else
        fpi_ssm_next_state (ssm);
      break;

    case ENROLL_WAIT_UPDATE_INT:
      enroll_wait_update_int (ssm, self);
      break;

    case ENROLL_DB_WRITE_ENABLE_READ:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case ENROLL_DB_WRITE_ENABLE_READ_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        fp_warn ("db_write_enable (2nd) failed: status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_APPEND_IMAGE_READ:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_enrollment_update (
          self->enroll_template, self->enroll_template_len, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_APPEND_IMAGE_READ_RECV:
      enroll_append_image_read_recv (ssm, self);
      break;

    case ENROLL_CLEANUPS_READ:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_CLEANUPS_READ_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK &&
          self->cmd_response_status != 0x0491)
        fp_warn ("call_cleanups (2nd) failed: status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_UPDATE_END:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_create_enrollment (FALSE, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_UPDATE_END_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_LOOP_CHECK:
      enroll_loop_check (ssm, self);
      break;

    case ENROLL_UPDATE_END2:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_create_enrollment (FALSE, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_UPDATE_END2_RECV:
      /* Ignore status — PY doesn't check it for the second call either */
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_GET_STORAGE:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_get_user_storage (
          VALIDITY_STORAGE_NAME, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_GET_STORAGE_RECV:
      enroll_get_storage_recv (ssm, self);
      break;

    case ENROLL_INIT_STORAGE_WE:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case ENROLL_INIT_STORAGE_WE_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        fp_warn ("db_write_enable (init_storage) failed: 0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_INIT_STORAGE_CREATE:
      {
        const gchar *name = VALIDITY_STORAGE_NAME;
        gsize name_len = strlen (name) + 1;
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_new_record (
          1, 4, 3, (const guint8 *) name, name_len, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_INIT_STORAGE_CREATE_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("create storage failed: status=0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fp_info ("StgWindsor storage created successfully");
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_INIT_STORAGE_CLEAN:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_INIT_STORAGE_CLEAN_RECV:
      /* Now retry get_user_storage to get the dbid */
      fpi_ssm_jump_to_state (ssm, ENROLL_GET_STORAGE);
      break;

    case ENROLL_DB_WRITE_ENABLE2:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case ENROLL_DB_WRITE_ENABLE2_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("db_write_enable for finger creation failed: 0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_CREATE_USER:
      enroll_create_user (ssm, self);
      break;

    case ENROLL_CREATE_USER_RECV:
      enroll_create_user_recv (ssm, self);
      break;

    case ENROLL_CREATE_USER_CLEANUPS:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_CREATE_USER_CLEANUPS_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_DB_WRITE_ENABLE3:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case ENROLL_DB_WRITE_ENABLE3_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("db_write_enable3 failed: 0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_CREATE_FINGER:
      enroll_create_finger (ssm, self);
      break;

    case ENROLL_CREATE_FINGER_RECV:
      enroll_create_finger_recv (ssm, self);
      break;

    case ENROLL_FINAL_CLEANUPS:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case ENROLL_FINAL_CLEANUPS_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_WAIT_FINGER_INT:
      enroll_wait_finger_int (ssm, self);
      break;

    case ENROLL_LED_OFF:
      {
        gsize cmd_len;
        const guint8 *cmd = validity_capture_glow_end_cmd (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case ENROLL_LED_OFF_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case ENROLL_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
enroll_reconnect_done (FpiDeviceValidity *self,
                       gboolean           success,
                       gpointer           user_data)
{
  FpDevice *dev = FP_DEVICE (self);

  (void) user_data;

  if (success)
    {
      fp_info ("Reconnected -- restarting enrollment");
      self->recovery_redispatch = TRUE;
      validity_enroll (dev);
      return;
    }

  fp_warn ("Reconnect failed -- reporting enroll removed");
  fpi_device_enroll_complete (dev, NULL,
                              fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
}

static void
enroll_ssm_done (FpiSsm   *ssm,
                 FpDevice *dev,
                 GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      g_clear_pointer (&self->enroll_template, g_free);
      self->enroll_template_len = 0;
      g_clear_pointer (&self->bulk_data, g_free);
      self->bulk_data_len = 0;

      /* Recoverable mid-operation reset? Reconnect and restart enroll instead
       * of failing the action (bounded; a true removal falls through). The
       * on-chip enroll session is lost on reset, so this restarts from stage 0
       * rather than resuming -- but the action does not fail. */
      if (validity_recover_or_fail (self, error, enroll_reconnect_done, NULL))
        {
          g_error_free (error);
          return;
        }

      fpi_device_enroll_complete (dev, NULL, validity_map_removed_error (error));
      return;
    }

  /* Build FpPrint for the enrolled finger */
  FpPrint *print = NULL;

  fpi_device_get_enroll_data (dev, &print);

  /* Set the print metadata */
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);

  /* Store the user ID as driver data for later verify/identify.
   * The RAW data GVariant is required for serialization. */
  GVariant *user_id_var = g_object_get_data (G_OBJECT (print),
                                             "validity-user-id");
  if (user_id_var)
    {
      const gchar *uid = g_variant_get_string (user_id_var, NULL);
      GVariant *data = g_variant_new_string (uid);
      g_object_set (print, "fpi-data", data, NULL);

      GDate *date = g_date_new ();
      g_date_set_time_t (date, time (NULL));
      fp_print_set_enroll_date (print, date);
      g_date_free (date);
    }
  else
    {
      /* Fallback: store an empty marker */
      GVariant *data = g_variant_new_string ("");
      g_object_set (print, "fpi-data", data, NULL);
    }

  g_clear_pointer (&self->enroll_template, g_free);
  self->enroll_template_len = 0;
  g_clear_pointer (&self->bulk_data, g_free);
  self->bulk_data_len = 0;

  enroll_report_progress (self,
                          validity_enroll_progress_target (self->enroll_reported,
                                                           self->enroll_stage,
                                                           TRUE));
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

/* ================================================================
 * Emulation mode: virtual enroll
 * ================================================================ */

static void
emulation_enroll (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpPrint *print = NULL;

  fpi_device_get_enroll_data (device, &print);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);

  /* Store serialisable data so the print survives serialisation */
  g_autofree gchar *user_id = fpi_print_generate_user_id (print);
  GVariant *data = g_variant_new_string (user_id);

  g_object_set (print, "fpi-data", data, NULL);

  /* Simulate progress for each enroll stage */
  for (int i = 0; i < VALIDITY_ENROLL_STAGES - 1; i++)
    fpi_device_enroll_progress (device, i, NULL, NULL);

  /* Store in emulation memory */
  g_ptr_array_add (self->emulation_prints, g_object_ref (print));

  fpi_device_enroll_complete (device, g_object_ref (print), NULL);
}

void
validity_enroll (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_enroll (device);
      return;
    }

  if (self->recovery_redispatch)
    {
      self->recovery_redispatch = FALSE; /* internal retry -- keep the budget */
    }
  else
    {
      self->reconnect_attempts = 0;      /* fresh action -- reset the budget */
      self->enroll_reported = 0;         /* and the reported progress */
    }

  ssm = fpi_ssm_new (device, enroll_run_state, ENROLL_NUM_STATES);
  fpi_ssm_start (ssm, enroll_ssm_done);
}
