/*
 * Futronic FS88 driver for libfprint
 *
 * Copyright (C) 2026 Zsolt Lengyel
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
 * Futronic FS88 (1491:0088), a vendor-class USB 2.0 press sensor.
 *
 * The protocol was recovered by tracing Futronic's own libScanAPI.so against the
 * hardware. Every exchange is a short bulk write to EP 0x01 followed by a
 * fixed-size bulk read from EP 0x82:
 *
 *   0xe0                 ->    512 bytes  device info
 *   0x50                 ->    512 bytes  status
 *   0x38                 ->    512 bytes  geometry (supported image sizes)
 *   0x9d 0x40 <lo> <hi>  ->    512 bytes  read calibration block at LE address
 *   0x6a 0x2d            -> 307200 bytes  capture one raw frame
 *
 * The device answers every command, so a reply must always be drained or the
 * next exchange desynchronises.
 *
 * Frames are 384x800 8-bit greyscale, ridges bright on a dark platen -- hence
 * FPI_IMAGE_COLORS_INVERTED below. With nothing on the sensor a frame reads a
 * mean intensity of about 2, which makes mean intensity a cheap and dependable
 * finger-presence test.
 */

#define FP_COMPONENT "futronic"
#include "fpi-log.h"

#include "drivers_api.h"

#include <math.h>

#define FTR_IMG_WIDTH 384
#define FTR_IMG_HEIGHT 800
#define FTR_FRAME_SIZE (FTR_IMG_WIDTH * FTR_IMG_HEIGHT)

/*
 * Scan resolution, pixels per millimetre. FpImage leaves this at zero unless a
 * driver fills it in, and NBIS sizes its quality-map neighbourhood from it
 * (radius_pix = RADIUS_MM * ppmm), so leaving it unset degrades the reliability
 * scores attached to every minutia.
 */
#define FTR_PPMM 19.685               /* 500 dpi / 25.4 mm per inch */

/*
 * Local contrast normalisation.
 *
 * The sensor returns a faithful but low-contrast image, and finger pressure
 * varies the level across the platen. Renormalising each pixel against the mean
 * and standard deviation of its neighbourhood corrects both, and is the standard
 * fingerprint pre-filter.
 *
 * Tuned against actual bozorth3 match scores -- eight separate presses of one
 * finger, each in turn scored against a five-press template, plus eight presses
 * of a different finger as impostors. Minutiae count was a tempting metric but a
 * misleading one: noise amplification raises the count while degrading matching.
 *
 *   config          genuine min/median   impostor max   margin
 *   raw               95 / 185                19         +76
 *   r12 gain 3.0     121 / 169                25         +96
 *   r16 gain 3.0     120 / 165                22         +98
 *   r12 gain 8.0     118 / 161                25         +93
 *
 * Every variant clears the threshold of 40 comfortably, so this is a margin
 * optimisation rather than a fix. What normalisation buys is the *worst* genuine
 * score, 95 -> 120, and that is what governs false rejects on a dry or partial
 * press. Differences among the normalised variants are within the noise of an
 * eight-sample test; radius 16 is chosen for the lowest impostor scores.
 */
#define FTR_NORM_RADIUS 16
#define FTR_NORM_TARGET_MEAN 128.0
#define FTR_NORM_TARGET_STD 48.0

/*
 * Cap on amplification. Genuine ridge areas measure a local standard deviation
 * around 25 and so never need more than about 2x; anything demanding far more
 * gain is weak contact or sensor noise, and amplifying it to full contrast just
 * manufactures ridges that NBIS then trusts. Capping keeps those regions visibly
 * weak so NBIS's own low-contrast rejection can discard them.
 */
#define FTR_NORM_MAX_GAIN 3.0
#define FTR_NORM_MIN_STD (FTR_NORM_TARGET_STD / FTR_NORM_MAX_GAIN)

#define FTR_EP_OUT (0x01 | FPI_USB_ENDPOINT_OUT)
#define FTR_EP_IN (0x02 | FPI_USB_ENDPOINT_IN)

#define FTR_REPLY_LEN 512

