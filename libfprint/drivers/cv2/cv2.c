/*
 * Broadcom ControlVault2 (BCM5880) fingerprint driver
 *
 * Copyright (C) 2026 Matt Trent <matt@thetrents.org>
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

/*
 * Match-on-chip driver for the fingerprint function of the Dell ControlVault2
 * (Broadcom BCM5880, USB 0a5c:5834). Only interface 0 is used; the
 * smart-card interfaces are left alone.
 *
 * The protocol was worked out from USB captures of the reader. The command
 * that lists stored templates (enumerate) appears in no capture: its
 * arguments come from Broadcom's public Citadel SDK (Apache-2.0), checked by
 * probing the reader.
 *
 * Security: templates are stored and matched inside the chip, and no
 * fingerprint images or templates cross USB. The plaintext interface used
 * here does not authenticate replies, so the host trusts the reader's match
 * result, as with other match-on-chip drivers in libfprint.
 */

#define FP_COMPONENT "cv2"

#include <string.h>

#include "drivers_api.h"

#include "cv2-proto.h"
#include "cv2.h"

#define CV2_INTERFACE 0
#define CV2_EP_OUT (0x01 | FPI_USB_ENDPOINT_OUT)
#define CV2_EP_IN (0x01 | FPI_USB_ENDPOINT_IN)
#define CV2_EP_INT (0x05 | FPI_USB_ENDPOINT_IN)
#define CV2_CMD_TIMEOUT_MS 5000
#define CV2_MAX_STALE 2
#define CV2_ENROLL_STAGES 4
/* Give up an enrollment that has not finished after this many accepted
 * samples (4 are normal; a degraded chip never finishes) */
#define CV2_ENROLL_MAX_SAMPLES 30

/* State of the operation in progress. cv2_start_task () sets the inputs;
 * everything else starts at zero, and cv2_task_complete () clears it all. */
typedef struct
{
  /* Inputs */
  GArray  *templates;          /* verify, identify: guint32 handles to match;
                                * list: the handles found */
  guint32  delete_id;          /* delete: the handle to delete */
  gboolean enroll;             /* enroll: begin the enrollment before each capture */

  guint32  session;
  gboolean session_open;
  gboolean enrolling;          /* an enrollment is open on the chip */
  gboolean capture_pending;    /* the chip holds a capture to cancel first */
  gboolean capture_retried;    /* this capture already canceled a pending one */
  guint8   capture_id[CV2_CAPTURE_ID_LEN];
  guint8   result_id[CV2_CAPTURE_ID_LEN];
  gint     enroll_stage;
  guint    accepted_samples;
  guint32  enrolled_id;        /* enroll: the new template's handle */
  gboolean match_one_by_one;   /* only templates[match_pos] is being matched */
  guint    match_pos;
  guint    match_missing;      /* handles found missing one by one */
} Cv2Op;

struct _FpiDeviceCv2
{
  FpDevice parent;

  gchar   *fw_version;     /* USH_REL_UPGRADE_VER, NULL if unknown */

  FpiSsm  *task_ssm;
  GError  *task_error;     /* held while cleanup runs */
  Cv2Op    op;
};

G_DEFINE_TYPE (FpiDeviceCv2, fpi_device_cv2, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x0a5c, .pid = 0x5834, },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

/* ---- Transport ----
 *
 * A command is: request on bulk OUT, an interrupt saying a reply of N bytes
 * is ready, then the reply on bulk IN. Command transfers are never
 * canceled half-way, as that would leave a reply behind for the next
 * command; cancellation is checked before sending instead.
 */

/* Called once per command. @reply is NULL if no usable reply arrived (a USB
 * error or a malformed message), and @outcome is then CV2_OUTCOME_ERROR.
 * Otherwise @outcome classifies the reply's status, see
 * cv2_status_classify (). @error is set unless @outcome is OK or NO_MATCH,
 * and the callback owns it; the transport keeps @reply. */
typedef void (*Cv2CmdCallback) (FpiDeviceCv2   *self,
                                FpiSsm         *ssm,
                                Cv2Outcome      outcome,
                                const Cv2Reply *reply,
                                GError         *error);

typedef struct
{
  guint16        cmd;
  GByteArray    *request;
  FpiSsm        *caller_ssm;
  Cv2CmdCallback callback;
  Cv2Reply      *reply;
  guint          stale;
} Cv2CmdData;

enum {
  CMD_SEND,
  CMD_WAIT_REPLY,
  CMD_RECV,
  CMD_NUM_STATES,
};

static void
cmd_data_free (gpointer user_data)
{
  Cv2CmdData *data = user_data;

  g_clear_pointer (&data->request, g_byte_array_unref);
  g_clear_pointer (&data->reply, cv2_reply_free);
  g_free (data);
}

