/*
 * SecuGen U20-AL driver for libfprint
 * Copyright (C) 2026
 *
 * Protocol reverse-engineered from Windows USBPcap traces and reproduced
 * with standalone libusb probes on Linux.
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

#define FP_COMPONENT "secugen_u20al"

#include <string.h>

#include "drivers_api.h"

#define U20AL_VID 0x1162
#define U20AL_PID 0x2370

#define U20AL_EP_DATA 0x82

#define U20AL_CTRL_TIMEOUT_MS 2000
#define U20AL_BULK_TIMEOUT_MS 10000
#define U20AL_POLL_INTERVAL_MS 100
#define U20AL_FRAME_CLEANUP_DELAY_MS 100
#define U20AL_INTERFRAME_DELAY_MS 60

#define U20AL_REPORT_LENGTH 11
#define U20AL_READY_LENGTH 2
#define U20AL_REPORT_VALUE 0x0600
#define U20AL_REPORT_INDEX 0x0600

#define U20AL_FRAME_COUNT 4
#define U20AL_FRAME_HEADER_SIZE 12
#define U20AL_FRAME_PAYLOAD_SIZE 1400000
#define U20AL_FRAME_SIZE (U20AL_FRAME_HEADER_SIZE + U20AL_FRAME_PAYLOAD_SIZE)
#define U20AL_BULK_REQUEST_SIZE 1400320

#define U20AL_RAW_WIDTH 700
#define U20AL_RAW_HEIGHT 1000
#define U20AL_RAW_IMAGE_SIZE (U20AL_RAW_WIDTH * U20AL_RAW_HEIGHT)
#define U20AL_PACKED_ROW_SIZE (U20AL_RAW_WIDTH * 2)

/* Public product output advertised by SecuGen. */
#define U20AL_IMAGE_WIDTH 300
#define U20AL_IMAGE_HEIGHT 400
#define U20AL_IMAGE_SIZE (U20AL_IMAGE_WIDTH * U20AL_IMAGE_HEIGHT)
#define U20AL_PPMM (500.0 / 25.4)

static const guint8 u20al_idle_command[U20AL_REPORT_LENGTH] = {
  0x01, 0x0b, 0x8f, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

static const guint8 u20al_poll_command[U20AL_REPORT_LENGTH] = {
  0x01, 0x0b, 0x8f, 0x01, 0x00, 0x06,
  0x0b, 0x00, 0x00, 0x00, 0x00,
};

static const guint8 u20al_arm_command[U20AL_REPORT_LENGTH] = {
  0x81, 0x0b, 0x8e, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

static const guint8 u20al_acquire_command[U20AL_REPORT_LENGTH] = {
  0x81, 0x0b, 0x78, 0x03, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

static const guint8 u20al_cleanup_commands[U20AL_FRAME_COUNT][U20AL_REPORT_LENGTH] = {
  { 0x81, 0x0b, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xa0, 0x6e },
  { 0x81, 0x0b, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x60, 0x6f },
  { 0x81, 0x0b, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xf0, 0x6f },
  { 0x81, 0x0b, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xb0, 0x6e },
};

struct _FpiDeviceSecugenU20al
{
  FpImageDevice       parent;

  guint8              iface_num;
  gboolean            iface_claimed;

  GSource            *poll_source;
  FpiImageDeviceState image_state;

  gboolean            report_running;
  gboolean            capture_running;
  gboolean            deactivating;
  gboolean            deactivate_cleanup_running;
  gboolean            capture_cleanup_needed;

  guint8              report_command[U20AL_REPORT_LENGTH];
  guint8              report_response[U20AL_REPORT_LENGTH];

  guint               current_frame;
  guint8              frame_types[U20AL_FRAME_COUNT];

  guint8             *bulk_buffer;
  guint8             *raw_image;
  guint8             *output_image;
};

G_DECLARE_FINAL_TYPE (FpiDeviceSecugenU20al,
                      fpi_device_secugen_u20al,
                      FPI,
                      DEVICE_SECUGEN_U20AL,
                      FpImageDevice);

G_DEFINE_TYPE (FpiDeviceSecugenU20al,
               fpi_device_secugen_u20al,
               FP_TYPE_IMAGE_DEVICE);

static void u20al_maybe_finish_deactivate (FpImageDevice *dev);
static void u20al_schedule_poll (FpImageDevice *dev,
                                 guint          delay_ms);

static GError *
u20al_protocol_error (const gchar *message)
{
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", message);
}

static void
u20al_submit_ready (FpiSsm                *ssm,
                    FpImageDevice         *dev,
                    FpiUsbTransferCallback callback)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x85,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_READY_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           callback,
                           NULL);
}

static void
u20al_submit_set_report (FpiSsm        *ssm,
                         FpImageDevice *dev,
                         const guint8  *command)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x01,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_REPORT_LENGTH);
  memcpy (transfer->buffer, command, U20AL_REPORT_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           fpi_ssm_usb_transfer_cb,
                           NULL);
}