/*
 * A frame is read in FTR_FRAME_CHUNK-byte bulk transfers rather than one
 * 307200-byte transfer. The sensor streams a frame as one run of 512-byte
 * packets and never sees host-side transfer boundaries, so nothing changes on
 * the wire; libusb-0.1, which Futronic's own library is built on, splits bulk
 * reads at exactly this size.
 *
 * It exists for the umockdev test. Above 16384 bytes usbfs builds
 * scatter-gather URBs, and usbmon cannot copy their data when the IOMMU has
 * coalesced the segments (it records the 'D' flag and an empty payload), so a
 * capture of larger transfers is silently blank. Small URBs also stay well
 * inside usbmon's per-URB ring limit and libpcap's 262144-byte snapshot.
 * Must be a multiple of 512; the last chunk of a frame is simply shorter.
 */
#define FTR_FRAME_CHUNK 16384

#define FTR_TIMEOUT 5000
#define FTR_FRAME_TIMEOUT 10000

/* Frames arrive at roughly 13/s, so polling faster than this buys nothing. */
#define FTR_POLL_DELAY 40

/*
 * A finger keeps pressing down for a moment after it first touches; captures
 * taken immediately are noticeably fainter than ones taken after it settles.
 *
 * Crucially this settling is done by *continuing to request frames*, never by
 * idling. The sensor's illumination switches itself off when it stops receiving
 * frame requests -- the vendor API exposes ftrScanMainLEDsTimeout for exactly
 * this -- and Futronic's own library polls continuously for that reason. An idle
 * delay here returns a dark, near-empty frame.
 *
 * Nine frames at roughly 13/s is about 700ms of settling.
 */
#define FTR_SETTLE_FRAMES 9

/*
 * Presence thresholds on mean frame intensity. Bare platen sits near 2 and a
 * pressed finger runs well above 15; the gap is wide, and the hysteresis keeps a
 * finger lifting slowly from retriggering a capture.
 *
 * These deliberately stay permissive. An optical platen holds a greasy latent
 * print after the finger lifts, and that residue can read high enough to look
 * like a finger; rather than raise the bar here and start ignoring light presses,
 * marginal captures are rejected afterwards on image quality.
 */
#define FTR_FINGER_ON_MEAN 12
#define FTR_FINGER_OFF_MEAN 8

/*
 * Quality gate, applied to the normalised image.
 *
 * A latent residue print looks like a real finger to a presence check and, once
 * normalised, carries just enough structure for NBIS to extract minutiae from --
 * which then match nothing. Submitting it produces a confident-looking failure:
 * observed bozorth3 scores of 5-9 against every template stage, where genuine
 * presses score 42-297.
 *
 * Mean intensity cannot detect this, because normalisation re-centres every
 * image on 128 by construction. Standard deviation can, and cleanly:
 *
 *   16 known-good presses   std 31.5 .. 36.8
 *   latent ghost capture    std 11.3
 *
 * 20 sits about 1.6x clear of both sides. Below it, ask for another scan instead
 * of handing the matcher something meaningless -- which matters most during
 * enrolment, where one bad stage quietly poisons the stored template.
 */
#define FTR_MIN_QUALITY_STD 20.0

typedef struct
{
  guint8 len;
  guint8 data[4];
} FtrCommand;

static const FtrCommand ftr_cmd_get_frame = { 2, { 0x6a, 0x2d } };

/*
 * The scanner returns only dark frames until it has been walked through the
 * vendor's full open sequence: three status/geometry queries, then fourteen
 * calibration blocks at little-endian addresses 0x0040..0x0380 stepping 0x40.
 * Replayed verbatim from the trace.
 */
