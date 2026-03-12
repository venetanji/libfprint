/*
 * FocalTech FT9201 Match-on-Host driver for libfprint
 *
 * Copyright (C) 2025 libfprint contributors
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
 * FocalTech FT9201 (chip FT9338, VID:2808 PID:9338)
 *
 * Area fingerprint sensor with USB SIU (Serial Interface Unit) bridge.
 * 64×80 pixels, 8-bit grayscale, match-on-host.
 *
 * MCU has ROM firmware — no firmware upload needed. The SIU uses a
 * "New SIU" protocol with compound register addresses.
 *
 * Read sequence (3 control OUTs + 1 bulk IN):
 *   1. OUT req=0x34 wValue=0x00FF  (prepare init)
 *   2. OUT req=0x34 wValue=0x0003  (prepare read mode)
 *   3. OUT req=0x6F wValue=size wIndex=compound_addr  (configure)
 *   4. Bulk IN on EP3 (read data)
 *
 * Compound addresses:
 *   0x9180 — chip status / OTP info
 *   0x9080 — image capture (5120 bytes)
 *   0xFF00 — sync / reset (size=0, no bulk IN)
 *
 * Important: after USB reset or first enumeration, the first bulk IN
 * read returns garbage (all 0x02). A warmup read must be performed
 * and its result discarded.
 */

#define FP_COMPONENT "focaltech_moh"

#include "drivers_api.h"
#include "focaltech_moh.h"

G_DEFINE_TYPE (FpiDeviceFocaltechMoh, fpi_device_focaltech_moh,
               FP_TYPE_IMAGE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = FT9201_VID, .pid = FT9201_PID },
  { .vid = 0, .pid = 0 },
};

/* ─── Helper: send vendor control OUT ──────────────────────────── */

static void
ft9201_ctrl_out (FpDevice  *dev,
                 FpiSsm    *ssm,
                 guint8     request,
                 guint16    value,
                 guint16    index)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 request, value, index, 0);
  fpi_usb_transfer_submit (transfer, FT9201_CMD_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

/* ─── Capture state machine ────────────────────────────────────── */

static void
capture_read_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);
  int state = fpi_ssm_get_cur_state (ssm);

  switch (state)
    {
    /*
     * Warmup: first bulk read after reset returns garbage.
     * Skip if already done.
     */
    case CAPTURE_WARMUP_PREP1:
      if (self->warmup_done)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_SYNC_PREP1);
          return;
        }
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_INIT, 0);
      break;

    case CAPTURE_WARMUP_PREP2:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_READ, 0);
      break;

    case CAPTURE_WARMUP_CMD:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_NEW_SIU_RW, 0x0020, FT9201_REG_STATUS);
      break;

    case CAPTURE_WARMUP_READ:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk (transfer, FT9201_EP_IN, 32);
        transfer->short_is_error = FALSE;
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, FT9201_CMD_TIMEOUT, NULL,
                                 capture_read_cb, NULL);
        self->warmup_done = TRUE;
      }
      break;

    /* Sync: poke 0xFF00 (no bulk read) */
    case CAPTURE_SYNC_PREP1:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_INIT, 0);
      break;

    case CAPTURE_SYNC_PREP2:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_READ, 0);
      break;

    case CAPTURE_SYNC_CMD:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_NEW_SIU_RW, 0, FT9201_REG_SYNC);
      break;

    /* Status: read 4 bytes from 0x9180 */
    case CAPTURE_STATUS_PREP1:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_INIT, 0);
      break;

    case CAPTURE_STATUS_PREP2:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_READ, 0);
      break;

    case CAPTURE_STATUS_CMD:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_NEW_SIU_RW, 4, FT9201_REG_STATUS);
      break;

    case CAPTURE_STATUS_READ:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk (transfer, FT9201_EP_IN, 32);
        transfer->short_is_error = FALSE;
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, FT9201_CMD_TIMEOUT, NULL,
                                 capture_read_cb, NULL);
      }
      break;

    /* Image capture: 5120 bytes from 0x9080 */
    case CAPTURE_IMG_PREP1:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_INIT, 0);
      break;

    case CAPTURE_IMG_PREP2:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_PREPARE, FT9201_PREPARE_READ, 0);
      break;

    case CAPTURE_IMG_CMD:
      ft9201_ctrl_out (dev, ssm, FT9201_REQ_NEW_SIU_RW,
                       FT9201_IMG_SIZE, FT9201_REG_CAPTURE);
      break;

    case CAPTURE_IMG_READ:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk_full (transfer, FT9201_EP_IN,
                                         self->image_buf, FT9201_IMG_SIZE,
                                         NULL);
        transfer->short_is_error = FALSE;
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, FT9201_CMD_TIMEOUT, NULL,
                                 capture_read_cb, NULL);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