static void
u20al_submit_get_report (FpiSsm                *ssm,
                         FpImageDevice         *dev,
                         FpiUsbTransferCallback callback)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x81,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_REPORT_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           callback,
                           NULL);
}

/*
 * Deactivation cleanup must not use the action cancellable.  When an
 * enroll/identify action is cancelled, that cancellable is already in the
 * cancelled state, but the sensor still needs its cleanup and idle commands.
 */
static void
u20al_submit_ready_uncancellable (FpiSsm                *ssm,
                                  FpImageDevice         *dev,
                                  FpiUsbTransferCallback callback)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x85,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_READY_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           NULL,
                           callback,
                           NULL);
}

static void
u20al_submit_set_report_uncancellable (FpiSsm        *ssm,
                                       FpImageDevice *dev,
                                       const guint8  *command)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x01,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_REPORT_LENGTH);
  memcpy (transfer->buffer, command, U20AL_REPORT_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           NULL,
                           fpi_ssm_usb_transfer_cb,
                           NULL);
}

static void
u20al_submit_get_report_uncancellable (FpiSsm                *ssm,
                                       FpImageDevice         *dev,
                                       FpiUsbTransferCallback callback)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_CLASS,
                                 G_USB_DEVICE_RECIPIENT_INTERFACE,
                                 0x81,
                                 U20AL_REPORT_VALUE,
                                 U20AL_REPORT_INDEX,
                                 U20AL_REPORT_LENGTH);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           U20AL_CTRL_TIMEOUT_MS,
                           NULL,
                           callback,
                           NULL);
}

static void
u20al_ready_cb (FpiUsbTransfer        *transfer,
                FpDevice       *device G_GNUC_UNUSED,
                gpointer user_data     G_GNUC_UNUSED,
                GError                *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != U20AL_READY_LENGTH ||
      transfer->buffer[0] != 0x0b ||
      transfer->buffer[1] != 0x00)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           u20al_protocol_error ("Unexpected U20-AL ready response"));
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
u20al_report_get_cb (FpiUsbTransfer    *transfer,
                     FpDevice          *device,
                     gpointer user_data G_GNUC_UNUSED,
                     GError            *error)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != U20AL_REPORT_LENGTH ||
      transfer->buffer[0] != 0xa1 ||
      transfer->buffer[1] != 0x81)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           u20al_protocol_error ("Unexpected U20-AL status report"));
      return;
    }

  memcpy (self->report_response,
          transfer->buffer,
          U20AL_REPORT_LENGTH);
  fpi_ssm_next_state (transfer->ssm);
}

enum report_states {
  REPORT_READY_SET = 0,
  REPORT_SET,
  REPORT_READY_GET,
  REPORT_GET,
  REPORT_DONE,
  REPORT_NUM_STATES,
};

static void
u20al_report_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case REPORT_READY_SET:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case REPORT_SET:
      u20al_submit_set_report (ssm, dev, self->report_command);
      break;

    case REPORT_READY_GET:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case REPORT_GET:
      u20al_submit_get_report (ssm, dev, u20al_report_get_cb);
      break;

    case REPORT_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static FpiSsm *
u20al_new_report_ssm (FpImageDevice *dev, const guint8 *command)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  g_assert (!self->report_running);

  memcpy (self->report_command, command, U20AL_REPORT_LENGTH);
  memset (self->report_response, 0, U20AL_REPORT_LENGTH);
  self->report_running = TRUE;

  return fpi_ssm_new (FP_DEVICE (dev),
                      u20al_report_run_state,
                      REPORT_NUM_STATES);
}

