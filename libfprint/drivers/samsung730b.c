/*
 * Samsung 730B fingerprint sensor driver for libfprint
 *
 * Sensor information:
 *   Manufacturer: Samsung Electro-Mechanics
 *   Sensor type:  Press (area) sensor
 *   Image size:   112 x 96 pixels
 *   Resolution:   ~500 DPI
 *   USB Vendor:   0x04e8 (Samsung)
 *   USB Product:  0x730b
 *
 * The sensor outputs grayscale images with light ridges on a dark background
 * (inverted polarity). This driver applies CLAHE + contrast stretching +
 * unsharp masking to enhance ridge visibility before minutiae extraction.
 *
 * Due to the small image size (112x96), images are upscaled 2x before passing
 * to NBIS for more reliable minutiae detection.
 *
 * Copyright (C) 2025 Jang Han-gil <lignah1@icloud.com>
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

#define FP_COMPONENT "samsung730b"

#include "drivers_api.h"

#include <math.h>

/* Access to FpImage internal fields (flags, ppmm) */
#include "../fpi-image.h"

/* USB device identifiers */
#define SAMSUNG730B_VID 0x04e8
#define SAMSUNG730B_PID 0x730b

/* USB endpoints */
#define SAMSUNG730B_EP_IN   (0x82 | FPI_USB_ENDPOINT_IN)
#define SAMSUNG730B_EP_OUT  (0x01 | FPI_USB_ENDPOINT_OUT)
#define BULK_PACKET_SIZE    256

/* Sensor image geometry.
 * The raw USB data contains a 180-byte header before the actual pixel data.
 */
#define IMG_W      112
#define IMG_H      96
#define IMG_OFFSET 180

/* NBIS minutiae detection and bozorth matching tend to be unreliable on very
 * small images; upscale before handing off.
 */
#define S730B_RESIZE_FACTOR 2

/*
 * ROI (Region of Interest) statistics for image quality assessment.
 * These metrics help determine whether a captured frame has sufficient
 * quality for reliable minutiae extraction.
 */
typedef struct
{
  double mean;        /* Mean pixel intensity (0-255) */
  double stddev;      /* Standard deviation of pixel intensities */
  double sat0_pct;    /* Percentage of pixels at minimum (black) saturation */
  double sat255_pct;  /* Percentage of pixels at maximum (white) saturation */
  double grad_mean;   /* Mean gradient magnitude - indicates edge/ridge strength */
} S730bRoiStats;

static gboolean
s730b_calc_roi_stats (const guint8 *raw,
                      gsize         raw_len,
                      S730bRoiStats *out)
{
  gsize needed = IMG_OFFSET + (gsize) IMG_W * IMG_H;
  if (!raw || raw_len < needed || !out)
    return FALSE;

  const guint8 *roi = raw + IMG_OFFSET;
  const gsize roi_len = (gsize) IMG_W * IMG_H;
  guint64 sum = 0;
  guint64 sumsq = 0;
  guint64 grad_sum = 0;
  gsize sat0 = 0;
  gsize sat255 = 0;

  for (gsize i = 0; i < roi_len; i++)
    {
      const guint8 v = roi[i];
      sum += v;
      sumsq += (guint64) v * (guint64) v;
      if (v == 0x00)
        sat0++;
      else if (v == 0xFF)
        sat255++;
    }

  /* Very small, cheap edge/texture proxy: mean absolute forward differences.
   * Higher values typically correlate with clearer ridges (more minutiae).
   */
  for (int y = 0; y < (IMG_H - 1); y++)
    {
      for (int x = 0; x < (IMG_W - 1); x++)
        {
          const gsize idx = (gsize) y * IMG_W + (gsize) x;
          const int v = roi[idx];
          const int vx = roi[idx + 1];
          const int vy = roi[idx + IMG_W];
          grad_sum += (guint64) ABS (vx - v);
          grad_sum += (guint64) ABS (vy - v);
        }
    }

  const double mean = (double) sum / (double) roi_len;
  const double var = ((double) sumsq / (double) roi_len) - (mean * mean);
  const double stddev = var > 0.0 ? sqrt (var) : 0.0;

  out->mean = mean;
  out->stddev = stddev;
  out->sat0_pct = 100.0 * (double) sat0 / (double) roi_len;
  out->sat255_pct = 100.0 * (double) sat255 / (double) roi_len;
  out->grad_mean = (double) grad_sum / (double) ((IMG_W - 1) * (IMG_H - 1) * 2);
  return TRUE;
}

static double
s730b_roi_score (const S730bRoiStats *s)
{
  /* Higher is better.
   * - Prefer more ridge/edge energy (grad_mean).
   * - Penalize heavy saturation.
   * - Strongly reject blank/flat frames.
   */
  if (s->mean < 5.0 || s->stddev < 5.0 || s->sat0_pct > 95.0)
    return -1e12;

  /* sat255 is particularly harmful (clipped highlights destroy ridge detail),
   * so penalize it aggressively even if grad/std look good.
   */
  const double mean_penalty = fabs (s->mean - 128.0);
  return (s->grad_mean * 30.0)
         + (s->stddev * 2.0)
         - (s->sat255_pct * 35.0)
         - (s->sat0_pct * 10.0)
         - (mean_penalty * 1.0);
}

static gboolean
s730b_roi_needs_retry (const S730bRoiStats *s)
{
  /* Heuristics tuned from observed failures:
   * - Very high saturation or very low contrast often leads to < 10 minutiae.
   * - sat255 is especially harmful; even 35-40% destroys ridge detail.
   * - Low gradient (grad_mean) correlates with poor minutiae extraction.
   * 
   * NOTE: These thresholds have been relaxed to allow more images through,
   * since preprocessing (CLAHE + contrast stretch) can recover some quality.
   */
  const double sat_clip = MAX (s->sat0_pct, s->sat255_pct);

  if (s->stddev < 10.0)
    return TRUE;

  /* Stricter threshold: sat255 > 45% almost always yields < 10 minutiae. */
  if (s->sat255_pct > 45.0)
    return TRUE;

  if (sat_clip > 60.0)
    return TRUE;

  /* Low gradient typically means poor minutiae. */
  if (s->grad_mean < 18.0)
    return TRUE;

  /* Combined condition: high saturation with very low gradient is bad. */
  if (sat_clip > 40.0 && s->grad_mean < 20.0)
    return TRUE;

  return FALSE;
}

static gboolean
s730b_roi_needs_retry_for_action (const S730bRoiStats *s,
                                 FpiDeviceAction action)
{
  if (s730b_roi_needs_retry (s))
    return TRUE;

  /* ENROLL must be stricter: if we store low-minutiae templates, VERIFY will
   * permanently fail because bozorth skips gallery entries with < 10 minutiae.
   * 
   * NOTE: These thresholds have been relaxed since we now apply additional
   * contrast stretching after CLAHE.
   */
  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      /* Enroll quality gate: was sat255>42 which rejected many real
       * presses on this sensor (sat often 40–50% with usable ridges).
       * Keep a ceiling so totally blown-out frames still retry.
       */
      if (s->sat255_pct > 52.0)
        return TRUE;

      /* Require decent gradient for enrollment. */
      if (s->grad_mean < 18.0)
        return TRUE;

      /* Combined: very high sat255 needs higher gradient. */
      if (s->sat255_pct > 45.0 && s->grad_mean < 20.0)
        return TRUE;
    }

  return FALSE;
}