static void
cmd_int_cb (FpiUsbTransfer *transfer,
            FpDevice       *dev,
            gpointer        user_data,
            GError         *error)
{
  FpiSsm *ssm = transfer->ssm;
  Cv2IntEvent event;

  if (error ||
      !cv2_int_event_parse (transfer->buffer, transfer->actual_length, &event, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  switch (event.type)
    {
    case CV2_INT_REPLY:
      fpi_ssm_next_state (ssm);
      break;

    case CV2_INT_FINGER:
      fp_dbg ("Ignoring a finger event while waiting for a reply");
      fpi_ssm_jump_to_state (ssm, CMD_WAIT_REPLY);
      break;

    default:
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected interrupt message (type %u)",
                                                     event.type));
    }
}

static void
cmd_recv_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiSsm *ssm = transfer->ssm;
  Cv2CmdData *data = fpi_ssm_get_data (ssm);
  GError *local_error = NULL;
  guint16 cmd;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* A reply left over from a timed-out command or an earlier client. Our
   * own reply is queued behind it, so read again. (If the stale reply's
   * interrupt was still queued, ours is now left over; later reads skip such
   * extra reply-ready events.) */
  if (cv2_message_peek_cmd (transfer->buffer, transfer->actual_length, &cmd) &&
      cmd != data->cmd && data->stale < CV2_MAX_STALE)
    {
      fp_dbg ("Discarding a stale reply to 0x%02x", cmd);
      data->stale++;
      fpi_ssm_jump_to_state (ssm, CMD_RECV);
      return;
    }

  data->reply = cv2_reply_parse (transfer->buffer, transfer->actual_length,
                                 data->cmd, &local_error);
  if (!data->reply)
    {
      fpi_ssm_mark_failed (ssm, local_error);
      return;
    }

  fp_dbg ("< 0x%02x status 0x%02x", data->cmd, data->reply->status);
  fpi_ssm_mark_completed (ssm);
}

static void
cmd_run_state (FpiSsm *ssm, FpDevice *dev)
{
  Cv2CmdData *data = fpi_ssm_get_data (ssm);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  gsize len;

  transfer->ssm = ssm;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CMD_SEND:
      fp_dbg ("> 0x%02x", data->cmd);
      len = data->request->len;
      fpi_usb_transfer_fill_bulk_full (transfer, CV2_EP_OUT,
                                       g_byte_array_free (g_steal_pointer (&data->request), FALSE),
                                       len, g_free);
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_submit (transfer, CV2_CMD_TIMEOUT_MS, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case CMD_WAIT_REPLY:
      fpi_usb_transfer_fill_interrupt (transfer, CV2_EP_INT, CV2_INT_BUF_LEN);
      fpi_usb_transfer_submit (transfer, CV2_CMD_TIMEOUT_MS, NULL, cmd_int_cb, NULL);
      break;

    case CMD_RECV:
      fpi_usb_transfer_fill_bulk (transfer, CV2_EP_IN, CV2_REPLY_BUF_LEN);
      fpi_usb_transfer_submit (transfer, CV2_CMD_TIMEOUT_MS, NULL, cmd_recv_cb, NULL);
      break;
    }
}

static void
cmd_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  Cv2CmdData *data = fpi_ssm_get_data (ssm);
  Cv2Outcome outcome = CV2_OUTCOME_ERROR;

  if (!error)
    {
      /* A command that completed has a reply */
      g_assert (data->reply);
      outcome = cv2_status_classify (data->reply->cmd, data->reply->status, &error);
    }

  data->callback (FPI_DEVICE_CV2 (dev), data->caller_ssm, outcome, data->reply, error);
}

static void
cv2_cmd_full (FpiDeviceCv2  *self,
              FpiSsm        *ssm,
              GByteArray    *request,
              gboolean       check_cancel,
              Cv2CmdCallback callback)
{
  FpDevice *dev = FP_DEVICE (self);
  Cv2CmdData *data;
  FpiSsm *cmd_ssm;
  guint16 cmd = 0;

  if (check_cancel && fpi_device_action_is_cancelled (dev))
    {
      GError *error = NULL;

      g_cancellable_set_error_if_cancelled (fpi_device_get_cancellable (dev), &error);
      g_byte_array_unref (request);
      callback (self, ssm, CV2_OUTCOME_ERROR, NULL, error);
      return;
    }

  cv2_message_peek_cmd (request->data, request->len, &cmd);

  data = g_new0 (Cv2CmdData, 1);
  data->cmd = cmd;
  data->request = request;
  data->caller_ssm = ssm;
  data->callback = callback;

  cmd_ssm = fpi_ssm_new_full (dev, cmd_run_state, CMD_NUM_STATES, CMD_NUM_STATES, "cv2-cmd");
  fpi_ssm_set_data (cmd_ssm, data, cmd_data_free);
  fpi_ssm_silence_debug (cmd_ssm);
  fpi_ssm_start (cmd_ssm, cmd_ssm_done);
}