static void
u20al_activate_complete (FpiSsm   *ssm G_GNUC_UNUSED,
                         FpDevice     *device,
                         GError       *error)
{
  g_autoptr(GError) local_error = error;
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);

  self->report_running = FALSE;

  if (self->deactivating)
    {
      u20al_maybe_finish_deactivate (dev);
      return;
    }

  fpi_image_device_activate_complete (dev, g_steal_pointer (&local_error));
}

static void
u20al_poll_complete (FpiSsm   *ssm G_GNUC_UNUSED,
                     FpDevice     *device,
                     GError       *error)
{
  g_autoptr(GError) local_error = error;
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);
  gboolean present;

  self->report_running = FALSE;

  if (self->deactivating)
    {
      u20al_maybe_finish_deactivate (dev);
      return;
    }

  if (local_error)
    {
      fpi_image_device_session_error (dev, g_steal_pointer (&local_error));
      return;
    }

  present = self->report_response[3] == 0x00;

  if (self->image_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      if (present)
        {
          fp_dbg ("Finger detected");
          fpi_image_device_report_finger_status (dev, TRUE);
        }
      else
        {
          u20al_schedule_poll (dev, U20AL_POLL_INTERVAL_MS);
        }
    }
  else if (self->image_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    {
      if (!present)
        {
          fp_dbg ("Finger removed");
          fpi_image_device_report_finger_status (dev, FALSE);
        }
      else
        {
          u20al_schedule_poll (dev, U20AL_POLL_INTERVAL_MS);
        }
    }
}

static void
u20al_poll_timeout (FpDevice          *device,
                    gpointer user_data G_GNUC_UNUSED)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);
  FpiSsm *ssm;

  self->poll_source = NULL;

  if (self->deactivating || self->report_running || self->capture_running)
    return;

  if (self->image_state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON &&
      self->image_state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    return;

  ssm = u20al_new_report_ssm (dev, u20al_poll_command);
  fpi_ssm_start (ssm, u20al_poll_complete);
}

static void
u20al_schedule_poll (FpImageDevice *dev, guint delay_ms)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  g_clear_pointer (&self->poll_source, g_source_destroy);

  if (self->deactivating)
    return;

  self->poll_source = fpi_device_add_timeout (FP_DEVICE (dev),
                                              delay_ms,
                                              u20al_poll_timeout,
                                              NULL,
                                              NULL);
}

static void
u20al_reconstruct_raw_image (const guint8 *payload, guint8 *image)
{
  guint row;
  guint column;

  for (row = 0; row < U20AL_RAW_HEIGHT; row++)
    {
      const guint8 *packed_row = payload + row * U20AL_PACKED_ROW_SIZE;
      guint8 *image_row = image + row * U20AL_RAW_WIDTH;

      for (column = 0; column < U20AL_RAW_WIDTH; column++)
        {
          guint8 lane_a = packed_row[column * 2];
          guint8 lane_b = packed_row[column * 2 + 1];

          image_row[column] = (row & 1) ? lane_a : lane_b;
        }
    }
}

#define U20AL_RESIZE_FRAC_BITS 8
#define U20AL_RESIZE_FRAC (1 << U20AL_RESIZE_FRAC_BITS)

static void
u20al_resize_bilinear (const guint8 *src,
                       gint          src_width,
                       gint          src_height,
                       guint8       *dst,
                       gint          dst_width,
                       gint          dst_height)
{
  gint row;
  gint column;

  for (row = 0; row < dst_height; row++)
    {
      gint src_row = (gint) ((gint64) row * (src_height - 1) * U20AL_RESIZE_FRAC /
                             (dst_height - 1));
      gint row0 = src_row >> U20AL_RESIZE_FRAC_BITS;
      gint row1 = MIN (row0 + 1, src_height - 1);
      gint row_weight1 = src_row & (U20AL_RESIZE_FRAC - 1);
      gint row_weight0 = U20AL_RESIZE_FRAC - row_weight1;

      for (column = 0; column < dst_width; column++)
        {
          gint src_column = (gint) ((gint64) column * (src_width - 1) * U20AL_RESIZE_FRAC /
                                    (dst_width - 1));
          gint column0 = src_column >> U20AL_RESIZE_FRAC_BITS;
          gint column1 = MIN (column0 + 1, src_width - 1);
          gint column_weight1 = src_column & (U20AL_RESIZE_FRAC - 1);
          gint column_weight0 = U20AL_RESIZE_FRAC - column_weight1;
          guint32 accumulator;

          accumulator =
            (guint32) row_weight0 * column_weight0 * src[row0 * src_width + column0] +
            (guint32) row_weight0 * column_weight1 * src[row0 * src_width + column1] +
            (guint32) row_weight1 * column_weight0 * src[row1 * src_width + column0] +
            (guint32) row_weight1 * column_weight1 * src[row1 * src_width + column1];

          dst[row * dst_width + column] =
            (guint8) ((accumulator +
                       (U20AL_RESIZE_FRAC * U20AL_RESIZE_FRAC) / 2) >>
                      (2 * U20AL_RESIZE_FRAC_BITS));
        }
    }
}