/*
 * Image preprocessing functions to enhance fingerprint ridges before NBIS.
 * These help improve minutiae extraction on low-contrast sensor images.
 */

/* Unsharp masking: sharpen edges by subtracting blurred version
 * output = original + amount * (original - blurred)
 */
static void
s730b_preproc_unsharp_mask (guint8 *data, int width, int height, double amount)
{
  const int len = width * height;
  g_autofree gint16 *blurred = g_new0 (gint16, len);
  
  /* Simple 3x3 box blur */
  for (int y = 1; y < height - 1; y++)
    {
      for (int x = 1; x < width - 1; x++)
        {
          int sum = 0;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
              sum += data[(y + dy) * width + (x + dx)];
          blurred[y * width + x] = (gint16)(sum / 9);
        }
    }
  
  /* Handle borders */
  for (int x = 0; x < width; x++)
    {
      blurred[x] = data[x];
      blurred[(height - 1) * width + x] = data[(height - 1) * width + x];
    }
  for (int y = 0; y < height; y++)
    {
      blurred[y * width] = data[y * width];
      blurred[y * width + width - 1] = data[y * width + width - 1];
    }
  
  /* Apply unsharp mask: output = original + amount * (original - blurred) */
  for (int i = 0; i < len; i++)
    {
      double val = data[i] + amount * ((double)data[i] - blurred[i]);
      data[i] = (guint8) CLAMP((int)val, 0, 255);
    }
}

/* Simple contrast stretching: linearly map [min, max] to [0, 255] */
static void
s730b_preproc_contrast_stretch (guint8 *data, gsize len)
{
  if (len == 0)
    return;

  guint8 min_val = 255;
  guint8 max_val = 0;

  for (gsize i = 0; i < len; i++)
    {
      if (data[i] < min_val) min_val = data[i];
      if (data[i] > max_val) max_val = data[i];
    }

  /* Use 1st/99th percentile to avoid outlier influence */
  guint hist[256] = {0};
  for (gsize i = 0; i < len; i++)
    hist[data[i]]++;

  gsize count = 0;
  gsize p1_target = len / 100;      /* 1st percentile */
  gsize p99_target = len * 99 / 100; /* 99th percentile */
  guint8 p1_val = 0, p99_val = 255;

  for (int v = 0; v < 256; v++)
    {
      count += hist[v];
      if (count >= p1_target && p1_val == 0)
        p1_val = (guint8) v;
      if (count >= p99_target)
        {
          p99_val = (guint8) v;
          break;
        }
    }

  if (p99_val <= p1_val)
    return; /* Can't stretch */

  const double scale = 255.0 / (double)(p99_val - p1_val);
  for (gsize i = 0; i < len; i++)
    {
      int v = (int)data[i];
      if (v <= p1_val)
        data[i] = 0;
      else if (v >= p99_val)
        data[i] = 255;
      else
        data[i] = (guint8)((v - p1_val) * scale);
    }
}

/*
 * CLAHE (Contrast Limited Adaptive Histogram Equalization)
 *
 * Unlike global histogram equalization, CLAHE divides the image into tiles
 * and applies histogram equalization to each tile separately. This preserves
 * local contrast while avoiding over-amplification of noise.
 *
 * The clip_limit parameter controls contrast amplification - higher values
 * allow more contrast but may amplify noise.
 *
 * This is the standard preprocessing technique for fingerprint images.
 */
static void
s730b_preproc_clahe (guint8 *data, int width, int height, double clip_limit)
{
  const int grid_x = 8;
  const int grid_y = 8;
  const int tile_w = width / grid_x;
  const int tile_h = height / grid_y;

  if (tile_w < 2 || tile_h < 2)
    {
      /* Fallback to simple contrast stretching for very small images */
      s730b_preproc_contrast_stretch (data, (gsize)width * height);
      return;
    }

  /* Allocate LUTs for each tile */
  guint8 (*luts)[256] = g_malloc (grid_x * grid_y * 256 * sizeof(guint8));

  /* Build LUT for each tile */
  for (int ty = 0; ty < grid_y; ty++)
    {
      for (int tx = 0; tx < grid_x; tx++)
        {
          const int x_start = tx * tile_w;
          const int y_start = ty * tile_h;
          const int x_end = (tx == grid_x - 1) ? width : x_start + tile_w;
          const int y_end = (ty == grid_y - 1) ? height : y_start + tile_h;
          const int tile_size = (x_end - x_start) * (y_end - y_start);

          /* Build histogram for this tile */
          guint hist[256] = {0};
          for (int y = y_start; y < y_end; y++)
            for (int x = x_start; x < x_end; x++)
              hist[data[y * width + x]]++;

          /* Clip histogram and redistribute */
          const guint clip_threshold = (guint)(clip_limit * tile_size / 256.0);
          guint excess = 0;
          for (int v = 0; v < 256; v++)
            {
              if (hist[v] > clip_threshold)
                {
                  excess += hist[v] - clip_threshold;
                  hist[v] = clip_threshold;
                }
            }
          /* Redistribute excess evenly */
          const guint incr = excess / 256;
          for (int v = 0; v < 256; v++)
            hist[v] += incr;

          /* Build CDF */
          guint cdf[256];
          cdf[0] = hist[0];
          for (int v = 1; v < 256; v++)
            cdf[v] = cdf[v-1] + hist[v];

          guint cdf_min = 0;
          for (int v = 0; v < 256; v++)
            if (cdf[v] > 0) { cdf_min = cdf[v]; break; }

          /* Build LUT */
          const int lut_idx = ty * grid_x + tx;
          const double denom = (double)(tile_size - cdf_min);
          for (int v = 0; v < 256; v++)
            {
              if (denom <= 0)
                luts[lut_idx][v] = (guint8)v;
              else
                {
                  double val = ((double)(cdf[v] - cdf_min) / denom) * 255.0;
                  luts[lut_idx][v] = (guint8)(val < 0 ? 0 : (val > 255 ? 255 : val));
                }
            }
        }
    }

  /* Apply LUTs with bilinear interpolation */
  guint8 *result = g_malloc (width * height);

  for (int y = 0; y < height; y++)
    {
      for (int x = 0; x < width; x++)
        {
          /* Find which tiles this pixel belongs to */
          const double fx = (double)x / tile_w - 0.5;
          const double fy = (double)y / tile_h - 0.5;

          int tx1 = (int)floor(fx);
          int ty1 = (int)floor(fy);
          int tx2 = tx1 + 1;
          int ty2 = ty1 + 1;

          /* Clamp to valid tile indices */
          tx1 = CLAMP(tx1, 0, grid_x - 1);
          ty1 = CLAMP(ty1, 0, grid_y - 1);
          tx2 = CLAMP(tx2, 0, grid_x - 1);
          ty2 = CLAMP(ty2, 0, grid_y - 1);

          /* Interpolation weights */
          double ax = fx - floor(fx);
          double ay = fy - floor(fy);
          if (tx1 == tx2) ax = 0;
          if (ty1 == ty2) ay = 0;

          const guint8 v = data[y * width + x];
          const double v11 = luts[ty1 * grid_x + tx1][v];
          const double v12 = luts[ty1 * grid_x + tx2][v];
          const double v21 = luts[ty2 * grid_x + tx1][v];
          const double v22 = luts[ty2 * grid_x + tx2][v];

          const double top = v11 * (1 - ax) + v12 * ax;
          const double bot = v21 * (1 - ax) + v22 * ax;
          const double val = top * (1 - ay) + bot * ay;

          result[y * width + x] = (guint8)(val < 0 ? 0 : (val > 255 ? 255 : val));
        }
    }

  memcpy (data, result, width * height);
  g_free (result);
  g_free (luts);
}