capture_ssm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);
  FpImage *image;

  self->capture_ssm = NULL;

  if (self->deactivating)
    {
      g_clear_error (&error);
      fpi_image_device_deactivate_complete (img_dev, NULL);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (img_dev, error);
      return;
    }

  /* Check if image has meaningful data.
   * Warmup/blank images have very few unique pixel values.
   * A real fingerprint has 100+ unique values. */
  {
    gboolean seen[256] = { FALSE, };
    int unique = 0;
    int i;

    for (i = 0; i < FT9201_IMG_SIZE; i++)
      {
        if (!seen[self->image_buf[i]])
          {
            seen[self->image_buf[i]] = TRUE;
            unique++;
          }
      }

    if (unique < 50)
      {
        fp_dbg ("Skipping low-quality image (%d unique values)", unique);
        goto restart_capture;
      }
  }

  /* Report finger on, submit image, report finger off */
  image = fp_image_new (FT9201_IMG_WIDTH, FT9201_IMG_HEIGHT);
  memcpy (image->data, self->image_buf, FT9201_IMG_SIZE);
  image->flags = FPI_IMAGE_V_FLIPPED;

  fp_dbg ("Image captured (%d unique values), submitting",
          FT9201_IMG_SIZE);

  fpi_image_device_report_finger_status (img_dev, TRUE);
  fpi_image_device_image_captured (img_dev, image);
  fpi_image_device_report_finger_status (img_dev, FALSE);

restart_capture:
  /* Restart capture loop with a short delay to avoid busy-polling */
  if (!self->deactivating)
    {
      self->capture_ssm = fpi_ssm_new (FP_DEVICE (dev), capture_ssm_handler,
                                       CAPTURE_NUM_STATES);
      fpi_ssm_start (self->capture_ssm, capture_ssm_complete);
    }
}

/* ─── Device lifecycle ─────────────────────────────────────────── */

static void
dev_open (FpImageDevice *dev)
{
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);
  GError *error = NULL;

  G_DEBUG_HERE ();

  /* Reset USB device to clear any stuck bulk IN pipe state.
   * Without this, the first bulk read may timeout if the pipe
   * was left in a bad state from a previous session. */
  if (!g_usb_device_reset (fpi_device_get_usb_device (FP_DEVICE (dev)), &error))
    {
      fp_dbg ("USB reset failed (non-fatal): %s", error->message);
      g_clear_error (&error);
    }

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                     0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->image_buf = g_malloc0 (FT9201_IMG_SIZE);
  self->warmup_done = FALSE;

  fpi_image_device_open_complete (dev, NULL);
}

static void
dev_close (FpImageDevice *dev)
{
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);
  GError *error = NULL;

  G_DEBUG_HERE ();

  g_clear_pointer (&self->image_buf, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);

  G_DEBUG_HERE ();

  self->deactivating = FALSE;

  self->capture_ssm = fpi_ssm_new (FP_DEVICE (dev), capture_ssm_handler,
                                   CAPTURE_NUM_STATES);
  fpi_ssm_start (self->capture_ssm, capture_ssm_complete);

  fpi_image_device_activate_complete (dev, NULL);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceFocaltechMoh *self = FPI_DEVICE_FOCALTECH_MOH (dev);

  G_DEBUG_HERE ();

  if (!self->capture_ssm)
    fpi_image_device_deactivate_complete (dev, NULL);
  else
    self->deactivating = TRUE;
}

/* ─── GType boilerplate ────────────────────────────────────────── */

static void
fpi_device_focaltech_moh_init (FpiDeviceFocaltechMoh *self)
{
}

static void
fpi_device_focaltech_moh_class_init (FpiDeviceFocaltechMohClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "focaltech_moh";
  dev_class->full_name = "FocalTech FT9201 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;

  img_class->img_open = dev_open;
  img_class->img_close = dev_close;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;

  img_class->img_width = FT9201_IMG_WIDTH;
  img_class->img_height = FT9201_IMG_HEIGHT;
  img_class->bz3_threshold = 24;
}