static void
u20al_bulk_cb (FpiUsbTransfer    *transfer,
               FpDevice          *device,
               gpointer user_data G_GNUC_UNUSED,
               GError            *error)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);
  guint8 *header = transfer->buffer;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != U20AL_FRAME_SIZE)
    {
      fp_warn ("Unexpected U20-AL frame length on frame %u: "
               "received %zd bytes, expected %u bytes",
               self->current_frame + 1,
               transfer->actual_length,
               (guint) U20AL_FRAME_SIZE);

      fpi_ssm_mark_failed (transfer->ssm,
                           u20al_protocol_error ("Unexpected U20-AL frame length"));
      return;
    }

  if (header[0] != U20AL_FRAME_HEADER_SIZE ||
      (header[1] != 0x8e && header[1] != 0x8f))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           u20al_protocol_error ("Unexpected U20-AL frame header"));
      return;
    }

  if (memcmp (header + 2, header + 6, 4) != 0)
    fp_warn ("Frame %u header duplicate fields do not match",
             self->current_frame + 1);

  self->frame_types[self->current_frame] = header[1];

  fp_dbg ("Frame %u: %zd bytes, type 0x%02x, counter bytes %02x %02x",
          self->current_frame + 1,
          transfer->actual_length,
          header[1],
          header[10],
          header[11]);

  if (self->current_frame == U20AL_FRAME_COUNT - 1)
    u20al_reconstruct_raw_image (transfer->buffer + U20AL_FRAME_HEADER_SIZE,
                                 self->raw_image);

  fpi_ssm_next_state (transfer->ssm);
}

static void
u20al_capture_get_report_cb (FpiUsbTransfer    *transfer,
                             FpDevice          *device,
                             gpointer user_data G_GNUC_UNUSED,
                             GError            *error)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != U20AL_REPORT_LENGTH ||
      transfer->buffer[0] != 0xa1 ||
      transfer->buffer[1] != 0x81)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           u20al_protocol_error ("Unexpected U20-AL post-frame report"));
      return;
    }

  memcpy (self->report_response,
          transfer->buffer,
          U20AL_REPORT_LENGTH);
  fpi_ssm_next_state (transfer->ssm);
}

enum capture_states {
  CAPTURE_READY_ARM = 0,
  CAPTURE_SET_ARM,
  CAPTURE_READY_ACQUIRE,
  CAPTURE_SET_ACQUIRE,
  CAPTURE_BULK,
  CAPTURE_READY_CLEANUP,
  CAPTURE_SET_CLEANUP,
  CAPTURE_WAIT_IDLE,
  CAPTURE_READY_IDLE,
  CAPTURE_SET_IDLE,
  CAPTURE_READY_GET,
  CAPTURE_GET_IDLE,
  CAPTURE_NEXT_FRAME,
  CAPTURE_DONE,
  CAPTURE_NUM_STATES,
};