/* Detection parameters
 * For lock screens and PAM authentication, we need to wait indefinitely
 * for finger placement. Use G_MAXUINT for effectively infinite waiting.
 * Cancellation is handled by checking g_cancellable_is_cancelled() in each loop.
 */
#define DETECT_MAX_LOOPS         G_MAXUINT
#define DETECT_PER_LOOP          5
#define DETECT_PACKETS           6
#define DETECT_INTERVAL_MS       250
#define DETECT_INIT_TIMEOUT_MS   500
#define DETECT_BULK_TIMEOUT_MS   700

/* Capture parameters (Vendor Protocol) */
#define CAPTURE_START_IDX  0x032a
#define CAPTURE_CTRL_REQ   0xCA
#define CAPTURE_CTRL_VAL   0x0003

G_DECLARE_FINAL_TYPE (FpiDeviceSamsung730b,
                      fpi_device_samsung730b,
                      FPI, DEVICE_SAMSUNG730B,
                      FpImageDevice);

struct _FpiDeviceSamsung730b
{
  FpImageDevice parent;

  GCancellable *cancellable;      /* Used to cancel async operations on deactivate */
  gboolean      pending_retry;    /* Set when we request a scan retry due to quality */
  guint         scan_seq_counter; /* Monotonic counter for scan sequence logging */
};

G_DEFINE_TYPE (FpiDeviceSamsung730b,
               fpi_device_samsung730b,
               FP_TYPE_IMAGE_DEVICE);

/* ===================== Initialization Sequence =====================
 *
 * The sensor requires a specific initialization sequence before each
 * capture or detection operation. This sequence was reverse-engineered
 * from USB traffic captures on Windows.
 *
 * Command format:
 *   0xa9 xx yy zz - Write register xx with value yyzz
 *   0xa8 xx yy    - Read/configure register xx
 *   0x4f xx       - Mode/control command
 *
 * The exact meaning of most registers is unknown, but this sequence
 * reliably prepares the sensor for image capture.
 */

typedef struct
{
  const guint8 *data;
  gsize         len;
} SamsungCmd;

#define CMD(...) (const guint8[]){ __VA_ARGS__ }, sizeof((const guint8[]){ __VA_ARGS__ })

/* Sensor initialization commands (derived from USB traffic analysis) */
static const SamsungCmd init_cmds[] = {
  { CMD(0x4f, 0x80) },
  { CMD(0xa9, 0x4f, 0x80) },
  { CMD(0xa8, 0xb9, 0x00) },
  { CMD(0xa9, 0x60, 0x1b, 0x00) },
  { CMD(0xa9, 0x50, 0x21, 0x00) },
  { CMD(0xa9, 0x61, 0x00, 0x00) },
  { CMD(0xa9, 0x62, 0x00, 0x1a) },
  { CMD(0xa9, 0x63, 0x00, 0x1a) },
  { CMD(0xa9, 0x64, 0x04, 0x0a) },
  { CMD(0xa9, 0x66, 0x0f, 0x80) },
  { CMD(0xa9, 0x67, 0x1b, 0x00) },
  { CMD(0xa9, 0x68, 0x00, 0x0f) },
  { CMD(0xa9, 0x69, 0x00, 0x14) },
  { CMD(0xa9, 0x6a, 0x00, 0x19) },
  { CMD(0xa9, 0x6c, 0x00, 0x19) },
  { CMD(0xa9, 0x40, 0x43, 0x00) },
  { CMD(0xa9, 0x41, 0x6f, 0x00) },
  { CMD(0xa9, 0x55, 0x20, 0x00) },
  { CMD(0xa9, 0x5f, 0x00, 0x00) },
  { CMD(0xa9, 0x52, 0x27, 0x00) },
  { CMD(0xa9, 0x09, 0x00, 0x00) },
  { CMD(0xa9, 0x5d, 0x4d, 0x00) },
  { CMD(0xa9, 0x51, 0xa8, 0x25) },
  { CMD(0xa9, 0x03, 0x00) },
  { CMD(0xa9, 0x38, 0x01, 0x00) },
  { CMD(0xa9, 0x3d, 0xff, 0x0f) },
  { CMD(0xa9, 0x10, 0x60, 0x00) },
  { CMD(0xa9, 0x3b, 0x14, 0x00) },
  { CMD(0xa9, 0x2f, 0xf6, 0xff) },
  { CMD(0xa9, 0x09, 0x00, 0x00) },
  { CMD(0xa9, 0x0c, 0x00) },
  { CMD(0xa8, 0x20, 0x00, 0x00) },
  { CMD(0xa9, 0x04, 0x00) },
  { CMD(0xa8, 0x08, 0x00) },
  { CMD(0xa9, 0x09, 0x00, 0x00) },
  { CMD(0xa8, 0x3e, 0x00, 0x00) },
  { CMD(0xa9, 0x03, 0x00, 0x00) },
  { CMD(0xa8, 0x20, 0x00, 0x00) },
  { CMD(0xa9, 0x10, 0x00, 0x01) },
  { CMD(0xa9, 0x2f, 0xef, 0x00) },
  { CMD(0xa9, 0x09, 0x00, 0x00) },
  { CMD(0xa9, 0x5d, 0x4d, 0x00) },
  { CMD(0xa9, 0x51, 0x3a, 0x25) },
  { CMD(0xa9, 0x0c, 0x00) },
  { CMD(0xa8, 0x20, 0x00, 0x00) },
  { CMD(0xa9, 0x04, 0x00, 0x00) },
  { CMD(0xa9, 0x09, 0x00, 0x00) },
};

/*
 * Sequence of wIndex values used to request image chunks from the sensor.
 * The driver iterates through these indices to read the full image frame
 * in 256-byte blocks. The values were derived from USB traffic analysis.
 */
static const guint16 capture_indices[] = {
  0x032a, 0x042a, 0x052a, 0x062a, 0x072a, 0x082a, 0x092a, 0x0a2a,
  0x0b2a, 0x0c2a, 0x0d2a, 0x0e2a, 0x0f2a, 0x102a, 0x112a, 0x122a,
  0x132a, 0x142a, 0x152a, 0x162a, 0x172a, 0x182a, 0x192a, 0x1a2a,
  0x1b2a, 0x1c2a, 0x1d2a, 0x1e2a, 0x1f2a, 0x202a, 0x212a, 0x222a,
  0x232a, 0x242a, 0x252a, 0x262a, 0x272a, 0x282a, 0x292a, 0x2a2a,
  0x2b2a, 0x2c2a, 0x2d2a, 0x2e2a, 0x2f2a, 0x302a, 0x312a, 0x322a,
  0x332a, 0x342a, 0x352a, 0x362a, 0x372a, 0x382a, 0x392a, 0x3a2a,
  0x3b2a, 0x3c2a, 0x3d2a, 0x3e2a, 0x3f2a, 0x402a, 0x412a, 0x422a,
  0x432a, 0x442a, 0x452a, 0x462a, 0x472a, 0x482a, 0x492a, 0x4a2a,
  0x4b2a, 0x4c2a, 0x4d2a, 0x4e2a, 0x4f2a, 0x502a, 0x512a, 0x522a,
  0x532a, 0x542a, 0x552a, 0x562a, 0x572a,
};
#define CAPTURE_NUM_PACKETS (G_N_ELEMENTS (capture_indices))