/* Sends a step of the current operation. If the operation was canceled,
 * nothing is sent and the callback gets the cancellation error instead. */
static void
cv2_cmd (FpiDeviceCv2  *self,
         FpiSsm        *ssm,
         GByteArray    *request,
         Cv2CmdCallback callback)
{
  cv2_cmd_full (self, ssm, request, TRUE, callback);
}

/* Sends even if the operation was canceled: for cleanup, and while opening. */
static void
cv2_cmd_always (FpiDeviceCv2  *self,
                FpiSsm        *ssm,
                GByteArray    *request,
                Cv2CmdCallback callback)
{
  cv2_cmd_full (self, ssm, request, FALSE, callback);
}

/* For steps whose failure doesn't change the result */
static void
log_failure (const char *step, GError *error)
{
  if (error)
    fp_info ("%s failed: %s", step, error->message);

  g_clear_error (&error);
}

/* ---- Prints ----
 *
 * A print stores the chip's template handle as "(u)".
 */

static void
cv2_print_set_template (FpPrint *print, guint32 template_id)
{
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", g_variant_new ("(u)", template_id), NULL);
}

static gboolean
cv2_print_get_template (FpPrint *print, guint32 *template_id)
{
  g_autoptr(GVariant) data = NULL;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_check_format_string (data, "(u)", FALSE))
    return FALSE;

  g_variant_get (data, "(u)", template_id);
  return TRUE;
}

/* ---- Operation lifecycle ----
 *
 * Each operation opens a chip session, runs, and closes it. If it fails or
 * is canceled while a session is open, a cleanup machine cancels any
 * capture, discards an unfinished enrollment and closes the session before
 * the original error is reported.
 */

static void task_ssm_done (FpiSsm   *ssm,
                           FpDevice *dev,
                           GError   *error);

static void
cv2_op_clear (Cv2Op *op)
{
  g_clear_pointer (&op->templates, g_array_unref);
  memset (op, 0, sizeof (*op));
}

/* Starts an operation. @inputs holds its inputs, the rest of it is zero;
 * takes @inputs->templates. */
static void
cv2_start_task (FpiDeviceCv2         *self,
                FpiSsmHandlerCallback handler,
                int                   nr_states,
                const char           *name,
                const Cv2Op          *inputs)
{
  g_assert (self->task_ssm == NULL);

  cv2_op_clear (&self->op);
  self->op = *inputs;

  self->task_ssm = fpi_ssm_new_full (FP_DEVICE (self), handler, nr_states, nr_states, name);
  fpi_ssm_start (self->task_ssm, task_ssm_done);
}

static void
cv2_task_complete (FpiDeviceCv2 *self, GError *error)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  guint32 enrolled_id = self->op.enrolled_id;

  g_autoptr(GArray) listed = NULL;
  g_autoptr(GPtrArray) prints = NULL;
  FpPrint *print;

  if (action == FPI_DEVICE_ACTION_LIST)
    listed = g_steal_pointer (&self->op.templates);

  cv2_op_clear (&self->op);
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      if (error)
        {
          fpi_device_enroll_complete (dev, NULL, error);
          return;
        }

      fpi_device_get_enroll_data (dev, &print);
      cv2_print_set_template (print, enrolled_id);
      fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
    }
  else if (action == FPI_DEVICE_ACTION_VERIFY)
    {
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
          fpi_device_verify_complete (dev, NULL);
          return;
        }
      fpi_device_verify_complete (dev, error);
    }
  else if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_identify_report (dev, NULL, NULL, g_steal_pointer (&error));
          fpi_device_identify_complete (dev, NULL);
          return;
        }
      fpi_device_identify_complete (dev, error);
    }
  else if (action == FPI_DEVICE_ACTION_DELETE)
    {
      fpi_device_delete_complete (dev, error);
    }
  else if (action == FPI_DEVICE_ACTION_LIST)
    {
      if (error)
        {
          fpi_device_list_complete (dev, NULL, error);
          return;
        }

      prints = g_ptr_array_new_full (listed->len, g_object_unref);
      for (guint i = 0; i < listed->len; i++)
        {
          print = fp_print_new (dev);
          cv2_print_set_template (print, g_array_index (listed, guint32, i));
          g_ptr_array_add (prints, g_object_ref_sink (print));
        }
      fpi_device_list_complete (dev, g_steal_pointer (&prints), NULL);
    }
  else
    {
      g_assert_not_reached ();
    }
}

