/*
 * Next Biometrics NB-2033-U driver for libfprint
 *
 * Copyright (C) 2026 Sebastian van de Meer <kernel-error@kernel-error.com>
 *
 * Based on nb1010.c by Huan Wang and Andrej Krutak.
 * Protocol reverse-engineered from USB captures using the
 * NBBiometrics ANF SDK on Linux with usbmon.
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
 * NB-2033-U Protocol Notes:
 *
 * Endpoints: Bulk OUT EP 0x02, Bulk IN EP 0x01 (address 0x81 with direction bit)
 * Image: 256x180 pixels, 8-bit grayscale, same sensor as NB-1010-U/NB-2020-U
 *
 * Command format:  [CMD] [0x00] [LEN_LO] [LEN_HI] [PAYLOAD...]
 * Response format: [STATUS_LO] [STATUS_HI] [LEN_LO] [LEN_HI] [PAYLOAD...]
 *
 * Two 0x0D config commands must precede each 0x38 finger detect command
 * to enable enhanced detection mode (empirically determined from USB captures).
 * Capture (0x12) returns 180 rows of 268 bytes (12 header + 256 pixels).
 */

#define FP_COMPONENT "nb2033"
#include "fpi-log.h"

#include "drivers_api.h"

#define FRAME_HEIGHT 180
#define FRAME_WIDTH 256

#define NB2033_EP_OUT (0x02 | FPI_USB_ENDPOINT_OUT)
#define NB2033_EP_IN (0x01 | FPI_USB_ENDPOINT_IN)

#define NB2033_CMD_RECV_LEN 6
#define NB2033_ROW_HDR_LEN 12
#define NB2033_ROW_RECV_LEN (NB2033_ROW_HDR_LEN + FRAME_WIDTH)

/* Finger detect: response byte[4], range 0-255.
 * Threshold 40 determined empirically from USB captures:
 * values 0-3 = no finger, transients after init peak ~21,
 * real finger placement peaks 25-50+. */
#define NB2033_FINGER_LEVEL_OFFSET 4
#define NB2033_FINGER_THRESHOLD 40

#define NB2033_INIT_RECV_LEN 8
#define NB2033_DEFAULT_TIMEOUT 1000
#define NB2033_CAPTURE_TIMEOUT 2000
#define NB2033_TRANSITION_DELAY 50
#define NB2033_SETTLE_DELAY 1500
#define NB2033_ROW_READY_DELAY 150

/* SSM states.
 * nb2033_write_ignore_read() sends a command, then reads and discards the
 * response; the response read completion advances the SSM by ONE state, to
 * the matching M_*_DONE state. Each M_*_DONE handler then advances to the
 * next functional state. */
enum {
  /* Init sensor at start of each cycle (SDK sends 0x07 twice) */
  M_INIT1_SEND,
  M_INIT1_READ,
  M_INIT2_SEND,
  M_INIT2_READ,
  M_INIT_SETTLE,
  /* Finger detection: 0x0D, 0x0D, 0x38 */
  M_WAIT_PRINT,
  M_CONFIG1,        /* send 0x0D + read response */
  M_CONFIG1_DONE,   /* response read complete; advance */
  M_CONFIG2,        /* send 0x0D + read response */
  M_CONFIG2_DONE,   /* response read complete; advance */
  M_DETECT_SEND,
  M_DETECT_READ,
  /* Capture */
  M_PRECAPTURE,      /* send 0x0D precapture + read */
  M_PRECAPTURE_DONE, /* auto-advanced */
  M_CAPTURE,         /* send 0x12 capture + read */
  M_CAPTURE_DONE,    /* auto-advanced */
  M_CAPTURE_WAIT,
  M_READ_DATA,
  M_SUBMIT_PRINT,

  M_LOOP_NUM_STATES,
};

/* Commands */
static guint8 nb2033_cmd_init[] = {
  0x07, 0x00, 0x06, 0x00, 0x01, 0x00, 0x4e, 0x00, 0x00, 0x00,
};