/* ========================= USB Helpers =========================
 *
 * Thin wrappers around GUsb functions for cleaner code.
 * These handle the common patterns of vendor control and bulk transfers.
 */

/* Send vendor control request (host to device) */
static gboolean
usb_ctrl_out (GUsbDevice *usb_dev, guint8 req, guint16 val, guint16 idx,
              const guint8 *data, gsize len, guint timeout_ms, GError **error)
{
  gsize actual = 0;
  return g_usb_device_control_transfer (usb_dev,
                                        G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                        G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                        G_USB_DEVICE_RECIPIENT_DEVICE,
                                        req, val, idx, (guchar *) data, len,
                                        &actual, timeout_ms, NULL, error);
}

/* Bulk transfer out (host to device) */
static gboolean
usb_bulk_out (GUsbDevice *usb_dev, guint8 ep, const guint8 *data, gsize len,
              guint timeout_ms, GError **error)
{
  gsize actual = 0;
  return g_usb_device_bulk_transfer (usb_dev, ep, (guchar *) data, len,
                                     &actual, timeout_ms, NULL, error);
}

/* Bulk transfer in (device to host) */
static gboolean
usb_bulk_in (GUsbDevice *usb_dev, guint8 ep, guint8 *buf, gsize len,
             guint timeout_ms, gsize *actual_out, GError **error)
{
  return g_usb_device_bulk_transfer (usb_dev, ep, buf, len,
                                     actual_out, timeout_ms, NULL, error);
}

/* ======================== Device Logic ======================== */

/*
 * Initialize the sensor before capture/detection.
 *
 * This sends a vendor control request (0xC3) followed by the full
 * initialization command sequence. Must be called before each capture
 * operation to ensure the sensor is in a known good state.
 */
static gboolean
s730b_init (GUsbDevice *usb_dev, GError **error)
{
  /* Vendor control data - purpose unknown, but required for sensor reset */
  guint8 c3_data[16] = {
    0x80, 0x84, 0x1e, 0x00, 0x08, 0x00, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
  };
  gsize i;

  if (!usb_ctrl_out (usb_dev, 0xC3, 0x0000, 0x0000, c3_data, sizeof (c3_data), 500, error))
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (init_cmds); i++)
    {
      if (!usb_bulk_out (usb_dev, SAMSUNG730B_EP_OUT, init_cmds[i].data, init_cmds[i].len, 500, error))
        return FALSE;
    }
  return TRUE;
}

/*
 * Read a small amount of image data to detect finger presence.
 *
 * This performs a partial capture (DETECT_PACKETS chunks) which is faster
 * than a full capture but sufficient to determine if a finger is on the sensor.
 * The returned buffer can be analyzed with s730b_has_fingerprint().
 *
 * Returns: Allocated buffer with partial image data (caller must free), or NULL on error.
 */
static guint8 *
s730b_detect_finger (GUsbDevice *usb_dev, gsize *out_len, GError **error)
{
  guint8 *buf = NULL;
  gsize capacity = DETECT_PACKETS * BULK_PACKET_SIZE + 64;
  gsize total = 0;
  guint8 start_cmd[BULK_PACKET_SIZE] = { 0 };
  guint i;

  buf = g_malloc0 (capacity);
  if (!buf)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "alloc fail");
      return NULL;
    }

  /* packet 0 */
  if (!usb_ctrl_out (usb_dev, CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL, CAPTURE_START_IDX, NULL, 0, DETECT_INIT_TIMEOUT_MS, error))
    goto fail;

  start_cmd[0] = 0xa8;
  start_cmd[1] = 0x06;
  if (!usb_bulk_out (usb_dev, SAMSUNG730B_EP_OUT, start_cmd, sizeof (start_cmd), DETECT_INIT_TIMEOUT_MS, error))
    goto fail;

  {
    guint8 tmp[BULK_PACKET_SIZE];
    gsize got = 0;
    if (!usb_bulk_in (usb_dev, SAMSUNG730B_EP_IN, tmp, sizeof (tmp), DETECT_INIT_TIMEOUT_MS, &got, error))
      goto fail;
    if (got > 0)
      {
        memcpy (buf + total, tmp, got);
        total += got;
      }
  }

  /* packet 1..N: Read remaining chunks */
  for (i = 1; i < DETECT_PACKETS; i++)
    {
      guint8 tmp[BULK_PACKET_SIZE];
      guint8 ack[BULK_PACKET_SIZE] = { 0 };
      gsize got = 0;
      guint16 widx = CAPTURE_START_IDX + i * BULK_PACKET_SIZE;

      if (!usb_ctrl_out (usb_dev, CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL, widx, NULL, 0, DETECT_INIT_TIMEOUT_MS, error))
        goto fail;
      if (!usb_bulk_in (usb_dev, SAMSUNG730B_EP_IN, tmp, sizeof (tmp), DETECT_BULK_TIMEOUT_MS, &got, error))
        goto fail;
      if (got > 0)
        {
          memcpy (buf + total, tmp, got);
          total += got;
        }
      if (!usb_bulk_out (usb_dev, SAMSUNG730B_EP_OUT, ack, sizeof (ack), DETECT_INIT_TIMEOUT_MS, error))
        goto fail;
    }

  *out_len = total;
  return buf;

fail:
  g_free (buf);
  *out_len = 0;
  return NULL;
}

/*
 * Detect if a finger is present on the sensor.
 *
 * This is a heuristic based on pixel value distribution: when a finger
 * is pressed, the sensor outputs significant 0xFF pixels (light ridges
 * due to inverted polarity). An empty sensor reads mostly dark/mid values.
 *
 * Returns TRUE if finger presence is detected.
 */
static gboolean
s730b_has_fingerprint (const guint8 *data, gsize len)
{
  if (!data || len < 512)
    return FALSE;

  gsize total = MIN (len, (gsize) 4096);
  gsize zeros = 0, ff = 0;

  for (gsize i = 0; i < total; i++)
    {
      guint8 v = data[i];
      if (v == 0x00) zeros++;
      else if (v == 0xFF) ff++;
    }

  double ff_ratio = (double) ff / (double) total;
  /* Trigger a little earlier to avoid capturing only over-pressed
   * (highly saturated) frames.
   */
  if (ff_ratio > 0.20 && zeros < total * 0.95)
    return TRUE;

  return FALSE;
}

static gboolean s730b_wait_finger_lost (GUsbDevice *usb_dev, GCancellable *cancellable);

static gboolean
s730b_finger_present (GUsbDevice *usb_dev, GCancellable *cancellable)
{
  g_autoptr(GError) local_error = NULL;
  gsize finger_len = 0;
  guint8 *finger = NULL;
  gboolean present = FALSE;

  if (g_cancellable_is_cancelled (cancellable))
    return FALSE;

  if (!s730b_init (usb_dev, &local_error))
    return FALSE;

  finger = s730b_detect_finger (usb_dev, &finger_len, &local_error);
  if (!finger)
    return FALSE;

  present = s730b_has_fingerprint (finger, finger_len);
  g_free (finger);
  return present;
}