enum {
  CLEANUP_CANCEL_CAPTURE,
  CLEANUP_DISCARD_ENROLLMENT,
  CLEANUP_CLOSE_SESSION,
  CLEANUP_NUM_STATES,
};

static void
cleanup_cmd_cb (FpiDeviceCv2   *self,
                FpiSsm         *ssm,
                Cv2Outcome      outcome,
                const Cv2Reply *reply,
                GError         *error)
{
  log_failure ("Cleanup step", error);
  fpi_ssm_next_state (ssm);
}

static void
cleanup_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CLEANUP_CANCEL_CAPTURE:
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_CAPTURE_CANCEL), cleanup_cmd_cb);
      break;

    case CLEANUP_DISCARD_ENROLLMENT:
      if (!self->op.enrolling)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_ENROLL_DISCARD), cleanup_cmd_cb);
      break;

    case CLEANUP_CLOSE_SESSION:
      cv2_cmd_always (self, ssm, cv2_build_close_session (self->op.session), cleanup_cmd_cb);
      break;
    }
}

static void
cleanup_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  /* Cleanup steps never fail the machine; see cleanup_cmd_cb */
  g_clear_error (&error);
  cv2_task_complete (self, g_steal_pointer (&self->task_error));
}

static void
task_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  FpiSsm *cleanup;

  self->task_ssm = NULL;

  if (error && self->op.session_open)
    {
      fp_dbg ("Operation failed (%s), cleaning up", error->message);
      self->task_error = error;
      cleanup = fpi_ssm_new_full (dev, cleanup_run_state, CLEANUP_NUM_STATES,
                                  CLEANUP_NUM_STATES, "cv2-cleanup");
      fpi_ssm_start (cleanup, cleanup_done);
      return;
    }

  cv2_task_complete (self, error);
}

static void
open_session_cb (FpiDeviceCv2   *self,
                 FpiSsm         *ssm,
                 Cv2Outcome      outcome,
                 const Cv2Reply *reply,
                 GError         *error)
{
  if (error || !cv2_reply_get_session (reply, &self->op.session, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->op.session_open = TRUE;
  fpi_ssm_next_state (ssm);
}

/* The operation already succeeded, so a failed close is only logged. */
static void
close_session_cb (FpiDeviceCv2   *self,
                  FpiSsm         *ssm,
                  Cv2Outcome      outcome,
                  const Cv2Reply *reply,
                  GError         *error)
{
  log_failure ("Closing the session", error);
  self->op.session_open = FALSE;
  fpi_ssm_next_state (ssm);
}

/* ---- Capture ----
 *
 * Starts a capture and waits for a finger. A capture the chip still holds
 * (left by another client, or by a touch too short to use) is canceled
 * first. A touch too short to use fails the capture with a retry error;
 * the operation decides what a retry means.
 *
 * When enrolling, 0x8a (begin enrollment) comes right before every capture,
 * as the vendor's Windows driver sends it. It keeps the samples already
 * accepted.
 */

enum {
  CAPTURE_CANCEL_PENDING,
  CAPTURE_ENROLL_BEGIN,
  CAPTURE_START,
  CAPTURE_WAIT_FINGER,
  CAPTURE_NUM_STATES,
};

static void
capture_cancel_pending_cb (FpiDeviceCv2   *self,
                           FpiSsm         *ssm,
                           Cv2Outcome      outcome,
                           const Cv2Reply *reply,
                           GError         *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->op.capture_pending = FALSE;
  fpi_ssm_next_state (ssm);
}

static void
capture_enroll_begin_cb (FpiDeviceCv2   *self,
                         FpiSsm         *ssm,
                         Cv2Outcome      outcome,
                         const Cv2Reply *reply,
                         GError         *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->op.enrolling = TRUE;
  fpi_ssm_next_state (ssm);
}

static void
capture_start_cb (FpiDeviceCv2   *self,
                  FpiSsm         *ssm,
                  Cv2Outcome      outcome,
                  const Cv2Reply *reply,
                  GError         *error)
{
  if (outcome == CV2_OUTCOME_CAPTURE_PENDING && !self->op.capture_retried)
    {
      fp_dbg ("A capture is still pending; canceling it and retrying");
      g_clear_error (&error);
      self->op.capture_pending = TRUE;
      self->op.capture_retried = TRUE;
      fpi_ssm_jump_to_state (ssm, CAPTURE_CANCEL_PENDING);
      return;
    }

  if (error || !cv2_reply_get_capture_id (reply, self->op.capture_id, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->op.capture_retried = FALSE;
  fpi_ssm_next_state (ssm);
}

static void
wait_finger_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  Cv2IntEvent event;

  if (error ||
      !cv2_int_event_parse (transfer->buffer, transfer->actual_length, &event, &error))
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (event.type == CV2_INT_REPLY)
    {
      /* Left over after a stale reply was skipped; keep waiting */
      fp_dbg ("Ignoring a leftover reply-ready event while waiting for a finger");
      fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_WAIT_FINGER);
      return;
    }

  if (event.type != CV2_INT_FINGER)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Expected a finger event, got interrupt type %u",
                                                     event.type));
      return;
    }

  /* A nonzero value means the chip got no usable capture: seen when the
   * finger touches only briefly. The chip still holds the capture. */
  if (event.value != 0)
    {
      fp_info ("Finger lifted too soon (finger event value %u)", event.value);
      self->op.capture_pending = TRUE;
      fpi_ssm_mark_failed (transfer->ssm, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }

  fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                           FP_FINGER_STATUS_NEEDED);
  fpi_ssm_mark_completed (transfer->ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_CANCEL_PENDING:
      if (!self->op.capture_pending)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      cv2_cmd (self, ssm, cv2_build_simple (CV2_CMD_CAPTURE_CANCEL), capture_cancel_pending_cb);
      break;

    case CAPTURE_ENROLL_BEGIN:
      if (!self->op.enroll)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      cv2_cmd (self, ssm, cv2_build_simple (CV2_CMD_ENROLL_BEGIN), capture_enroll_begin_cb);
      break;

    case CAPTURE_START:
      cv2_cmd (self, ssm, cv2_build_capture_start (self->op.session), capture_start_cb);
      break;

    case CAPTURE_WAIT_FINGER:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_interrupt (transfer, CV2_EP_INT, CV2_INT_BUF_LEN);
      fpi_usb_transfer_submit (transfer, 0, fpi_device_get_cancellable (dev),
                               wait_finger_cb, NULL);
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      break;
    }
}