static const FtrCommand ftr_init_cmds[] = {
  { 1, { 0xe0 } },
  { 1, { 0x50 } },
  { 1, { 0x38 } },
  { 4, { 0x9d, 0x40, 0x40, 0x00 } },
  { 4, { 0x9d, 0x40, 0x80, 0x00 } },
  { 4, { 0x9d, 0x40, 0xc0, 0x00 } },
  { 4, { 0x9d, 0x40, 0x00, 0x01 } },
  { 4, { 0x9d, 0x40, 0x40, 0x01 } },
  { 4, { 0x9d, 0x40, 0x80, 0x01 } },
  { 4, { 0x9d, 0x40, 0xc0, 0x01 } },
  { 4, { 0x9d, 0x40, 0x00, 0x02 } },
  { 4, { 0x9d, 0x40, 0x40, 0x02 } },
  { 4, { 0x9d, 0x40, 0x80, 0x02 } },
  { 4, { 0x9d, 0x40, 0xc0, 0x02 } },
  { 4, { 0x9d, 0x40, 0x00, 0x03 } },
  { 4, { 0x9d, 0x40, 0x40, 0x03 } },
  { 4, { 0x9d, 0x40, 0x80, 0x03 } },
};

/* Open sequence */
enum {
  INIT_WRITE,
  INIT_READ,
  INIT_NUM_STATES,
};

/* Capture loop */
enum {
  M_WAIT,
  M_REQ_DETECT,
  M_READ_DETECT,
  M_SETTLE,
  M_REQ_CAPTURE,
  M_READ_CAPTURE,
  M_SUBMIT,
  M_REQ_FINGER_OFF,
  M_READ_FINGER_OFF,
  M_LOOP_NUM_STATES,
};

struct _FpiDeviceFutronic
{
  FpImageDevice parent;
  FpiSsm       *ssm;
  guint8       *frame_buf;
  gsize         frame_got;
  guint         init_idx;
  guint         settle_count;
  gboolean      deactivating;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFutronic, fpi_device_futronic, FPI, DEVICE_FUTRONIC, FpImageDevice)
G_DEFINE_TYPE (FpiDeviceFutronic, fpi_device_futronic, FP_TYPE_IMAGE_DEVICE)

/* ------------------------------ helpers ------------------------------ */

static guint
ftr_frame_mean (const guint8 *frame)
{
  guint64 sum = 0;
  gsize i;

  for (i = 0; i < FTR_FRAME_SIZE; i++)
    sum += frame[i];

  return (guint) (sum / FTR_FRAME_SIZE);
}

/*
 * Rescale every pixel against its local neighbourhood, in place.
 *
 * Uses summed-area tables so the cost is independent of the window size: two
 * O(n) passes rather than O(n * r^2). The tables hold the original values, so
 * rewriting the image as we go is safe.
 */
static void
ftr_normalize_contrast (guint8 *img)
{
  const gint w = FTR_IMG_WIDTH;
  const gint h = FTR_IMG_HEIGHT;
  const gint r = FTR_NORM_RADIUS;
  const gint stride = w + 1;
  guint64 *sum, *sumsq;
  gint x, y;

  /*
   * 64-bit throughout: the squared-value table reaches 255^2 * 307200, roughly
   * 2.0e10, which overflows 32 bits.
   */
  sum = g_new0 (guint64, (gsize) stride * (h + 1));
  sumsq = g_new0 (guint64, (gsize) stride * (h + 1));

  for (y = 0; y < h; y++)
    {
      guint64 row = 0, row_sq = 0;

      for (x = 0; x < w; x++)
        {
          guint8 v = img[y * w + x];

          row += v;
          row_sq += (guint64) v * v;

          sum[(y + 1) * stride + (x + 1)] = sum[y * stride + (x + 1)] + row;
          sumsq[(y + 1) * stride + (x + 1)] = sumsq[y * stride + (x + 1)] + row_sq;
        }
    }

  for (y = 0; y < h; y++)
    {
      gint y0 = MAX (y - r, 0);
      gint y1 = MIN (y + r, h - 1);

      for (x = 0; x < w; x++)
        {
          gint x0 = MAX (x - r, 0);
          gint x1 = MIN (x + r, w - 1);
          guint64 area = (guint64) (x1 - x0 + 1) * (y1 - y0 + 1);
          guint64 s, s2;
          gdouble mean, var, sd, out;

          s = sum[(y1 + 1) * stride + (x1 + 1)]
              - sum[y0 * stride + (x1 + 1)]
              - sum[(y1 + 1) * stride + x0]
              + sum[y0 * stride + x0];

          s2 = sumsq[(y1 + 1) * stride + (x1 + 1)]
               - sumsq[y0 * stride + (x1 + 1)]
               - sumsq[(y1 + 1) * stride + x0]
               + sumsq[y0 * stride + x0];

          mean = (gdouble) s / area;
          var = (gdouble) s2 / area - mean * mean;
          sd = sqrt (MAX (var, 0.0));

          out = FTR_NORM_TARGET_MEAN
                + FTR_NORM_TARGET_STD * (img[y * w + x] - mean)
                / MAX (sd, FTR_NORM_MIN_STD);

          img[y * w + x] = (guint8) CLAMP (out, 0.0, 255.0);
        }
    }

  g_free (sum);
  g_free (sumsq);
}