static gboolean
s730b_wait_finger_lost_timeout (GUsbDevice *usb_dev,
                                GCancellable *cancellable,
                                guint timeout_ms)
{
  const gint64 deadline_us = g_get_monotonic_time () + ((gint64) timeout_ms * 1000);
  const guint required_consecutive = 3;
  guint consecutive_absent = 0;

  g_message ("s730b: waiting for fingerprint removal (timeout=%ums)...", timeout_ms);

  while (!g_cancellable_is_cancelled (cancellable))
    {
      if (g_get_monotonic_time () >= deadline_us)
        return FALSE;

      g_autoptr(GError) local_error = NULL;
      gsize finger_len = 0;
      guint8 *finger = NULL;

      if (!s730b_init (usb_dev, &local_error))
        {
          g_usleep (150 * 1000);
          continue;
        }

      finger = s730b_detect_finger (usb_dev, &finger_len, &local_error);
      if (!finger)
        {
          g_usleep (150 * 1000);
          continue;
        }

      gboolean fingerprint = s730b_has_fingerprint (finger, finger_len);
      g_free (finger);
      if (!fingerprint)
        {
          consecutive_absent++;
          if (consecutive_absent >= required_consecutive)
            return TRUE;
          g_usleep (100 * 1000);
          continue;
        }

      consecutive_absent = 0;

      g_usleep (150 * 1000);
    }

  return FALSE;
}

/* Wait for finger ON */
static gboolean
s730b_wait_finger (GUsbDevice *usb_dev, GCancellable *cancellable)
{
  GError *local_error = NULL;

  for (guint r = 0; r < DETECT_MAX_LOOPS; r++)
    {
      if (g_cancellable_is_cancelled (cancellable)) return FALSE;
      g_message ("s730b: wait_finger loop %u/%u", r + 1, DETECT_MAX_LOOPS);

      for (guint i = 0; i < DETECT_PER_LOOP; i++)
        {
          if (g_cancellable_is_cancelled (cancellable)) return FALSE;

          local_error = NULL;
          if (!s730b_init (usb_dev, &local_error))
            {
              g_clear_error (&local_error);
              g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
              continue;
            }

          gsize figner_len = 0;
          guint8 *finger = s730b_detect_finger (usb_dev, &figner_len, &local_error);
          if (!finger)
            {
              g_clear_error (&local_error);
              g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
              continue;
            }

          gboolean fingerprint = s730b_has_fingerprint (finger, figner_len);
          g_free (finger);

          if (fingerprint)
            {
              g_message ("s730b: fingerprint detected!");
              /*
               * Some devices report a brief false-positive at the moment of
               * touch/transition. Add a short settle delay and re-check.
               */
              g_usleep (20 * 1000);

              local_error = NULL;
              if (!s730b_init (usb_dev, &local_error))
                {
                  g_clear_error (&local_error);
                  g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
                  continue;
                }

              gsize confirm_len = 0;
              guint8 *confirm = s730b_detect_finger (usb_dev, &confirm_len, &local_error);
              if (!confirm)
                {
                  g_clear_error (&local_error);
                  g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
                  continue;
                }

              gboolean confirmed = s730b_has_fingerprint (confirm, confirm_len);
              g_free (confirm);
              if (!confirmed)
                {
                  g_message ("s730b: fingerprint not confirmed after settle; retrying");
                  g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
                  continue;
                }

              /* Re-init required before full capture */
              local_error = NULL;
              if (!s730b_init (usb_dev, &local_error))
                {
                  g_clear_error (&local_error);
                  g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
                  continue;
                }
              return TRUE;
            }
          g_usleep ((gulong) DETECT_INTERVAL_MS * 1000);
        }
      if (!s730b_init (usb_dev, &local_error)) g_clear_error (&local_error);
    }
  return FALSE;
}

/* Wait for finger OFF */
static gboolean
s730b_wait_finger_lost (GUsbDevice *usb_dev, GCancellable *cancellable)
{
  GError *local_error = NULL;
  const guint required_consecutive = 3;
  guint consecutive_absent = 0;

  g_message ("s730b: waiting for fingerprint removal (indefinite)...");

  while (!g_cancellable_is_cancelled (cancellable))
    {
      local_error = NULL;
      if (!s730b_init (usb_dev, &local_error))
        {
           g_clear_error (&local_error);
           g_usleep (150 * 1000);
           continue;
        }

      gsize figner_len = 0;
      guint8 *finger = s730b_detect_finger (usb_dev, &figner_len, &local_error);
      if (!finger)
        {
          g_clear_error (&local_error);
          g_usleep (150 * 1000);
          continue;
        }

      gboolean fingerprint = s730b_has_fingerprint (finger, figner_len);
      g_free (finger);

      if (!fingerprint)
        {
          consecutive_absent++;
          if (consecutive_absent >= required_consecutive)
            return TRUE;
          g_usleep (100 * 1000);
          continue;
        }

      consecutive_absent = 0;
      
      g_usleep (150 * 1000);
    }

  return FALSE;
}

/* Capture fingerprint image */
static gboolean
s730b_capture (GUsbDevice *usb_dev, guint8 **out_buf, gsize *out_len, GError **error)
{
  guint8 *buf = NULL;
  gsize capacity = (gsize) CAPTURE_NUM_PACKETS * BULK_PACKET_SIZE + 1024;
  gsize total = 0;

  buf = g_malloc (capacity);
  if (!buf)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "alloc fail");
      return FALSE;
    }

  /* packet 0 */
  {
    guint8 start_cmd[BULK_PACKET_SIZE] = { 0 };
    start_cmd[0] = 0xa8;
    start_cmd[1] = 0x06;

    if (!usb_ctrl_out (usb_dev, CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL, capture_indices[0], NULL, 0, 500, error)) goto fail;
    if (!usb_bulk_out (usb_dev, SAMSUNG730B_EP_OUT, start_cmd, sizeof (start_cmd), 500, error)) goto fail;
    guint8 tmp[BULK_PACKET_SIZE];
    gsize got = 0;
    if (!usb_bulk_in (usb_dev, SAMSUNG730B_EP_IN, tmp, sizeof (tmp), 500, &got, error)) goto fail;
  }

  /* packet 1..N */
  for (gsize i = 1; i < CAPTURE_NUM_PACKETS; i++)
    {
      guint8 tmp[BULK_PACKET_SIZE];
      guint8 ack[BULK_PACKET_SIZE] = { 0 };
      gsize got = 0;
      guint16 widx = capture_indices[i];

      if (!usb_ctrl_out (usb_dev, CAPTURE_CTRL_REQ, CAPTURE_CTRL_VAL, widx, NULL, 0, 500, error)) goto fail;
      if (!usb_bulk_in (usb_dev, SAMSUNG730B_EP_IN, tmp, sizeof (tmp), 1000, &got, error)) goto fail;
      
      if (got == 0) break;
      if (total + got > capacity)
        {
          capacity *= 2;
          guint8 *tmp_buf = g_realloc (buf, capacity);
          if (!tmp_buf) { g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "realloc fail"); goto fail; }
          buf = tmp_buf;
        }
      memcpy (buf + total, tmp, got);
      total += got;
      if (!usb_bulk_out (usb_dev, SAMSUNG730B_EP_OUT, ack, sizeof (ack), 500, error)) goto fail;
    }

  *out_buf = buf;
  *out_len = total;
  return TRUE;