static FpiSsm *
capture_ssm_new (FpiDeviceCv2 *self)
{
  return fpi_ssm_new_full (FP_DEVICE (self), capture_run_state, CAPTURE_NUM_STATES,
                           CAPTURE_NUM_STATES, "cv2-capture");
}

/* ---- Enroll ---- */

enum {
  ENROLL_OPEN_SESSION,
  ENROLL_CAPTURE,
  ENROLL_SAMPLE,
  ENROLL_COMMIT,
  ENROLL_CLOSE_SESSION,
  ENROLL_NUM_STATES,
};

/* No usable sample (a touch too short, or a rejected sample): ask for
 * another press and capture again. Takes @error, a retry error. */
static void
enroll_retry (FpiDeviceCv2 *self, GError *error)
{
  fpi_device_enroll_progress (FP_DEVICE (self), self->op.enroll_stage, NULL, error);
  fpi_ssm_jump_to_state (self->task_ssm, ENROLL_CAPTURE);
}

static void
enroll_capture_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  if (error && error->domain == FP_DEVICE_RETRY)
    enroll_retry (self, error);
  else if (error)
    fpi_ssm_mark_failed (self->task_ssm, error);
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
enroll_sample_cb (FpiDeviceCv2   *self,
                  FpiSsm         *ssm,
                  Cv2Outcome      outcome,
                  const Cv2Reply *reply,
                  GError         *error)
{
  FpDevice *dev = FP_DEVICE (self);
  gboolean done = FALSE;

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (outcome == CV2_OUTCOME_RETRY)
    {
      enroll_retry (self, error);
      return;
    }

  if (error || !cv2_reply_get_sample (reply, &done, self->op.result_id, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* Stages advance per accepted sample, but never reach the last stage
   * until the reader says the enrollment is complete. */
  if (done)
    self->op.enroll_stage = CV2_ENROLL_STAGES;
  else
    self->op.enroll_stage = MIN (self->op.enroll_stage + 1, CV2_ENROLL_STAGES - 1);
  fpi_device_enroll_progress (dev, self->op.enroll_stage, NULL, NULL);

  if (done)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  if (++self->op.accepted_samples >= CV2_ENROLL_MAX_SAMPLES)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Enrollment did not finish after %u samples. "
                                                     "Clean the sensor and try again.",
                                                     self->op.accepted_samples));
      return;
    }

  fpi_ssm_jump_to_state (ssm, ENROLL_CAPTURE);
}