static void
ftr_write_cmd (FpiDeviceFutronic     *self,
               const FtrCommand      *cmd,
               FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->short_is_error = TRUE;
  transfer->ssm = self->ssm;

  fpi_usb_transfer_fill_bulk_full (transfer, FTR_EP_OUT,
                                   (guint8 *) cmd->data, cmd->len, NULL);
  fpi_usb_transfer_submit (transfer, FTR_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           callback, NULL);
}

static void
ftr_read_reply (FpiDeviceFutronic     *self,
                gsize                  len,
                guint                  timeout,
                FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->short_is_error = TRUE;
  transfer->ssm = self->ssm;

  fpi_usb_transfer_fill_bulk (transfer, FTR_EP_IN, len);
  fpi_usb_transfer_submit (transfer, timeout,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           callback, NULL);
}

/*
 * Read one full frame into self->frame_buf, chunk by chunk, and hand the last
 * transfer to @callback once the whole frame is in (or as soon as a chunk
 * fails). Callbacks read the image from self->frame_buf, not transfer->buffer.
 */
static void ftr_read_frame_chunk (FpiDeviceFutronic     *self,
                                  FpiUsbTransferCallback callback);

static void
ftr_frame_chunk_cb (FpiUsbTransfer *transfer,
                    FpDevice       *device,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);
  FpiUsbTransferCallback callback = user_data;

  if (!error)
    {
      self->frame_got += transfer->actual_length;
      if (self->frame_got < FTR_FRAME_SIZE)
        {
          ftr_read_frame_chunk (self, callback);
          return;
        }
    }

  callback (transfer, device, NULL, error);
}

static void
ftr_read_frame_chunk (FpiDeviceFutronic     *self,
                      FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer;
  gsize len = MIN (FTR_FRAME_CHUNK, FTR_FRAME_SIZE - self->frame_got);

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->short_is_error = TRUE;
  transfer->ssm = self->ssm;

  fpi_usb_transfer_fill_bulk_full (transfer, FTR_EP_IN,
                                   self->frame_buf + self->frame_got, len, NULL);
  fpi_usb_transfer_submit (transfer, FTR_FRAME_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           ftr_frame_chunk_cb, callback);
}

static void
ftr_read_frame (FpiDeviceFutronic     *self,
                FpiUsbTransferCallback callback)
{
  self->frame_got = 0;
  ftr_read_frame_chunk (self, callback);
}

/* ------------------------------ open ------------------------------ */

static void
ftr_init_read_cb (FpiUsbTransfer *transfer,
                  FpDevice       *device,
                  gpointer        user_data,
                  GError         *error)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->init_idx++;
  if (self->init_idx >= G_N_ELEMENTS (ftr_init_cmds))
    fpi_ssm_mark_completed (transfer->ssm);
  else
    fpi_ssm_jump_to_state (transfer->ssm, INIT_WRITE);
}

static void
ftr_init_ssm_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_WRITE:
      ftr_write_cmd (self, &ftr_init_cmds[self->init_idx],
                     fpi_ssm_usb_transfer_cb);
      break;

    case INIT_READ:
      ftr_read_reply (self, FTR_REPLY_LEN, FTR_TIMEOUT, ftr_init_read_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
ftr_init_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

  self->ssm = NULL;
  fpi_image_device_open_complete (dev, error);
}