static guint8 nb2033_cmd_config[] = {
  0x0d, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static guint8 nb2033_cmd_check_finger[] = {
  0x38, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static guint8 nb2033_cmd_capture[] = {
  0x12, 0x00, 0x10, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct _FpiDeviceNb2033
{
  FpImageDevice parent;
  FpiSsm       *ssm;
  guint8       *scanline_buf;
  gboolean      deactivating;
  guint         partial_received;
};

G_DECLARE_FINAL_TYPE (FpiDeviceNb2033, fpi_device_nb2033, FPI, DEVICE_NB2033, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceNb2033, fpi_device_nb2033, FP_TYPE_IMAGE_DEVICE);

static void
nb2033_dev_init (FpImageDevice *dev)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->scanline_buf = g_malloc0 (FRAME_WIDTH * FRAME_HEIGHT);

  fpi_image_device_open_complete (dev, NULL);
  fp_dbg ("nb2033 initialized");
}

static void
nb2033_dev_deinit (FpImageDevice *dev)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->scanline_buf, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);
  fpi_image_device_close_complete (dev, error);
  fp_dbg ("nb2033 deinitialized");
}

static void
nb2033_dev_activate (FpImageDevice *dev)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);

  self->deactivating = FALSE;

  fpi_image_device_activate_complete (dev, NULL);
  fp_dbg ("nb2033 activated");
}

static void
nb2033_dev_deactivated (FpImageDevice *dev, GError *err)
{
  fpi_image_device_deactivate_complete (dev, err);
  fp_dbg ("nb2033 deactivated");
}

static void
nb2033_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);

  self->deactivating = TRUE;
  if (self->ssm == NULL)
    nb2033_dev_deactivated (dev, NULL);
}

/* --- Helpers for async bulk send + receive --- */

/* Callback: after bulk OUT, read the response and ignore it, advance SSM */
static void
nb2033_read_ignore_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer unused_data, GError *error)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  FpiUsbTransfer *new_transfer;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  new_transfer = fpi_usb_transfer_new (dev);
  new_transfer->short_is_error = TRUE;
  new_transfer->ssm = transfer->ssm;

  fpi_usb_transfer_fill_bulk (new_transfer, NB2033_EP_IN, NB2033_CMD_RECV_LEN);
  fpi_usb_transfer_submit (new_transfer, NB2033_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
nb2033_write_ignore_read (FpiDeviceNb2033 *self, guint8 *buf, gsize len)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->short_is_error = TRUE;
  transfer->ssm = self->ssm;

  fpi_usb_transfer_fill_bulk_full (transfer, NB2033_EP_OUT, buf, len, NULL);
  fpi_usb_transfer_submit (transfer, NB2033_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           nb2033_read_ignore_data_cb, NULL);
}

/* --- Finger detection --- */

static void
nb2033_check_fingerprint_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                             gpointer unused_data, GError *error)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  guint8 level;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  level = (transfer->actual_length > NB2033_FINGER_LEVEL_OFFSET) ?
          transfer->buffer[NB2033_FINGER_LEVEL_OFFSET] : 0;

  fp_dbg ("finger detect level=%d threshold=%d", level, NB2033_FINGER_THRESHOLD);

  if (level > NB2033_FINGER_THRESHOLD)
    fpi_ssm_next_state (transfer->ssm);
  else
    fpi_ssm_jump_to_state (transfer->ssm, M_WAIT_PRINT);
}

/* --- Capture --- */

static void
nb2033_read_capture_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer unused_data, GError *error)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  gsize offset;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  if (transfer->actual_length < NB2033_ROW_RECV_LEN)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Short row read: %d",
                                                     (int) transfer->actual_length));
      return;
    }

  offset = self->partial_received * FRAME_WIDTH;
  memcpy (self->scanline_buf + offset,
          transfer->buffer + NB2033_ROW_HDR_LEN, FRAME_WIDTH);

  self->partial_received++;
  if (self->partial_received == FRAME_HEIGHT)
    {
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer), NB2033_CAPTURE_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           nb2033_read_capture_cb, NULL);
}