fail:
  g_free (buf);
  *out_buf = NULL;
  *out_len = 0;
  return FALSE;
}

/* Process and submit the image to libfprint */
static const char *
s730b_action_name (FpiDeviceAction action)
{
  switch (action)
    {
    case FPI_DEVICE_ACTION_NONE:
      return "NONE";
    case FPI_DEVICE_ACTION_PROBE:
      return "PROBE";
    case FPI_DEVICE_ACTION_OPEN:
      return "OPEN";
    case FPI_DEVICE_ACTION_CLOSE:
      return "CLOSE";
    case FPI_DEVICE_ACTION_LIST:
      return "LIST";
    case FPI_DEVICE_ACTION_DELETE:
      return "DELETE";
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      return "CLEAR_STORAGE";
    case FPI_DEVICE_ACTION_ENROLL:
      return "ENROLL";
    case FPI_DEVICE_ACTION_VERIFY:
      return "VERIFY";
    case FPI_DEVICE_ACTION_IDENTIFY:
      return "IDENTIFY";
    case FPI_DEVICE_ACTION_CAPTURE:
      return "CAPTURE";
    }

  return "UNKNOWN";
}

static void
s730b_submit_image (FpImageDevice *dev,
                    guint8        *raw,
                    gsize          raw_len,
                    guint          scan_seq,
                    FpiDeviceAction action)
{
  gsize needed = IMG_OFFSET + (gsize) IMG_W * IMG_H;
  if (raw_len < needed)
    {
      g_warning ("s730b: scan=%u %s captured too short", scan_seq, s730b_action_name (action));
      g_free (raw);
      fpi_image_device_report_finger_status (dev, FALSE);
      fpi_image_device_image_captured (dev, NULL);
      return;
    }

  /* We have a captured frame that corresponds to a finger-on condition.
   * Report this early so that any retry/error path below keeps the
   * image-device state machine consistent.
   */
  fpi_image_device_report_finger_status (dev, TRUE);

  /* Basic ROI statistics to correlate with NBIS minutiae success. */
  {
    S730bRoiStats stats;
    const gboolean have_stats = s730b_calc_roi_stats (raw, raw_len, &stats);
    const double mean = have_stats ? stats.mean : 0.0;
    const double stddev = have_stats ? stats.stddev : 0.0;
    const double sat0_pct = have_stats ? stats.sat0_pct : 0.0;
    const double sat255_pct = have_stats ? stats.sat255_pct : 0.0;

    /*
     * Heuristic: reduce brightness if a large fraction of the ROI is clipped
     * at 0xFF. This doesn't restore lost detail, but it avoids feeding NBIS a
     * frame dominated by saturated highlights.
     */
    if (have_stats && (sat255_pct > 70.0 || stddev < 15.0))
      {
        g_warning ("s730b: scan=%u %s rejecting unusable frame mean=%.1f std=%.1f sat0=%.1f%% sat255=%.1f%%",
                   scan_seq, s730b_action_name (action), mean, stddev, sat0_pct, sat255_pct);
        g_free (raw);
        fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_GENERAL);
        return;
      }

    g_message ("s730b: scan=%u %s roi stats mean=%.1f std=%.1f grad=%.1f sat0=%.1f%% sat255=%.1f%%",
               scan_seq, s730b_action_name (action), mean, stddev,
               have_stats ? stats.grad_mean : 0.0, sat0_pct, sat255_pct);
  }

  /*
   * NOTE: NBIS minutiae detection can be unreliable on very small images.
   * Other libfprint drivers upscale frames before handing them off.
   */
  FpImage *tmp = fp_image_new (IMG_W, IMG_H);

  /* The sensor output is inverted (light ridges/dark valleys). Mark this so
   * downstream processing sees the expected polarity.
   */
  tmp->flags = FPI_IMAGE_COLORS_INVERTED;

  /*
   * Set pixels-per-mm (ppmm) for NBIS minutiae conversion and bozorth matching.
   * This value represents the sensor's physical resolution.
   * Formula: 500 DPI / 25.4 mm/inch = 19.69 px/mm
   *
   * After 2x upscaling, ppmm is also doubled to maintain correct physical units.
   */
  tmp->ppmm = 19.69;

  memcpy (tmp->data, raw + IMG_OFFSET, IMG_W * IMG_H);
  g_free (raw);

  /* Image preprocessing pipeline:
   * 1. CLAHE for adaptive contrast enhancement
   * 2. Contrast stretching to ensure full dynamic range
   * 3. Unsharp mask for edge sharpening
   *
   * NOTE: The sensor outputs light ridges on dark background (inverted).
   * The FPI_IMAGE_COLORS_INVERTED flag tells NBIS to handle this.
   */
  {
    /*
     * CLAHE clip_limit=3.0: experiment with 2.0 collapsed genuine scores
     * into the teens (TAR ~40%). Keep 3.0 for usable minutiae density.
     */
    s730b_preproc_clahe (tmp->data, tmp->width, tmp->height, 3.0);

    /* Additional contrast stretching to ensure full dynamic range */
    s730b_preproc_contrast_stretch (tmp->data, (gsize) tmp->width * tmp->height);

    /*
     * unsharp 1.5: better TAR than 1.0; still milder than original 2.5
     * which inflated FAR. Pair with thr~40 to reject residual impostors.
     */
    s730b_preproc_unsharp_mask (tmp->data, tmp->width, tmp->height, 1.5);
  }

  const int resize_factor = S730B_RESIZE_FACTOR;
  const double ppmm_out = tmp->ppmm * (double) resize_factor;

  const int out_w = tmp->width * resize_factor;
  const int out_h = tmp->height * resize_factor;

  g_message ("s730b: scan=%u %s image %dx%d -> %dx%d, ppmm=%.2f->%.2f",
             scan_seq, s730b_action_name (action), IMG_W, IMG_H,
             out_w, out_h, tmp->ppmm, ppmm_out);

  /* Upscale to help NBIS processing on this very small sensor image.
   * NBIS/minutiae conversion uses ppmm, so adjust it to keep physical units
   * consistent after resizing unless explicitly disabled.
   */
  FpImage *img = tmp;
  if (resize_factor != 1)
    {
      img = fpi_image_resize (tmp, resize_factor, resize_factor);
      g_object_unref (tmp);
    }
  img->ppmm = ppmm_out;

  fpi_image_device_image_captured (dev, img);
}

/* ==================== Asynchronous Task Management ====================
 *
 * The sensor requires blocking USB operations for finger detection and
 * image capture. These are performed in a worker thread using GTask to
 * avoid blocking the main GLib event loop.
 *
 * Two task modes:
 *   TASK_MODE_ENROLL   - Wait for finger, capture image, submit to libfprint
 *   TASK_MODE_WAIT_OFF - Wait for finger to be removed from sensor
 */

typedef enum {
  TASK_MODE_ENROLL,     /* Capture a fingerprint image */
  TASK_MODE_WAIT_OFF    /* Wait for finger removal */
} TaskMode;

typedef struct {
  GUsbDevice     *usb_dev;
  guint8         *raw_data;       /* Captured image data */
  gsize           raw_len;
  TaskMode        mode;
  FpiDeviceAction action;         /* Current device action (ENROLL/VERIFY/etc) */
  gboolean        finger_lifted;  /* Set when finger removal is detected */
  guint           scan_seq;       /* Sequence number for log correlation */
} S730bTaskData;