static void
ftr_dev_open (FpImageDevice *dev)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                     0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->frame_buf = g_malloc0 (FTR_FRAME_SIZE);
  self->init_idx = 0;

  self->ssm = fpi_ssm_new (FP_DEVICE (dev), ftr_init_ssm_state, INIT_NUM_STATES);
  fpi_ssm_start (self->ssm, ftr_init_ssm_done);
}

static void
ftr_dev_close (FpImageDevice *dev)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);
  GError *error = NULL;

  g_clear_pointer (&self->frame_buf, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

/* --------------------------- capture loop --------------------------- */

static void
ftr_detect_cb (FpiUsbTransfer *transfer,
               FpDevice       *device,
               gpointer        user_data,
               GError         *error)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);
  guint mean;

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

  mean = ftr_frame_mean (self->frame_buf);
  fp_dbg ("detect frame mean %u", mean);

  if (mean >= FTR_FINGER_ON_MEAN)
    fpi_ssm_next_state (transfer->ssm);
  else
    fpi_ssm_jump_to_state (transfer->ssm, M_WAIT);
}

static void
ftr_capture_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

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

  /*
   * Each read lands in frame_buf, so the most recent frame is always the one
   * held. Keep polling until the finger has settled, and keep the requests
   * back-to-back: pausing lets the sensor's illumination time out, which
   * yields a dark frame.
   */
  if (++self->settle_count < FTR_SETTLE_FRAMES)
    {
      fpi_ssm_jump_to_state (transfer->ssm, M_REQ_CAPTURE);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
ftr_finger_off_cb (FpiUsbTransfer *transfer,
                   FpDevice       *device,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

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

  if (ftr_frame_mean (self->frame_buf) > FTR_FINGER_OFF_MEAN)
    {
      fpi_ssm_jump_to_state (transfer->ssm, M_REQ_FINGER_OFF);
      return;
    }

  /*
   * Retire the SSM *before* announcing finger-off, and let the announcement be
   * the last thing this callback does.
   *
   * Reporting finger-off re-enters libfprint synchronously, and what happens
   * next depends on the caller. A single capture deactivates the device; a
   * multi-stage enrolment instead comes straight back through change_state()
   * asking for the next scan. That restart is dropped if it finds an SSM still
   * in flight, which strands the device with nothing driving it. Completing
   * first means self->ssm is already NULL either way: the restart takes hold,
   * and the deactivate path runs exactly once instead of racing this callback
   * to fpi_image_device_deactivate_complete().
   *
   * fpi_ssm_mark_completed() runs ftr_loop_ssm_done() synchronously and frees
   * the machine, so transfer->ssm must not be touched afterwards.
   */
  fpi_ssm_mark_completed (transfer->ssm);

  if (!self->deactivating)
    fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (device), FALSE);
}

/* Spread of the normalised image; see FTR_MIN_QUALITY_STD. */
static gdouble
ftr_image_stddev (const guint8 *img)
{
  guint64 sum = 0, sum_sq = 0;
  gdouble mean;
  gsize i;

  for (i = 0; i < FTR_FRAME_SIZE; i++)
    {
      sum += img[i];
      sum_sq += (guint64) img[i] * img[i];
    }

  mean = (gdouble) sum / FTR_FRAME_SIZE;
  return sqrt (MAX ((gdouble) sum_sq / FTR_FRAME_SIZE - mean * mean, 0.0));
}

/*
 * Returns FALSE if the frame was too poor to submit, having already asked
 * libfprint for another scan.
 */