static void
u20al_capture_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (_dev);

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_READY_ARM:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case CAPTURE_SET_ARM:
      u20al_submit_set_report (ssm, dev, u20al_arm_command);
      break;

    case CAPTURE_READY_ACQUIRE:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case CAPTURE_SET_ACQUIRE:
      self->capture_cleanup_needed = TRUE;
      u20al_submit_set_report (ssm, dev, u20al_acquire_command);
      break;

    case CAPTURE_BULK:
      {
        g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

        memset (self->bulk_buffer, 0, U20AL_BULK_REQUEST_SIZE);
        fpi_usb_transfer_fill_bulk_full (transfer,
                                         U20AL_EP_DATA,
                                         self->bulk_buffer,
                                         U20AL_BULK_REQUEST_SIZE,
                                         NULL);
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                                 U20AL_BULK_TIMEOUT_MS,
                                 fpi_device_get_cancellable (FP_DEVICE (dev)),
                                 u20al_bulk_cb,
                                 NULL);
      }
      break;

    case CAPTURE_READY_CLEANUP:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case CAPTURE_SET_CLEANUP:
      u20al_submit_set_report (ssm,
                               dev,
                               u20al_cleanup_commands[self->current_frame]);
      break;

    case CAPTURE_WAIT_IDLE:
      fpi_ssm_next_state_delayed (ssm, U20AL_FRAME_CLEANUP_DELAY_MS);
      break;

    case CAPTURE_READY_IDLE:
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case CAPTURE_SET_IDLE:
      u20al_submit_set_report (ssm, dev, u20al_idle_command);
      break;

    case CAPTURE_READY_GET:
      self->capture_cleanup_needed = FALSE;
      u20al_submit_ready (ssm, dev, u20al_ready_cb);
      break;

    case CAPTURE_GET_IDLE:
      u20al_submit_get_report (ssm, dev, u20al_capture_get_report_cb);
      break;

    case CAPTURE_NEXT_FRAME:
      self->current_frame++;
      if (self->current_frame < U20AL_FRAME_COUNT)
        fpi_ssm_jump_to_state_delayed (ssm,
                                       CAPTURE_READY_ARM,
                                       U20AL_INTERFRAME_DELAY_MS);
      else
        fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
u20al_capture_complete (FpiSsm   *ssm G_GNUC_UNUSED,
                        FpDevice     *device,
                        GError       *error)
{
  g_autoptr(GError) local_error = error;
  g_autoptr(FpImage) image = NULL;
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);

  self->capture_running = FALSE;

  if (self->deactivating)
    {
      u20al_maybe_finish_deactivate (dev);
      return;
    }

  if (local_error)
    {
      fpi_image_device_session_error (dev, g_steal_pointer (&local_error));
      return;
    }

  u20al_resize_bilinear (self->raw_image,
                         U20AL_RAW_WIDTH,
                         U20AL_RAW_HEIGHT,
                         self->output_image,
                         U20AL_IMAGE_WIDTH,
                         U20AL_IMAGE_HEIGHT);

  image = fp_image_new (U20AL_IMAGE_WIDTH, U20AL_IMAGE_HEIGHT);
  image->ppmm = U20AL_PPMM;
  image->flags = FPI_IMAGE_COLORS_INVERTED;
  memcpy (image->data, self->output_image, U20AL_IMAGE_SIZE);

  fpi_image_device_image_captured (dev, g_steal_pointer (&image));
}

static void
u20al_start_capture (FpImageDevice *dev)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);
  FpiSsm *ssm;

  if (self->capture_running || self->deactivating)
    return;

  g_clear_pointer (&self->poll_source, g_source_destroy);
  self->current_frame = 0;
  self->capture_cleanup_needed = FALSE;
  memset (self->frame_types, 0, sizeof (self->frame_types));
  memset (self->raw_image, 0, U20AL_RAW_IMAGE_SIZE);
  self->capture_running = TRUE;

  ssm = fpi_ssm_new (FP_DEVICE (dev),
                     u20al_capture_run_state,
                     CAPTURE_NUM_STATES);
  fpi_ssm_start (ssm, u20al_capture_complete);
}