static void
enroll_commit_cb (FpiDeviceCv2   *self,
                  FpiSsm         *ssm,
                  Cv2Outcome      outcome,
                  const Cv2Reply *reply,
                  GError         *error)
{
  /* 0x24: the firmware rejected the commit's attributes; name its version */
  if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED))
    {
      g_clear_error (&error);
      error = cv2_firmware_error (self->fw_version);
    }

  if (error || !cv2_reply_get_template (reply, &self->op.enrolled_id, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->op.enrolling = FALSE;
  fp_info ("Enrolled template 0x%08x", self->op.enrolled_id);
  fpi_ssm_next_state (ssm);
}

static void
enroll_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_OPEN_SESSION:
      cv2_cmd (self, ssm, cv2_build_open_session (), open_session_cb);
      break;

    case ENROLL_CAPTURE:
      /* Not a sub-machine: a retry error must not fail the enrollment */
      fpi_ssm_start (capture_ssm_new (self), enroll_capture_done);
      break;

    case ENROLL_SAMPLE:
      cv2_cmd (self, ssm, cv2_build_enroll_sample (self->op.session, self->op.capture_id),
               enroll_sample_cb);
      break;

    case ENROLL_COMMIT:
      cv2_cmd (self, ssm, cv2_build_enroll_commit (self->op.session, self->op.result_id),
               enroll_commit_cb);
      break;

    case ENROLL_CLOSE_SESSION:
      cv2_cmd_always (self, ssm, cv2_build_close_session (self->op.session), close_session_cb);
      break;
    }
}

static void
cv2_enroll (FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  cv2_start_task (self, enroll_run_state, ENROLL_NUM_STATES, "cv2-enroll",
                  &(Cv2Op){ .enroll = TRUE });
}

/* ---- Verify and identify ----
 *
 * Verify is identify against one print: one capture, then one match command
 * with every template handle.
 *
 * If any handle is no longer on the chip (deleted elsewhere, or the chip was
 * wiped), the reader fails the whole list. The capture stays usable, so the
 * handles are then matched one at a time, skipping the missing ones. If none
 * of them is on the chip, the result is DATA_NOT_FOUND, as for a single print.
 */

enum {
  MATCH_OPEN_SESSION,
  MATCH_CAPTURE,
  MATCH_MATCH,
  MATCH_CANCEL_CAPTURE,
  MATCH_CLOSE_SESSION,
  MATCH_NUM_STATES,
};

static FpPrint *
match_print_at (FpiDeviceCv2 *self, guint pos)
{
  FpDevice *dev = FP_DEVICE (self);
  GPtrArray *prints;
  FpPrint *print;

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    {
      fpi_device_get_verify_data (dev, &print);
      return print;
    }

  fpi_device_get_identify_data (dev, &prints);
  return g_ptr_array_index (prints, pos);
}

/* Which template the reader matched, or -1 for a handle that was not sent */
static gint
match_find (const Cv2Op *op, guint32 template_id)
{
  if (op->match_one_by_one)
    return g_array_index (op->templates, guint32, op->match_pos) == template_id ?
           (gint) op->match_pos : -1;

  for (guint i = 0; i < op->templates->len; i++)
    if (g_array_index (op->templates, guint32, i) == template_id)
      return i;

  return -1;
}

static void
match_report (FpiDeviceCv2 *self, FpiSsm *ssm, FpPrint *match)
{
  FpDevice *dev = FP_DEVICE (self);

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_report (dev, match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL, NULL, NULL);
  else
    fpi_device_identify_report (dev, match, NULL, NULL);

  fpi_ssm_next_state (ssm);
}

/* Matches the next handle on its own, or ends the search once every handle
 * has been tried. */
static void
match_next (FpiDeviceCv2 *self, FpiSsm *ssm)
{
  Cv2Op *op = &self->op;
  guint next = op->match_one_by_one ? op->match_pos + 1 : 0;

  if (next < op->templates->len)
    {
      op->match_one_by_one = TRUE;
      op->match_pos = next;
      fpi_ssm_jump_to_state (ssm, MATCH_MATCH);
      return;
    }

  if (op->match_missing == op->templates->len)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                                     "None of the templates are on the reader"));
      return;
    }

  match_report (self, ssm, NULL);
}

static void
match_cb (FpiDeviceCv2   *self,
          FpiSsm         *ssm,
          Cv2Outcome      outcome,
          const Cv2Reply *reply,
          GError         *error)
{
  Cv2Op *op = &self->op;
  gboolean matched = FALSE;
  guint32 template_id = 0;
  gint pos;

  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);

  /* A handle is missing: skip it, or start matching one at a time. A lone
   * handle has nothing to skip, so it fails as not found below. */
  if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_NOT_FOUND) &&
      op->templates->len > 1)
    {
      if (op->match_one_by_one)
        {
          fp_info ("Template 0x%08x is not on the reader, skipping it",
                   g_array_index (op->templates, guint32, op->match_pos));
          op->match_missing++;
        }
      else
        {
          fp_info ("A template is not on the reader, matching one at a time");
        }

      g_clear_error (&error);
      match_next (self, ssm);
      return;
    }

  /* Including a retry error, e.g. no usable capture */
  if (error ||
      (outcome == CV2_OUTCOME_OK &&
       !cv2_reply_get_match (reply, &matched, &template_id, &error)))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!matched)
    {
      if (op->match_one_by_one)
        match_next (self, ssm);
      else
        match_report (self, ssm, NULL);
      return;
    }

  pos = match_find (op, template_id);
  if (pos < 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Reader matched template 0x%08x, which was not requested",
                                                     template_id));
      return;
    }

  match_report (self, ssm, match_print_at (self, pos));
}

