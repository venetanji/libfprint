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
 * The protocol was worked out from USB captures of the reader only.
 *
 * Security: templates are stored and matched inside the chip, and no
 * fingerprint images or templates cross USB. The plaintext interface used
 * here does not authenticate replies, so the host trusts the reader's match
 * result, as with other match-on-chip drivers in libfprint.
 */

#define FP_COMPONENT "cv2"

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

struct _FpiDeviceCv2
{
  FpDevice parent;

  gchar   *fw_version;     /* USH_REL_UPGRADE_VER, NULL if unknown */
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

/* Sends even if the operation was canceled. */
static void
cv2_cmd_always (FpiDeviceCv2  *self,
                FpiSsm        *ssm,
                GByteArray    *request,
                Cv2CmdCallback callback)
{
  cv2_cmd_full (self, ssm, request, FALSE, callback);
}

/* ---- Device open and close ----
 *
 * Opening reads the firmware version. A reader that does not answer (a USB
 * error) fails the open; a reply that can't be used is only logged.
 */

enum {
  OPEN_GET_VERSION,
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
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCv2 *self = FPI_DEVICE_CV2 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_VERSION:
      cv2_cmd_always (self, ssm, cv2_build_simple (CV2_CMD_GET_VERSION), open_version_cb);
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

  fpi_device_class_auto_initialize_features (dev_class);
}