static gboolean
submit_image (FpiSsm        *ssm,
              FpImageDevice *dev)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  FpImage *img;

  img = fp_image_new (FRAME_WIDTH, FRAME_HEIGHT);
  if (img == NULL)
    return FALSE;

  memcpy (img->data, self->scanline_buf, FRAME_WIDTH * FRAME_HEIGHT);
  fpi_image_device_image_captured (dev, img);

  return TRUE;
}

/* --- State machine --- */

static void
m_loop_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (_dev);

  self->ssm = NULL;

  if (self->deactivating)
    nb2033_dev_deactivated (dev, error);
  else if (error != NULL)
    fpi_image_device_session_error (dev, error);
}

static void
m_loop_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (_dev);

  if (self->deactivating)
    {
      fp_dbg ("deactivating, marking completed");
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_INIT1_SEND:
    case M_INIT2_SEND:
      {
        /* Send init/wake command (SDK sends this twice) */
        FpiUsbTransfer *transfer;

        transfer = fpi_usb_transfer_new (FP_DEVICE (self));
        transfer->short_is_error = TRUE;
        transfer->ssm = ssm;

        fpi_usb_transfer_fill_bulk_full (transfer, NB2033_EP_OUT,
                                         nb2033_cmd_init,
                                         G_N_ELEMENTS (nb2033_cmd_init), NULL);
        fpi_usb_transfer_submit (transfer, NB2033_DEFAULT_TIMEOUT,
                                 fpi_device_get_cancellable (FP_DEVICE (self)),
                                 fpi_ssm_usb_transfer_cb, NULL);
      }
      break;

    case M_INIT1_READ:
    case M_INIT2_READ:
      {
        /* Init response is 8 bytes */
        FpiUsbTransfer *transfer;

        transfer = fpi_usb_transfer_new (FP_DEVICE (self));
        transfer->short_is_error = TRUE;
        transfer->ssm = ssm;

        fpi_usb_transfer_fill_bulk (transfer, NB2033_EP_IN, NB2033_INIT_RECV_LEN);
        fpi_usb_transfer_submit (transfer, NB2033_DEFAULT_TIMEOUT,
                                 fpi_device_get_cancellable (FP_DEVICE (self)),
                                 fpi_ssm_usb_transfer_cb, NULL);
      }
      break;

    case M_INIT_SETTLE:
      /* Wait for init transients to decay before finger detection */
      fpi_ssm_next_state_delayed (ssm, NB2033_SETTLE_DELAY);
      break;

    case M_WAIT_PRINT:
      /* Wait before next finger detection poll */
      fpi_ssm_next_state_delayed (ssm, NB2033_TRANSITION_DELAY);
      break;

    case M_CONFIG1:
      /* First 0x0D(0x00) — Enhanced finger detect requires two configs */
      nb2033_write_ignore_read (self, nb2033_cmd_config,
                                G_N_ELEMENTS (nb2033_cmd_config));
      break;

    case M_CONFIG1_DONE:
      fpi_ssm_next_state (ssm);
      break;

    case M_CONFIG2:
      /* Second 0x0D(0x00) — completes Enhanced detect setup */
      nb2033_write_ignore_read (self, nb2033_cmd_config,
                                G_N_ELEMENTS (nb2033_cmd_config));
      break;

    case M_CONFIG2_DONE:
      fpi_ssm_next_state (ssm);
      break;

    case M_DETECT_SEND:
      {
        /* Send 0x38 finger detect command */
        FpiUsbTransfer *transfer;

        transfer = fpi_usb_transfer_new (FP_DEVICE (self));
        transfer->short_is_error = TRUE;
        transfer->ssm = ssm;

        fpi_usb_transfer_fill_bulk_full (transfer, NB2033_EP_OUT,
                                         nb2033_cmd_check_finger,
                                         G_N_ELEMENTS (nb2033_cmd_check_finger),
                                         NULL);
        fpi_usb_transfer_submit (transfer, NB2033_DEFAULT_TIMEOUT,
                                 fpi_device_get_cancellable (FP_DEVICE (self)),
                                 fpi_ssm_usb_transfer_cb, NULL);
      }
      break;

    case M_DETECT_READ:
      {
        /* Read 0x38 response and check finger level */
        FpiUsbTransfer *transfer;

        transfer = fpi_usb_transfer_new (FP_DEVICE (self));
        transfer->short_is_error = TRUE;
        transfer->ssm = ssm;

        fpi_usb_transfer_fill_bulk (transfer, NB2033_EP_IN, NB2033_CMD_RECV_LEN);
        fpi_usb_transfer_submit (transfer, NB2033_DEFAULT_TIMEOUT,
                                 fpi_device_get_cancellable (FP_DEVICE (self)),
                                 nb2033_check_fingerprint_cb, NULL);
      }
      break;

    case M_PRECAPTURE:
      /* Finger detected — report and send precapture config */
      fpi_image_device_report_finger_status (dev, TRUE);
      nb2033_write_ignore_read (self, nb2033_cmd_config,
                                G_N_ELEMENTS (nb2033_cmd_config));
      break;

    case M_PRECAPTURE_DONE:
      fpi_ssm_next_state (ssm);
      break;

    case M_CAPTURE:
      /* Send capture command */
      self->partial_received = 0;
      nb2033_write_ignore_read (self, nb2033_cmd_capture,
                                G_N_ELEMENTS (nb2033_cmd_capture));
      break;

    case M_CAPTURE_DONE:
      fpi_ssm_next_state (ssm);
      break;

    case M_CAPTURE_WAIT:
      /* Wait for sensor to prepare first row */
      fpi_ssm_next_state_delayed (ssm, NB2033_ROW_READY_DELAY);
      break;

    case M_READ_DATA:
      {
        /* Read 180 rows of image data */
        FpiUsbTransfer *transfer;

        transfer = fpi_usb_transfer_new (FP_DEVICE (self));
        transfer->ssm = ssm;

        fpi_usb_transfer_fill_bulk (transfer, NB2033_EP_IN, NB2033_ROW_RECV_LEN);
        fpi_usb_transfer_submit (transfer, NB2033_CAPTURE_TIMEOUT,
                                 fpi_device_get_cancellable (FP_DEVICE (self)),
                                 nb2033_read_capture_cb, NULL);
      }
      break;

    case M_SUBMIT_PRINT:
      if (submit_image (ssm, dev))
        {
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_report_finger_status (dev, FALSE);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, M_WAIT_PRINT);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
nb2033_dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceNb2033 *self = FPI_DEVICE_NB2033 (dev);
  FpiSsm *ssm_loop;

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      ssm_loop = fpi_ssm_new (FP_DEVICE (dev), m_loop_state, M_LOOP_NUM_STATES);
      self->ssm = ssm_loop;
      fpi_ssm_start (ssm_loop, m_loop_complete);
    }
}

static const FpIdEntry id_table[] = {
  { .vid = 0x298d,  .pid = 0x2033, },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },
};

static void
fpi_device_nb2033_init (FpiDeviceNb2033 *self)
{
}

static void
fpi_device_nb2033_class_init (FpiDeviceNb2033Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "NextBiometrics NB-2033-U";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_height = FRAME_HEIGHT;
  img_class->img_width = FRAME_WIDTH;

  img_class->bz3_threshold = 40;

  img_class->img_open = nb2033_dev_init;
  img_class->img_close = nb2033_dev_deinit;
  img_class->activate = nb2033_dev_activate;
  img_class->deactivate = nb2033_dev_deactivate;
  img_class->change_state = nb2033_dev_change_state;
}