/* The result is already reported, so a failed cancel is only logged. */
static void
match_cancel_capture_cb (FpiDeviceCv2   *self,
                         FpiSsm         *ssm,
                         Cv2Outcome      outcome,
                         const Cv2Reply *reply,
                         GError         *error)
{
  log_failure ("Canceling the capture", error);
  fpi_ssm_next_state (ssm);
}

static void
match_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  Cv2Op *op = &self->op;
  GByteArray *request;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MATCH_OPEN_SESSION:
      cv2_cmd (self, ssm, cv2_build_open_session (), open_session_cb);
      break;

    case MATCH_CAPTURE:
      fpi_ssm_start_subsm (ssm, capture_ssm_new (self));
      break;

    case MATCH_MATCH:
      if (op->match_one_by_one)
        request = cv2_build_match (op->session,
                                   &g_array_index (op->templates, guint32, op->match_pos), 1);
      else
        request = cv2_build_match (op->session, (const guint32 *) op->templates->data,
                                   op->templates->len);
      cv2_cmd (self, ssm, request, match_cb);
      break;

    case MATCH_CANCEL_CAPTURE:
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_CAPTURE_CANCEL), match_cancel_capture_cb);
      break;

    case MATCH_CLOSE_SESSION:
      cv2_cmd_always (self, ssm, cv2_build_close_session (op->session), close_session_cb);
      break;
    }
}

static void
cv2_verify_identify (FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  gboolean verify = fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY;

  g_autoptr(GArray) templates = g_array_new (FALSE, FALSE, sizeof (guint32));
  GPtrArray *prints = NULL;
  FpPrint *print = NULL;
  guint32 template_id;

  if (verify)
    {
      fpi_device_get_verify_data (dev, &print);
      if (!cv2_print_get_template (print, &template_id))
        {
          fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }
      g_array_append_val (templates, template_id);
    }
  else
    {
      fpi_device_get_identify_data (dev, &prints);
      for (guint i = 0; i < prints->len; i++)
        {
          if (!cv2_print_get_template (g_ptr_array_index (prints, i), &template_id))
            {
              fpi_device_identify_complete (dev,
                                            fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
              return;
            }
          g_array_append_val (templates, template_id);
        }

      if (templates->len == 0)
        {
          fpi_device_identify_report (dev, NULL, NULL, NULL);
          fpi_device_identify_complete (dev, NULL);
          return;
        }
    }

  cv2_start_task (self, match_run_state, MATCH_NUM_STATES, "cv2-match",
                  &(Cv2Op){ .templates = g_steal_pointer (&templates) });
}

/* ---- Delete ---- */

enum {
  DELETE_OPEN_SESSION,
  DELETE_TEMPLATE,
  DELETE_CLOSE_SESSION,
  DELETE_NUM_STATES,
};

static void
delete_template_cb (FpiDeviceCv2   *self,
                    FpiSsm         *ssm,
                    Cv2Outcome      outcome,
                    const Cv2Reply *reply,
                    GError         *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_info ("Deleted template 0x%08x", self->op.delete_id);
  fpi_ssm_next_state (ssm);
}

static void
delete_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DELETE_OPEN_SESSION:
      cv2_cmd (self, ssm, cv2_build_open_session (), open_session_cb);
      break;

    case DELETE_TEMPLATE:
      cv2_cmd (self, ssm, cv2_build_delete (self->op.session, self->op.delete_id),
               delete_template_cb);
      break;

    case DELETE_CLOSE_SESSION:
      cv2_cmd_always (self, ssm, cv2_build_close_session (self->op.session), close_session_cb);
      break;
    }
}