static void
s730b_task_data_free (gpointer user_data)
{
  S730bTaskData *data = user_data;
  if (data->raw_data) g_free (data->raw_data);
  g_free (data);
}

/* Forward declarations to Asynchronous Task for Compiler */
static void s730b_enroll_done (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void s730b_enroll_worker (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);

static void
s730b_run_async_task (FpImageDevice *dev, TaskMode mode)
{
  FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B (dev);
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));
  GTask *task;
  S730bTaskData *data;

  task = g_task_new (dev, self->cancellable, s730b_enroll_done, NULL);
  data = g_new0 (S730bTaskData, 1);
  data->usb_dev = usb;
  data->mode = mode;
  data->action = fpi_device_get_current_action (FP_DEVICE (dev));
  if (mode == TASK_MODE_ENROLL)
    data->scan_seq = ++self->scan_seq_counter;
  else
    data->scan_seq = self->scan_seq_counter;

  g_task_set_task_data (task, data, s730b_task_data_free);
  g_task_run_in_thread (task, s730b_enroll_worker);
  g_object_unref (task);
}

/*
 * Worker thread for blocking USB operations (s730b_wait_finger(), s730b_capture()).
 * Prevent the main GLib loop from stopping by using the thread
 */
static void
s730b_enroll_worker (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  S730bTaskData *data = task_data;

  if (g_task_return_error_if_cancelled (task)) return;

  if (data->mode == TASK_MODE_WAIT_OFF)
    {
      data->finger_lifted = FALSE;

      if (data->action == FPI_DEVICE_ACTION_ENROLL)
        {
          /* ENROLL must enforce a fresh press: wait indefinitely until finger is removed. */
          if (s730b_wait_finger_lost (data->usb_dev, cancellable))
            {
              data->finger_lifted = TRUE;
              g_task_return_boolean (task, TRUE);
            }
          else
            {
              if (g_task_return_error_if_cancelled (task))
                return;

              g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "Wait lost failed");
            }
          return;
        }

      /* VERIFY/IDENTIFY: never block indefinitely in WAIT_OFF.
       * fprintd may run an internal IDENTIFY before ENROLL; if we block here it will cancel.
       */
      /* Longer wait: short timeouts caused frequent
       * "try again after removing the finger" in rapid verify loops.
       */
      if (s730b_wait_finger_lost_timeout (data->usb_dev, cancellable, 2800))
        data->finger_lifted = TRUE;

      if (g_task_return_error_if_cancelled (task))
        return;

      /* Always complete WAIT_OFF successfully for non-enroll actions. */
      g_task_return_boolean (task, TRUE);
      return;
    }

  /* TASK_MODE_ENROLL */
    if (s730b_finger_present (data->usb_dev, cancellable))
      {
        g_message ("s730b: scan=%u %s finger already present; waiting for removal before scan",
                   data->scan_seq, s730b_action_name (data->action));

        if (data->action == FPI_DEVICE_ACTION_ENROLL)
          {
            if (!s730b_wait_finger_lost (data->usb_dev, cancellable))
              {
                if (g_task_return_error_if_cancelled (task))
                  return;

                g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "Wait lost failed");
                return;
              }
          }
        else
          {
            /* For VERIFY/IDENTIFY, do not block indefinitely waiting for removal.
             * This avoids stalling deactivation paths while still encouraging a fresh press.
             */
            if (!s730b_wait_finger_lost_timeout (data->usb_dev, cancellable, 3500))
              {
                g_task_return_error (task,
                                     fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                                               "Finger already on sensor; please remove it"));
                return;
              }
          }
      }

  if (!s730b_wait_finger (data->usb_dev, cancellable))
    {
      if (g_task_return_error_if_cancelled (task))
        return;

      g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "No fingerprint detected");
      return;
    }

  if (g_task_return_error_if_cancelled (task)) return;

  /* Report finger presence immediately so UI can show "scanning..." message
   * before the multi-frame capture begins.
   */
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (source_object), TRUE);

  /* Short delay for stabilization.
   * Keep this minimal to avoid drifting into an over-pressed (more saturated)
   * regime on this sensor.
   */
  g_usleep (5 * 1000);

  /* Capture a small burst and keep the best-looking frame.
   * This improves the chance of getting enough minutiae without extra user retries.
   */
  {
    guint8 *best_buf = NULL;
    gsize best_len = 0;
    double best_score = -G_MAXDOUBLE;
    S730bRoiStats best_stats = { 0 };
    gboolean have_best_stats = FALSE;
    int best_idx = -1;

    const int n_candidates = 9;
    for (int i = 0; i < n_candidates; i++)
      {
        guint8 *buf = NULL;
        gsize len = 0;
        g_autoptr(GError) cap_error = NULL;
        S730bRoiStats st;

        if (i > 0)
          g_usleep (10 * 1000);

        if (!s730b_init (data->usb_dev, &cap_error))
          {
            /* If we already have a candidate, keep going; otherwise fail. */
            if (best_buf)
              continue;

            g_task_return_error (task, g_steal_pointer (&cap_error));
            return;
          }

        if (!s730b_capture (data->usb_dev, &buf, &len, &cap_error))
          {
            /* If we already have a candidate, keep going; otherwise fail. */
            if (best_buf)
              continue;

            g_task_return_error (task, g_steal_pointer (&cap_error));
            return;
          }

        if (!s730b_calc_roi_stats (buf, len, &st))
          {
            g_free (buf);
            continue;
          }

        const double score = s730b_roi_score (&st);
        g_message ("s730b: scan=%u %s capture candidate %d/%d mean=%.1f std=%.1f grad=%.1f sat0=%.1f%% sat255=%.1f%% score=%.1f",
             data->scan_seq,
             s730b_action_name (data->action),
             i + 1,
                   n_candidates,
                   st.mean,
                   st.stddev,
                   st.grad_mean,
                   st.sat0_pct,
                   st.sat255_pct,
                   score);

        if (score > best_score)
          {
            g_free (best_buf);
            best_buf = buf;
            best_len = len;
            best_score = score;
            best_stats = st;
            have_best_stats = TRUE;
            best_idx = i + 1;
          }
        else
          {
            g_free (buf);
          }
      }

    if (have_best_stats)
      {
        g_message ("s730b: scan=%u %s selected candidate %d/%d mean=%.1f std=%.1f grad=%.1f sat0=%.1f%% sat255=%.1f%% score=%.1f",
                   data->scan_seq,
                   s730b_action_name (data->action),
                   best_idx,
                   n_candidates,
                   best_stats.mean,
                   best_stats.stddev,
                   best_stats.grad_mean,
                   best_stats.sat0_pct,
                   best_stats.sat255_pct,
                   best_score);
      }

    if (!best_buf)
      {
        g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "Capture failed");
        return;
      }

    data->raw_data = best_buf;
    data->raw_len = best_len;
  }

  g_task_return_boolean (task, TRUE);
}