static void
u20al_dev_open (FpImageDevice *dev)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) interfaces = NULL;
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);
  GUsbInterface *iface = NULL;
  guint i;

  interfaces = g_usb_device_get_interfaces (
    fpi_device_get_usb_device (FP_DEVICE (dev)), &error);
  if (error)
    {
      fpi_image_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }

  for (i = 0; i < interfaces->len; i++)
    {
      GUsbInterface *candidate = g_ptr_array_index (interfaces, i);

      if (g_usb_interface_get_class (candidate) == 0xff &&
          g_usb_interface_get_subclass (candidate) == 0xff &&
          g_usb_interface_get_protocol (candidate) == 0xff)
        {
          iface = candidate;
          break;
        }
    }

  if (!iface)
    {
      fpi_image_device_open_complete (
        dev,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                  "Could not find the U20-AL vendor interface"));
      return;
    }

  self->iface_num = g_usb_interface_get_number (iface);

  if (!g_usb_device_claim_interface (
        fpi_device_get_usb_device (FP_DEVICE (dev)),
        self->iface_num,
        0,
        &error))
    {
      fpi_image_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }

  self->iface_claimed = TRUE;
  self->bulk_buffer = g_malloc0 (U20AL_BULK_REQUEST_SIZE);
  self->raw_image = g_malloc0 (U20AL_RAW_IMAGE_SIZE);
  self->output_image = g_malloc0 (U20AL_IMAGE_SIZE);

  fpi_image_device_open_complete (dev, NULL);
}

static void
u20al_dev_close (FpImageDevice *dev)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  g_clear_pointer (&self->poll_source, g_source_destroy);
  g_clear_pointer (&self->bulk_buffer, g_free);
  g_clear_pointer (&self->raw_image, g_free);
  g_clear_pointer (&self->output_image, g_free);

  if (self->iface_claimed)
    {
      g_usb_device_release_interface (
        fpi_device_get_usb_device (FP_DEVICE (dev)),
        self->iface_num,
        0,
        &error);
      self->iface_claimed = FALSE;
    }

  fpi_image_device_close_complete (dev, g_steal_pointer (&error));
}

static void
u20al_dev_activate (FpImageDevice *dev)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);
  FpiSsm *ssm;

  self->deactivating = FALSE;
  self->deactivate_cleanup_running = FALSE;
  self->capture_cleanup_needed = FALSE;
  self->image_state = FPI_IMAGE_DEVICE_STATE_ACTIVATING;

  ssm = u20al_new_report_ssm (dev, u20al_idle_command);
  fpi_ssm_start (ssm, u20al_activate_complete);
}

enum deactivate_cleanup_states {
  DEACTIVATE_CLEANUP_READY_CLEANUP = 0,
  DEACTIVATE_CLEANUP_SET_CLEANUP,
  DEACTIVATE_CLEANUP_WAIT_IDLE,
  DEACTIVATE_CLEANUP_READY_IDLE,
  DEACTIVATE_CLEANUP_SET_IDLE,
  DEACTIVATE_CLEANUP_READY_GET,
  DEACTIVATE_CLEANUP_GET_IDLE,
  DEACTIVATE_CLEANUP_DONE,
  DEACTIVATE_CLEANUP_NUM_STATES,
};

static void
u20al_deactivate_cleanup_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEACTIVATE_CLEANUP_READY_CLEANUP:
      u20al_submit_ready_uncancellable (ssm, dev, u20al_ready_cb);
      break;

    case DEACTIVATE_CLEANUP_SET_CLEANUP:
      u20al_submit_set_report_uncancellable (
        ssm,
        dev,
        u20al_cleanup_commands[self->current_frame]);
      break;

    case DEACTIVATE_CLEANUP_WAIT_IDLE:
      fpi_ssm_next_state_delayed (ssm, U20AL_FRAME_CLEANUP_DELAY_MS);
      break;

    case DEACTIVATE_CLEANUP_READY_IDLE:
      u20al_submit_ready_uncancellable (ssm, dev, u20al_ready_cb);
      break;

    case DEACTIVATE_CLEANUP_SET_IDLE:
      u20al_submit_set_report_uncancellable (ssm, dev, u20al_idle_command);
      break;

    case DEACTIVATE_CLEANUP_READY_GET:
      u20al_submit_ready_uncancellable (ssm, dev, u20al_ready_cb);
      break;

    case DEACTIVATE_CLEANUP_GET_IDLE:
      u20al_submit_get_report_uncancellable (ssm,
                                             dev,
                                             u20al_capture_get_report_cb);
      break;

    case DEACTIVATE_CLEANUP_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