static void
cv2_delete (FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  FpPrint *print = NULL;
  guint32 template_id;

  fpi_device_get_delete_data (dev, &print);
  if (!cv2_print_get_template (print, &template_id))
    {
      fpi_device_delete_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  cv2_start_task (self, delete_run_state, DELETE_NUM_STATES, "cv2-delete",
                  &(Cv2Op){ .delete_id = template_id });
}

/* ---- List ----
 *
 * Lists the fingerprint templates enrolled with this driver's session IDs,
 * which Dell's Linux drivers share, and not those of other owners (e.g.
 * Windows Hello on the same laptop), so fprintd never deletes those. When
 * the reader is full, fprintd may delete listed templates it has no print
 * for, e.g. from Dell's driver or another Linux install. The reader doesn't
 * say which finger a template is for.
 */

enum {
  LIST_OPEN_SESSION,
  LIST_ENUMERATE,
  LIST_CLOSE_SESSION,
  LIST_NUM_STATES,
};

static void
list_enumerate_cb (FpiDeviceCv2   *self,
                   FpiSsm         *ssm,
                   Cv2Outcome      outcome,
                   const Cv2Reply *reply,
                   GError         *error)
{
  if (!error)
    self->op.templates = cv2_reply_get_templates (reply, &error);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_info ("The reader holds %u of our templates", self->op.templates->len);
  fpi_ssm_next_state (ssm);
}

static void
list_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LIST_OPEN_SESSION:
      cv2_cmd (self, ssm, cv2_build_open_session (), open_session_cb);
      break;

    case LIST_ENUMERATE:
      cv2_cmd (self, ssm, cv2_build_enumerate (self->op.session), list_enumerate_cb);
      break;

    case LIST_CLOSE_SESSION:
      cv2_cmd_always (self, ssm, cv2_build_close_session (self->op.session), close_session_cb);
      break;
    }
}

static void
cv2_list (FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  cv2_start_task (self, list_run_state, LIST_NUM_STATES, "cv2-list", &(Cv2Op){ 0 });
}

/* ---- Device open and close ----
 *
 * Opening reads the firmware version and resets the sensor. A reader that
 * does not answer (a USB error) fails the open; a reply that can't be used
 * is only logged.
 */

enum {
  OPEN_GET_VERSION,
  OPEN_SENSOR_RESET,
  OPEN_NUM_STATES,
};

static gboolean
is_transport_error (const GError *error)
{
  return error && error->domain == G_USB_DEVICE_ERROR;
}

static void
open_version_cb (FpiDeviceCv2   *self,
                 FpiSsm         *ssm,
                 Cv2Outcome      outcome,
                 const Cv2Reply *reply,
                 GError         *error)
{
  g_autofree gchar *text = NULL;

  if (is_transport_error (error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!error)
    text = cv2_reply_get_version_text (reply, &error);

  if (text)
    {
      self->fw_version = cv2_version_lookup (text, "USH_REL_UPGRADE_VER");
      fp_info ("ControlVault2 firmware %s",
               self->fw_version ? self->fw_version : "(no USH_REL_UPGRADE_VER)");
    }
  else
    {
      fp_info ("Could not read the firmware version: %s", error->message);
      g_clear_error (&error);
    }

  fpi_ssm_next_state (ssm);
}

static void
open_sensor_reset_cb (FpiDeviceCv2   *self,
                      FpiSsm         *ssm,
                      Cv2Outcome      outcome,
                      const Cv2Reply *reply,
                      GError         *error)
{
  if (is_transport_error (error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  log_failure ("Sensor reset", error);
  fpi_ssm_next_state (ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_VERSION:
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_GET_VERSION), open_version_cb);
      break;

    /* After a suspend the chip can be left in a state where enrollment needs
     * many more samples and nothing matches, with any driver. This command,
     * which the Windows driver sends before each enrollment, restores it and
     * keeps the stored templates. */
    case OPEN_SENSOR_RESET:
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_SENSOR_RESET), open_sensor_reset_cb);
      break;
    }
}

static void
open_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (error)
    g_usb_device_release_interface (fpi_device_get_usb_device (dev), CV2_INTERFACE, 0, NULL);

  fpi_device_open_complete (dev, error);
}

static void
cv2_open (FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     CV2_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  g_clear_pointer (&self->fw_version, g_free);
  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES), open_done);
}

static void
cv2_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  CV2_INTERFACE, 0, &error);
  fpi_device_close_complete (dev, error);
}

/* ---- GObject ---- */

static void
fpi_device_cv2_init (FpiDeviceCv2 *self)
{
}

static void
fpi_device_cv2_finalize (GObject *object)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (object);

  g_clear_pointer (&self->fw_version, g_free);
  g_clear_error (&self->task_error);
  cv2_op_clear (&self->op);

  G_OBJECT_CLASS (fpi_device_cv2_parent_class)->finalize (object);
}

static void
fpi_device_cv2_class_init (FpiDeviceCv2Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_cv2_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Broadcom ControlVault2 (BCM5880) Fingerprint Reader";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = CV2_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = cv2_open;
  dev_class->close = cv2_close;
  dev_class->enroll = cv2_enroll;
  dev_class->verify = cv2_verify_identify;
  dev_class->identify = cv2_verify_identify;
  dev_class->delete = cv2_delete;
  dev_class->list = cv2_list;

  fpi_device_class_auto_initialize_features (dev_class);
}