static void
s730b_enroll_done (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (source_object);
  FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B (dev);
  GTask *task = G_TASK (res);
  S730bTaskData *data = g_task_get_task_data (task);
  GError *error = NULL;
  FpiDeviceAction action = fpi_device_get_current_action (FP_DEVICE (dev));

  g_message ("s730b: scan=%u %s task_done mode=%d task_action=%d current_action=%d",
             data->scan_seq, s730b_action_name (data->action), data->mode, data->action, action);

  if (g_task_propagate_boolean (task, &error))
    {
      if (data->mode == TASK_MODE_WAIT_OFF)
        {
          /* libfprint's image-device state machine expects an explicit
           * finger-off transition to finish actions. For non-enroll actions we
           * use a bounded WAIT_OFF; even if the finger is still on the sensor,
           * report finger-off here so the action can complete and fprintd can
           * continue (it performs an internal IDENTIFY before ENROLL).
           */
          fpi_image_device_report_finger_status (dev, FALSE);

          /* Keep looping in the ENROLL case. For non-enroll actions, only
           * loop again if we explicitly requested a retry due to image quality.
           */
          if (action == FPI_DEVICE_ACTION_ENROLL || self->pending_retry)
            {
              self->pending_retry = FALSE;
              s730b_run_async_task (dev, TASK_MODE_ENROLL);
            }
        }
      else
        {
          /* Capture done, submit image */
          guint8 *buf = data->raw_data;
          gsize len = data->raw_len;
          data->raw_data = NULL;

          /* If the ROI looks like it will produce too few minutiae, ask for a
           * retry (even in VERIFY). This avoids turning low-quality captures
           * into immediate verify-no-match results.
           */
          {
            S730bRoiStats stats;
            if (s730b_calc_roi_stats (buf, len, &stats) && s730b_roi_needs_retry_for_action (&stats, action))
              {
                g_warning ("s730b: scan=%u %s requesting retry due to quality mean=%.1f std=%.1f grad=%.1f sat0=%.1f%% sat255=%.1f%%",
                           data->scan_seq,
                           s730b_action_name (action),
                           stats.mean,
                           stats.stddev,
                           stats.grad_mean,
                           stats.sat0_pct,
                           stats.sat255_pct);
                g_free (buf);
                self->pending_retry = TRUE;
                fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_GENERAL);
                s730b_run_async_task (dev, TASK_MODE_WAIT_OFF);
                return;
              }
          }

          s730b_submit_image (dev, buf, len, data->scan_seq, action);

          /* Wait for finger removal after capture for all actions.
           * This keeps VERIFY consistent with ENROLL: users must lift the
           * finger between attempts. The wait is cancellable via
           * samsung730b_dev_deactivate().
           */
          s730b_run_async_task (dev, TASK_MODE_WAIT_OFF);
        }
    }
  else
    {
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          /* Retry errors must be reported via fpi_image_device_retry_scan(),
           * not as session errors (otherwise fprintd will surface them as
           * verify-unknown-error).
           */
          g_warning ("s730b: requesting retry: %s", error->message);
          self->pending_retry = TRUE;
          fpi_image_device_retry_scan (dev, error->code);
          g_clear_error (&error);
          s730b_run_async_task (dev, TASK_MODE_WAIT_OFF);
          return;
        }

      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("s730b: task failed: %s", error->message);

          /* For non-enroll operations, propagate the error instead of
           * restarting an internal loop.
           */
          if (action != FPI_DEVICE_ACTION_ENROLL)
            {
              fpi_image_device_session_error (dev, g_steal_pointer (&error));
              return;
            }

          if (data->mode == TASK_MODE_ENROLL)
            {
              fpi_image_device_report_finger_status (dev, FALSE);

              /* Retry detection on error */
              s730b_run_async_task (dev, TASK_MODE_ENROLL);
            }
        }
      g_error_free (error);
    }
}

/* ==================== libfprint Callbacks ====================
 *
 * Standard FpImageDevice interface implementation.
 * These functions are called by the libfprint core at various stages
 * of device operation.
 */

static void
samsung730b_dev_init (FpImageDevice *dev)
{
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;

  if (!g_usb_device_claim_interface (usb, 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }
  if (!s730b_init (usb, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }
  fpi_image_device_open_complete (dev, NULL);
}

static void
samsung730b_dev_deinit (FpImageDevice *dev)
{
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;

  g_usb_device_release_interface (usb, 0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
samsung730b_dev_activate (FpImageDevice *dev)
{
  FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B (dev);
  FpiImageDeviceState state;
  FpiDeviceAction action;

  if (self->cancellable) g_object_unref (self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  fpi_image_device_activate_complete (dev, NULL);

  action = fpi_device_get_current_action (FP_DEVICE (dev));

  if (action == FPI_DEVICE_ACTION_ENROLL && state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    s730b_run_async_task (dev, TASK_MODE_WAIT_OFF);
  else
    s730b_run_async_task (dev, TASK_MODE_ENROLL);
}

static void
samsung730b_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B (dev);
  
  if (self->cancellable) 
    g_cancellable_cancel (self->cancellable);
    
  fpi_image_device_deactivate_complete (dev, NULL);
}

/* ================= Device ID Table & GObject Boilerplate ================= */

static const FpIdEntry samsung730b_ids[] = {
  { .vid = SAMSUNG730B_VID, .pid = SAMSUNG730B_PID, .driver_data = 0 },
  { .vid = 0, .pid = 0, .driver_data = 0 },  /* sentinel */
};

static void
fpi_device_samsung730b_init (FpiDeviceSamsung730b *self)
{
  self->cancellable = NULL;
  self->pending_retry = FALSE;
}

static void
fpi_device_samsung730b_finalize (GObject *object)
{
  FpiDeviceSamsung730b *self = FPI_DEVICE_SAMSUNG730B (object);
  if (self->cancellable)
    {
      g_cancellable_cancel (self->cancellable);
      g_object_unref (self->cancellable);
    }
  G_OBJECT_CLASS (fpi_device_samsung730b_parent_class)->finalize (object);
}

static void
fpi_device_samsung730b_class_init (FpiDeviceSamsung730bClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  dev_class->id = "samsung730b";
  dev_class->full_name = "Samsung 730B";
  dev_class->id_table = samsung730b_ids;
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  /* This driver can require many retries to get a usable
   * (computable) NBIS/bozorth template. Use a slower warm-up/cool-down model
   * than the global defaults to avoid spurious FP_DEVICE_ERROR_TOO_HOT.
   */
  dev_class->temp_hot_seconds = 15 * 60;
  dev_class->temp_cold_seconds = 45 * 60;

  img_class->img_open = samsung730b_dev_init;
  img_class->img_close = samsung730b_dev_deinit;
  img_class->activate = samsung730b_dev_activate;
  img_class->deactivate = samsung730b_dev_deactivate;

  img_class->img_width = IMG_W;
  img_class->img_height = IMG_H;

  /*
   * bz3_threshold is the minimum bozorth3 match score required to consider
   * two fingerprints as matching. Lower values increase FAR (False Accept Rate)
   * but reduce FRR (False Reject Rate).
   *
   * Tuning (2026-08-01), CLAHE 3.0 + unsharp 1.5:
   *   thr=25 → FAR ~40%
   *   thr=32 → TAR 10/10, FAR 1/5 (one fluke impostor ~40)
   *   thr=41 → FAR 0/5 but TAR 7/10 (genuine 26–33 dropped)
   * Keep thr=32 as best TAR/FAR tradeoff; rely on milder sharpening
   * and longer finger-off waits for stability.
   */
  img_class->bz3_threshold = 32;

  gobject_class->finalize = fpi_device_samsung730b_finalize;
}