static gboolean
ftr_submit_image (FpImageDevice *dev)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);
  FpImage *img;
  gdouble quality;

  img = fp_image_new (FTR_IMG_WIDTH, FTR_IMG_HEIGHT);

  /* Ridges come back bright against a dark platen; let libfprint invert. */
  img->flags = FPI_IMAGE_COLORS_INVERTED;
  img->ppmm = FTR_PPMM;
  memcpy (img->data, self->frame_buf, FTR_FRAME_SIZE);

  /*
   * Normalise before handing the image over. Order matters only in that this
   * operates on the sensor's own polarity; libfprint applies the inversion flag
   * afterwards, during minutiae detection.
   */
  ftr_normalize_contrast (img->data);

  quality = ftr_image_stddev (img->data);
  if (quality < FTR_MIN_QUALITY_STD)
    {
      fp_dbg ("rejecting capture: quality %.1f below %.1f",
              quality, FTR_MIN_QUALITY_STD);
      g_object_unref (img);
      fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_CENTER_FINGER);
      return FALSE;
    }

  fp_dbg ("submitting capture: quality %.1f", quality);
  fpi_image_device_image_captured (dev, img);
  return TRUE;
}

static void
ftr_loop_ssm_state (FpiSsm *ssm, FpDevice *device)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTR_POLL_DELAY);
      break;

    case M_REQ_DETECT:
      ftr_write_cmd (self, &ftr_cmd_get_frame, fpi_ssm_usb_transfer_cb);
      break;

    case M_READ_DETECT:
      ftr_read_frame (self, ftr_detect_cb);
      break;

    case M_SETTLE:
      fpi_image_device_report_finger_status (dev, TRUE);
      self->settle_count = 0;
      fpi_ssm_next_state (ssm);
      break;

    case M_REQ_CAPTURE:
      ftr_write_cmd (self, &ftr_cmd_get_frame, fpi_ssm_usb_transfer_cb);
      break;

    case M_READ_CAPTURE:
      ftr_read_frame (self, ftr_capture_cb);
      break;

    case M_SUBMIT:
      /*
       * Either way the finger is still down, so fall through to waiting for it
       * to lift. On rejection libfprint has already been told to expect another
       * scan, and the next press starts a fresh loop.
       */
      ftr_submit_image (dev);
      fpi_ssm_next_state (ssm);
      break;

    /*
     * Wait for the finger to actually leave before finishing. Without this a
     * multi-stage enrolment would immediately recapture the same still-resting
     * finger and burn through its stages.
     */
    case M_REQ_FINGER_OFF:
      ftr_write_cmd (self, &ftr_cmd_get_frame, fpi_ssm_usb_transfer_cb);
      break;

    case M_READ_FINGER_OFF:
      ftr_read_frame (self, ftr_finger_off_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
ftr_loop_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (device);

  self->ssm = NULL;

  if (self->deactivating)
    fpi_image_device_deactivate_complete (dev, error);
  else if (error)
    fpi_image_device_session_error (dev, error);
}

/* --------------------------- activation --------------------------- */

static void
ftr_dev_activate (FpImageDevice *dev)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);

  self->deactivating = FALSE;
  fpi_image_device_activate_complete (dev, NULL);
}

static void
ftr_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);

  self->deactivating = TRUE;

  /* If no loop is running there is nothing to unwind. */
  if (self->ssm == NULL)
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
ftr_dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceFutronic *self = FPI_DEVICE_FUTRONIC (dev);

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return;

  if (self->ssm != NULL)
    return;

  self->ssm = fpi_ssm_new (FP_DEVICE (dev), ftr_loop_ssm_state, M_LOOP_NUM_STATES);
  fpi_ssm_start (self->ssm, ftr_loop_ssm_done);
}

/* ------------------------------ class ------------------------------ */

static const FpIdEntry id_table[] = {
  { .vid = 0x1491, .pid = 0x0088, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_futronic_init (FpiDeviceFutronic *self)
{
}

static void
fpi_device_futronic_class_init (FpiDeviceFutronicClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Futronic FS88";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_width = FTR_IMG_WIDTH;
  img_class->img_height = FTR_IMG_HEIGHT;

  /*
   * A 384x800 print at 500 dpi yields plenty of minutiae, so there is no reason
   * to relax below libfprint's default and weaken matching.
   */
  img_class->bz3_threshold = 40;

  img_class->img_open = ftr_dev_open;
  img_class->img_close = ftr_dev_close;
  img_class->activate = ftr_dev_activate;
  img_class->deactivate = ftr_dev_deactivate;
  img_class->change_state = ftr_dev_change_state;
}