u20al_deactivate_cleanup_complete (FpiSsm   *ssm G_GNUC_UNUSED,
                                   FpDevice     *device,
                                   GError       *error)
{
  g_autoptr(GError) local_error = error;
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (device);

  self->deactivate_cleanup_running = FALSE;
  self->capture_cleanup_needed = FALSE;
  self->deactivating = FALSE;
  self->image_state = FPI_IMAGE_DEVICE_STATE_INACTIVE;

  if (local_error)
    fp_warn ("U20-AL hardware cleanup during deactivation failed: %s",
             local_error->message);
  else
    fp_dbg ("U20-AL hardware cleanup during deactivation completed");

  fpi_image_device_deactivate_complete (dev,
                                        g_steal_pointer (&local_error));
}

static void
u20al_start_deactivate_cleanup (FpImageDevice *dev)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);
  FpiSsm *ssm;

  g_assert (self->deactivating);
  g_assert (self->capture_cleanup_needed);
  g_assert (!self->deactivate_cleanup_running);

  self->deactivate_cleanup_running = TRUE;
  fp_dbg ("Running uncancellable U20-AL cleanup after interrupted capture");

  ssm = fpi_ssm_new (FP_DEVICE (dev),
                     u20al_deactivate_cleanup_run_state,
                     DEACTIVATE_CLEANUP_NUM_STATES);
  fpi_ssm_start (ssm, u20al_deactivate_cleanup_complete);
}

static void
u20al_maybe_finish_deactivate (FpImageDevice *dev)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  if (!self->deactivating ||
      self->report_running ||
      self->capture_running ||
      self->deactivate_cleanup_running)
    return;

  if (self->capture_cleanup_needed)
    {
      u20al_start_deactivate_cleanup (dev);
      return;
    }

  self->deactivating = FALSE;
  self->image_state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
  fpi_image_device_deactivate_complete (dev, NULL);
}

static void
u20al_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  self->deactivating = TRUE;
  self->image_state = FPI_IMAGE_DEVICE_STATE_DEACTIVATING;
  g_clear_pointer (&self->poll_source, g_source_destroy);

  u20al_maybe_finish_deactivate (dev);
}

static void
u20al_dev_change_state (FpImageDevice      *dev,
                        FpiImageDeviceState state)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (dev);

  self->image_state = state;
  g_clear_pointer (&self->poll_source, g_source_destroy);

  if (self->deactivating)
    return;

  switch (state)
    {
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON:
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF:
      u20al_schedule_poll (dev, 0);
      break;

    case FPI_IMAGE_DEVICE_STATE_CAPTURE:
      u20al_start_capture (dev);
      break;

    case FPI_IMAGE_DEVICE_STATE_INACTIVE:
    case FPI_IMAGE_DEVICE_STATE_ACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_DEACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_IDLE:
      break;
    }
}

static const FpIdEntry id_table[] = {
  { .vid = U20AL_VID, .pid = U20AL_PID, },
  { .vid = 0, .pid = 0, },
};

static void
fpi_device_secugen_u20al_init (FpiDeviceSecugenU20al *self)
{
  self->image_state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
}

static void
fpi_device_secugen_u20al_finalize (GObject *object)
{
  FpiDeviceSecugenU20al *self = FPI_DEVICE_SECUGEN_U20AL (object);

  g_clear_pointer (&self->poll_source, g_source_destroy);
  g_clear_pointer (&self->bulk_buffer, g_free);
  g_clear_pointer (&self->raw_image, g_free);
  g_clear_pointer (&self->output_image, g_free);

  G_OBJECT_CLASS (fpi_device_secugen_u20al_parent_class)->finalize (object);
}

static void
fpi_device_secugen_u20al_class_init (FpiDeviceSecugenU20alClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *image_class = FP_IMAGE_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_secugen_u20al_finalize;

  device_class->id = "secugen_u20al";
  device_class->full_name = "SecuGen U20-AL";
  device_class->type = FP_DEVICE_TYPE_USB;
  device_class->id_table = id_table;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;

  image_class->img_open = u20al_dev_open;
  image_class->img_close = u20al_dev_close;
  image_class->activate = u20al_dev_activate;
  image_class->deactivate = u20al_dev_deactivate;
  image_class->change_state = u20al_dev_change_state;

  image_class->bz3_threshold = 40;
  image_class->img_width = U20AL_IMAGE_WIDTH;
  image_class->img_height = U20AL_IMAGE_HEIGHT;
}
