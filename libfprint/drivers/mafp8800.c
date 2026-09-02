/*
 * Microarray MAFP8800 driver for libfprint
 * Copyright (C) 2026 Mark Shank
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
 * This driver subclasses FpDevice directly instead of FpImageDevice because
 * the sensor's 36x160 pixel image at ~200 DPI is far too low-resolution for
 * libfprint's NBIS minutiae extraction (mindtct typically finds 0-5 minutiae,
 * insufficient for bozorth3 matching). Instead, the driver implements its own
 * SIFT-WHT binary descriptor matching pipeline tuned for this sensor geometry.
 *
 * For detailed documentation (protocol, matching algorithm, project history):
 * https://github.com/IngeniousIdiocy/mafp8800-fingerprint-driver
 */

#define FP_COMPONENT "mafp8800"

#include "drivers_api.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/* Image geometry: 160 rows x 37 pixels, 74 bytes/row (2 hdr + 72 pixel) */
#define MAFP_ROWS 160
#define MAFP_COLS 37
#define MAFP_ROW_BYTES 74              /* 0x4A */
#define MAFP_PIXELS (MAFP_ROWS * MAFP_COLS)            /* 5920 */
#define MAFP_FRAME_BYTES (320 * MAFP_ROW_BYTES)        /* 0x5C80 = 23680 */
#define MAFP_ENHANCED_COLS 36          /* column 0 stripped */
#define MAFP_ENHANCED_PIXELS (MAFP_ROWS * MAFP_ENHANCED_COLS)   /* 5760 */

/* SPI */
#define MAFP_SPI_SPEED 4000000
#define MAFP_RAW_READ_SZ 20480         /* 0x5000 */

/* Chip ID */
#define MAFP_CHIPID_FP36 0x24

/* Detection thresholds */
#define MAFP_DETECT_PX_THRESH 320      /* 0x140: per-pixel delta for "changed" */
#define MAFP_DETECT_RAW_LIMIT 178559   /* 0x2B97F: count*100 must exceed this */
#define MAFP_STABLE_SAD_LIMIT 114687   /* 0x1C1FF: sum-of-abs-diffs for "stable" */

/* Enrollment */
#define MAFP_ENROLL_STAGES 8

/* Gaussian pyramid: 5 levels (original + 4 blurs), 4 DoG layers */
#define MAFP_PYR_LEVELS 5
#define MAFP_DOG_LEVELS 4

/* Template geometry: 2 banks × 50 keypoints × 20 bytes + headers */
#define MAFP_MAX_KP 50               /* max keypoints per bank */
#define MAFP_NUM_BANKS 2
#define MAFP_DESC_BYTES 16           /* 128-bit binary descriptor */
#define MAFP_KP_META 4               /* row(u8) + col(u8) + orientation(u16) */
#define MAFP_KP_SIZE (MAFP_DESC_BYTES + MAFP_KP_META)           /* 20 */
#define MAFP_BANK_DATA_SZ (MAFP_MAX_KP * MAFP_KP_SIZE)      /* 1000 */
#define MAFP_BANK_SZ (4 + MAFP_BANK_DATA_SZ)           /* 1004 */
#define MAFP_TPL_MAGIC 0xEF
#define MAFP_TPL_SAMPLE_SZ (4 + MAFP_NUM_BANKS * MAFP_BANK_SZ)    /* 2012 */

/* Match scoring */
#define MAFP_MATCH_THRESH 3000       /* 0xBB8: score >= this = match */
#define MAFP_MIN_MATCH_PTS 6         /* raised from binary's 4: our simplified scoring
                                      * lacks the 9-feature sigmoid that rejects
                                      * low-quality 4-5 inlier matches */
#define MAFP_HAMMING_THRESH 48       /* max Hamming distance for descriptor match */
#define MAFP_HAMMING_RATIO 219       /* ratio test: best*256 < second*219 (≈0.855) */
#define MAFP_INLIER_DIST_SQ 9.0     /* 2303/256: binary uses Q8, we use pixel coords */
#define MAFP_MAX_INLIERS 10         /* binary hard-caps inliers at 10 */
#define MAFP_MAX_MATCHES 15         /* binary caps matches per section at 15 */
#define MAFP_MAX_KP_TOTAL 100       /* max keypoints across both DoG layers */

/* Template buffer: header + 8 enrollment samples */
#define MAFP_TPL_HDR_SZ 4
#define MAFP_TPL_BUF_SZ (MAFP_TPL_HDR_SZ + MAFP_ENROLL_STAGES * MAFP_TPL_SAMPLE_SZ)

/* Calibration file */
#define MAFP_CALIB_PATH "/var/lib/fprint/mafp_calibration"
#define MAFP_CALIB_SZ 0x2E50           /* 11856 bytes */
#define MAFP_CALIB_MAGIC 0x24

/* One step of a register write sequence; .flush entries issue the FIFO
 * flush command instead of a register write. */
typedef struct
{
  guint8   reg;
  guint8   val;
  gboolean flush;
} MafpRegEntry;

struct _FpiDeviceMafp8800
{
  FpDevice parent;

  int      spi_fd;

  /* calibration data (loaded from file or computed) */
  guint8 calib[MAFP_CALIB_SZ];

  /* image buffers (each MAFP_FRAME_BYTES = 23680 bytes of u16 in LE) */
  guint8 *bg_frame;          /* background/image_data reference */
  guint8 *cur_frame;         /* current capture */
  guint8 *stab_frame;        /* stability reference */
  guint8 *detect_ref;        /* best-finger reference (hysteresis) */

  /* enhanced output (MAFP_ENHANCED_PIXELS × 2 bytes, u16 LE) */
  guint16 *enhanced;

  /* SPI scratch buffer */
  guint8 *spi_buf;

  /* detection hysteresis state */
  gboolean detect_flag;      /* was finger detected last call */
  gint32   gray_value;       /* saved detection score */

  /* Session state carried across SSM callbacks.
   *
   * Populated when an action (open/enroll/verify/identify) begins and read by
   * the state handlers. Cleared when the action completes. Only one action can
   * be in flight at a time (libfprint enforces this), so a single copy is safe.
   */
  guint8 *tpl_buf;              /* enroll: template buffer being built */
  int     enroll_stage;         /* enroll: current stage (0-7) */
  int     tpl_count;            /* enroll: samples captured so far */
  int     stable_tries;         /* stability loop counter */

  /* verify/identify: extracted probe template */
  guint8 probe_tpl[MAFP_TPL_SAMPLE_SZ];

  /* capture sub-SSM: destination for the unpacked frame */
  guint8 *capture_dest;

  /* register-sequence sub-SSM: sequence being written */
  MafpRegEntry reg_seq[20];
  int          reg_seq_len;
  int          reg_seq_pos;

  /* live calibration: gain binary search and threshold scan progress */
  int    calib_low, calib_high, calib_mid, calib_iter;
  int    thresh_pass, thresh_v, thresh_hi, thresh_final;
  guint8 grey_resp[3][2];       /* grey level responses g0/g1/g2 */

  /* Response bytes for single register reads issued from SSM states. The
   * read target must live at a stable address until the transfer completes;
   * keeping it on the device struct avoids per-transfer heap allocations.
   * Two bytes so the register-read transfer clocks the same 4 total wire
   * bytes as the original protocol; the response is in reg_response[0].
   */
  guint8 reg_response[2];

  /* Retry counter for chip-ID polls (after reset and during the grey level
   * settle wait). Reset to 0 before each poll sequence, bounded at 20. */
  int poll_retries;
};

G_DECLARE_FINAL_TYPE (FpiDeviceMafp8800, fpi_device_mafp8800, FPI, DEVICE_MAFP8800, FpDevice)
G_DEFINE_TYPE (FpiDeviceMafp8800, fpi_device_mafp8800, FP_TYPE_DEVICE)

/* SPI transfer builders
 *
 * These construct FpiSpiTransfer objects for the primitive operations the
 * FP36 chip exposes. They do not submit; the caller (an SSM state handler)
 * submits and receives completion via callback.
 *
 * The FP36 register protocol on the wire is a 4-byte sequence:
 * [reg, val, response, ignored]. The old full-duplex driver sent all four
 * bytes with tx=rx=same buffer and read the response from rx[2]. Under
 * half-duplex fpi_spi_transfer, this is equivalent to a 2-byte write
 * {reg, val} followed by a 2-byte read, with chip-select held down between
 * (which is the default when both fit in one SPI ioctl block). The response
 * appears at buffer_rd[0].
 */

static FpiSpiTransfer *
mafp_build_write_reg (FpiDeviceMafp8800 * self, guint8 reg, guint8 val)
{
  FpiSpiTransfer *xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 2);
  xfer->buffer_wr[0] = reg;
  xfer->buffer_wr[1] = val;
  return xfer;
}

/* Write reg=val and read the response the chip returns while the third wire
 * byte is clocked. The read is 2 bytes so the transfer clocks 4 bytes total,
 * exactly like the original full-duplex register sequence; the response is
 * out_response[0] and out_response[1] is discarded. out_response must be at
 * least 2 bytes and remain valid until the transfer completes.
 */
static FpiSpiTransfer *
mafp_build_write_reg_read (FpiDeviceMafp8800 *self, guint8 reg, guint8 val,
                           guint8 *out_response)
{
  FpiSpiTransfer *xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 2);
  xfer->buffer_wr[0] = reg;
  xfer->buffer_wr[1] = val;
  fpi_spi_transfer_read_full (xfer, out_response, 2, NULL);
  return xfer;
}

/* Flush transfer: write 0x78 then clock out 0x25 more bytes so the chip's
 * FIFO settles. We do not care about the received data.
 */
static FpiSpiTransfer *
mafp_build_flush (FpiDeviceMafp8800 *self)
{
  FpiSpiTransfer *xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 0x26);
  memset (xfer->buffer_wr, 0, 0x26);
  xfer->buffer_wr[0] = 0x78;
  return xfer;
}

/* Read a raw image dump: write 0x70 then read MAFP_RAW_READ_SZ-1 bytes.
 * The caller supplies the destination buffer (typically self->spi_buf).
 */
static FpiSpiTransfer *
mafp_build_read_image (FpiDeviceMafp8800 *self, guint8 *dest)
{
  FpiSpiTransfer *xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = 0x70;
  fpi_spi_transfer_read_full (xfer, dest, MAFP_RAW_READ_SZ - 1, NULL);
  return xfer;
}

/* Read a small 0x400-byte chunk used during calibration's gain search. */
static FpiSpiTransfer *
mafp_build_read_calib_chunk (FpiDeviceMafp8800 *self, guint8 *dest)
{
  FpiSpiTransfer *xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = 0x70;
  fpi_spi_transfer_read_full (xfer, dest, 0x400 - 1, NULL);
  return xfer;
}

/* Unpack a raw SPI dump into row-major frame data, byte-swapping u16 samples.
 *
 * The chip transmits framed rows preceded by a 4-byte marker
 * (0x00 0x00 0x0A 0x5x). Scan up to max_scan bytes, extract at most max_rows
 * rows of MAFP_ROW_BYTES each into out_frame. Pure CPU: called from SSM
 * callbacks after an image read completes.
 */
static int
mafp_unpack_frame (const guint8 *raw, int max_scan,
                   guint8 *out_frame, int max_rows)
{
  int rows = 0;
  int i = 0;

  while (i < max_scan - 4 && rows < max_rows)
    {
      if (raw[i] == 0x00 && raw[i + 1] == 0x00 &&
          raw[i + 2] == 0x0A && (raw[i + 3] & 0xF0) == 0x50)
        {
          int src = i + 4;
          if (src + MAFP_ROW_BYTES > max_scan)
            break;
          memcpy (out_frame + rows * MAFP_ROW_BYTES, raw + src, MAFP_ROW_BYTES);
          rows++;
          i = src + MAFP_ROW_BYTES;
        }
      else
        {
          i++;
        }
    }

  for (int j = 0; j < rows * MAFP_ROW_BYTES; j += 2)
    {
      guint8 tmp = out_frame[j];
      out_frame[j] = out_frame[j + 1];
      out_frame[j + 1] = tmp;
    }

  return rows;
}

static inline guint16
frame_pixel (const guint8 *frame, int row, int col)
{
  int off = row * MAFP_ROW_BYTES + col * 2;

  return (guint16) frame[off] | ((guint16) frame[off + 1] << 8);
}

/* Calibration file I/O */

static guint8
mafp_crc8 (const guint8 *data, gsize len)
{
  guint8 crc = 0;

  for (gsize i = 0; i < len; i++)
    crc ^= data[i];
  return crc;
}

static gboolean
mafp_load_calib (FpiDeviceMafp8800 *self)
{
  FILE *f = fopen (MAFP_CALIB_PATH, "rb");

  if (!f)
    return FALSE;
  gsize n = fread (self->calib, 1, MAFP_CALIB_SZ, f);
  fclose (f);
  if (n < MAFP_CALIB_SZ)
    return FALSE;
  if (self->calib[0] != MAFP_CALIB_MAGIC)
    return FALSE;
  guint8 crc = mafp_crc8 (self->calib, 0x2E4C);
  return self->calib[0x2E4C] == crc;
}

static void
mafp_save_calib (FpiDeviceMafp8800 *self)
{
  self->calib[0] = MAFP_CALIB_MAGIC;
  guint8 crc = mafp_crc8 (self->calib, 0x2E4C);
  self->calib[0x2E4C] = crc;

  g_mkdir_with_parents ("/var/lib/fprint", 0755);
  FILE *f = fopen (MAFP_CALIB_PATH, "wb");
  if (f)
    {
      fwrite (self->calib, 1, MAFP_CALIB_SZ, f);
      fsync (fileno (f));
      fclose (f);
      fp_dbg ("calibration saved to %s", MAFP_CALIB_PATH);
    }
}

/* Evaluate whether the current frame shows a finger, using hysteresis over
 * the running gray-value peak. Pure CPU: called from an SSM callback after
 * an image read completes and the frame has been unpacked into self->cur_frame.
 */
static gboolean
mafp_evaluate_detect (FpiDeviceMafp8800 *self)
{
  int changed = 0;

  for (int row = 0; row < MAFP_ROWS; row++)
    for (int col = 1; col < MAFP_COLS; col++)
      {
        gint32 bg = (gint32) frame_pixel (self->bg_frame, row, col);
        gint32 cur = (gint32) frame_pixel (self->cur_frame, row, col);
        if ((bg - cur) > MAFP_DETECT_PX_THRESH)
          changed++;
      }

  gint32 raw_score = changed * 100;
  gboolean detected = raw_score > MAFP_DETECT_RAW_LIMIT;
  gint32 pct = changed * 100 / MAFP_ENHANCED_PIXELS;

  if (!detected)
    {
      self->detect_flag = FALSE;
      return FALSE;
    }

  if (!self->detect_flag)
    {
      memset (self->detect_ref, 0, MAFP_FRAME_BYTES);
      self->gray_value = 0;
    }

  if (self->gray_value < pct)
    {
      memcpy (self->detect_ref, self->cur_frame, MAFP_FRAME_BYTES);
      self->gray_value = pct;
    }

  self->detect_flag = TRUE;
  return TRUE;
}

static gboolean
mafp_fp36_finger_is_stable (FpiDeviceMafp8800 *self)
{
  long sad = 0;

  for (int row = 0; row < MAFP_ROWS; row++)
    for (int col = 1; col < MAFP_COLS; col++)
      {
        gint32 a = (gint32) frame_pixel (self->cur_frame, row, col);
        gint32 b = (gint32) frame_pixel (self->stab_frame, row, col);
        gint32 d = b - a;
        if (d < 0)
          d = -d;
        sad += d;
      }
  return sad <= MAFP_STABLE_SAD_LIMIT;
}

/* Asynchronous building blocks
 *
 * Everything below drives the chip through FpiSpiTransfer submissions from
 * FpiSsm state handlers. The small sub-machines (register sequence, reset,
 * frame capture) are composed into the top-level open/enroll/verify machines
 * with fpi_ssm_start_subsm, which advances the parent on completion and
 * fails it on error.
 */

/* Register-sequence sub-SSM: write self->reg_seq[0..reg_seq_len) in order */

enum {
  MAFP_REGSEQ_WRITE,
  MAFP_REGSEQ_ITERATE,
  MAFP_REGSEQ_NSTATES
};

static void
mafp_regseq_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  const MafpRegEntry *entry;
  FpiSpiTransfer *xfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_REGSEQ_WRITE:
      entry = &self->reg_seq[self->reg_seq_pos];
      if (entry->flush)
        xfer = mafp_build_flush (self);
      else
        xfer = mafp_build_write_reg (self, entry->reg, entry->val);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_REGSEQ_ITERATE:
      if (++self->reg_seq_pos < self->reg_seq_len)
        fpi_ssm_jump_to_state (ssm, MAFP_REGSEQ_WRITE);
      else
        fpi_ssm_mark_completed (ssm);
      return;
    }
}

static void
mafp_start_reg_seq (FpiDeviceMafp8800 *self, FpiSsm *parent,
                    const MafpRegEntry *seq, int len)
{
  FpiSsm *chld;

  g_assert (len <= (int) G_N_ELEMENTS (self->reg_seq));
  memcpy (self->reg_seq, seq, len * sizeof (MafpRegEntry));
  self->reg_seq_len = len;
  self->reg_seq_pos = 0;
  chld = fpi_ssm_new (FP_DEVICE (self), mafp_regseq_handler,
                      MAFP_REGSEQ_NSTATES);
  fpi_ssm_silence_debug (chld);
  fpi_ssm_start_subsm (parent, chld);
}

/* Enter capture mode: gain, integration, DAC, then flush the FIFO */
static void
mafp_start_capture_mode (FpiDeviceMafp8800 *self, FpiSsm *parent,
                         guint8 gain, guint8 integration, guint8 dac)
{
  const MafpRegEntry seq[] = {
    { 0x20, 0x8F, FALSE }, { 0x18, gain, FALSE }, { 0x38, 0x02, FALSE },
    { 0x40, 0x00, FALSE }, { 0x48, 0x25, FALSE }, { 0x3C, integration, FALSE },
    { 0x44, dac, FALSE }, { 0x00, 0x00, TRUE },
  };

  mafp_start_reg_seq (self, parent, seq, G_N_ELEMENTS (seq));
}

/* Integration control init used by the calibration threshold scan */
static void
mafp_start_int_ctl_init (FpiDeviceMafp8800 *self, FpiSsm *parent)
{
  static const MafpRegEntry seq[] = {
    { 0x10, 0xBF, FALSE }, { 0x00, 0x00, TRUE },
    { 0x20, 0x80, FALSE }, { 0x28, 0x00, FALSE }, { 0x38, 0x02, FALSE },
    { 0x3C, 0x38, FALSE }, { 0x44, 0x78, FALSE }, { 0x40, 0x08, FALSE },
    { 0x48, 0x1E, FALSE }, { 0x4C, 0x88, FALSE }, { 0x50, 0x00, FALSE },
    { 0x54, 0x00, FALSE }, { 0x58, 0x00, FALSE }, { 0x5C, 0x00, FALSE },
  };

  mafp_start_reg_seq (self, parent, seq, G_N_ELEMENTS (seq));
}

/* Detection mode with calibrated thresholds (0x38 = 0x06, detect scan) */
static void
mafp_start_detect_mode (FpiDeviceMafp8800 *self, FpiSsm *parent)
{
  const MafpRegEntry seq[] = {
    { 0x10, 0xBF, FALSE }, { 0x00, 0x00, TRUE },
    { 0x20, 0x80, FALSE }, { 0x28, 0x00, FALSE }, { 0x38, 0x06, FALSE },
    { 0x3C, 0x38, FALSE }, { 0x44, 0x78, FALSE }, { 0x40, 0x08, FALSE },
    { 0x48, 0x1E, FALSE }, { 0x4C, 0x88, FALSE }, { 0x50, 0xFF, FALSE },
    { 0x54, self->calib[4], FALSE }, { 0x58, self->calib[5], FALSE },
    { 0x5C, self->calib[6], FALSE }, { 0x18, self->calib[2], FALSE },
    { 0x84, 0x00, FALSE },
  };

  mafp_start_reg_seq (self, parent, seq, G_N_ELEMENTS (seq));
}

/* Reset sub-SSM: issue the reset command, then poll for the chip ID. Used
 * standalone by open and as a child machine before every capture. */

enum {
  MAFP_RESET_WRITE,
  MAFP_RESET_POLL_ID,
  MAFP_RESET_CHECK_ID,
  MAFP_RESET_NSTATES
};

static void
mafp_reset_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  FpiSpiTransfer *xfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_RESET_WRITE:
      self->poll_retries = 0;
      xfer = mafp_build_write_reg (self, 0x8C, 0xFF);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_RESET_POLL_ID:
      xfer = mafp_build_write_reg_read (self, 0x04, 0x00, self->reg_response);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_RESET_CHECK_ID:
      if (self->reg_response[0] == MAFP_CHIPID_FP36)
        {
          fpi_ssm_mark_completed (ssm);
          return;
        }

      if (++self->poll_retries >= 20)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "chip not responding to reset "
                                                         "(last id=0x%02x, expected 0x%02x)",
                                                         self->reg_response[0],
                                                         MAFP_CHIPID_FP36));
          return;
        }

      fpi_ssm_jump_to_state_delayed (ssm, MAFP_RESET_POLL_ID, 1);
      return;
    }
}

static void
mafp_start_reset (FpiDeviceMafp8800 *self, FpiSsm *parent)
{
  FpiSsm *chld = fpi_ssm_new (FP_DEVICE (self), mafp_reset_ssm_handler,
                              MAFP_RESET_NSTATES);

  fpi_ssm_silence_debug (chld);
  fpi_ssm_start_subsm (parent, chld);
}

/* Capture sub-SSM: reset, enter capture mode, read one raw dump and unpack
 * it into self->capture_dest */

enum {
  MAFP_CAPT_RESET,
  MAFP_CAPT_MODE,
  MAFP_CAPT_READ,
  MAFP_CAPT_PROCESS,
  MAFP_CAPT_NSTATES
};

static void
mafp_capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  FpiSpiTransfer *xfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_CAPT_RESET:
      mafp_start_reset (self, ssm);
      return;

    case MAFP_CAPT_MODE:
      mafp_start_capture_mode (self, ssm, self->calib[1], 0x02, 0xA1);
      return;

    case MAFP_CAPT_READ:
      xfer = mafp_build_read_image (self, self->spi_buf);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CAPT_PROCESS:
      mafp_unpack_frame (self->spi_buf, MAFP_RAW_READ_SZ - 1,
                         self->capture_dest, MAFP_ROWS);
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

static void
mafp_start_capture (FpiDeviceMafp8800 *self, FpiSsm *parent, guint8 *dest)
{
  FpiSsm *chld = fpi_ssm_new (FP_DEVICE (self), mafp_capture_ssm_handler,
                              MAFP_CAPT_NSTATES);

  fpi_ssm_silence_debug (chld);
  self->capture_dest = dest;
  fpi_ssm_start_subsm (parent, chld);
}

/* Calibration SSM: load the cached calibration if valid, otherwise run the
 * live procedure (binary gain search, background capture, 3-pass detection
 * threshold scan) and persist the result. */

enum {
  MAFP_CALIB_LOAD,
  MAFP_CALIB_GAIN_RESET,
  MAFP_CALIB_GAIN_MODE,
  MAFP_CALIB_GAIN_READ,
  MAFP_CALIB_GAIN_EVAL,
  MAFP_CALIB_BG_CAPTURE,
  MAFP_CALIB_BG_STORE,
  MAFP_CALIB_THRESH_RESET,
  MAFP_CALIB_THRESH_INTCTL,
  MAFP_CALIB_GREY_SET,
  MAFP_CALIB_GREY_WAIT,
  MAFP_CALIB_GREY_TRIG,
  MAFP_CALIB_GREY_POLL,
  MAFP_CALIB_GREY_CHECK,
  MAFP_CALIB_GREY_G0,
  MAFP_CALIB_GREY_G1,
  MAFP_CALIB_GREY_G2,
  MAFP_CALIB_THRESH_EVAL,
  MAFP_CALIB_SAVE,
  MAFP_CALIB_NSTATES
};

/* Threshold scan passes: coarse, fine, finest. Offsets are relative to the
 * previous pass's result (absolute on the first pass). */
static const struct
{
  int start_off, end_off, step, back;
} mafp_thresh_passes[3] = {
  { 0, 256, 16, 15 }, { 0, 16, 4, 0 }, { -3, 1, 1, 0 },
};

/* Enter threshold pass self->thresh_pass, or jump to SAVE when done.
 * Degenerate (empty) ranges skip to the next pass, matching the original
 * loop bounds. */
static void
mafp_calib_enter_pass (FpiDeviceMafp8800 *self, FpiSsm *ssm)
{
  while (self->thresh_pass < 3)
    {
      int p = self->thresh_pass;
      int lo, hi;

      if (p == 0)
        {
          lo = mafp_thresh_passes[p].start_off;
          hi = mafp_thresh_passes[p].end_off;
        }
      else
        {
          lo = self->thresh_final + mafp_thresh_passes[p].start_off;
          hi = self->thresh_final + mafp_thresh_passes[p].end_off;
        }
      if (lo < 0)
        lo = 0;

      if (lo < hi)
        {
          self->thresh_v = lo;
          self->thresh_hi = hi;
          fpi_ssm_jump_to_state (ssm, MAFP_CALIB_THRESH_RESET);
          return;
        }
      self->thresh_pass++;
    }
  fpi_ssm_jump_to_state (ssm, MAFP_CALIB_SAVE);
}

static void
mafp_calib_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  FpiSpiTransfer *xfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_CALIB_LOAD:
      if (mafp_load_calib (self))
        {
          fp_dbg ("loaded cached calibration (gain=%d, detect_int=%d, thresh=%d/%d/%d)",
                  self->calib[1], self->calib[2],
                  self->calib[4], self->calib[5], self->calib[6]);
          memcpy (self->bg_frame, self->calib + 8,
                  MIN ((gsize) (MAFP_CALIB_SZ - 8), (gsize) MAFP_FRAME_BYTES));
          fpi_ssm_mark_completed (ssm);
          return;
        }
      fp_dbg ("running live calibration...");
      self->calib_low = 0;
      self->calib_high = 255;
      self->calib_iter = 0;
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_CALIB_GAIN_RESET:
      self->calib_mid = (self->calib_low + self->calib_high) / 2;
      mafp_start_reset (self, ssm);
      return;

    case MAFP_CALIB_GAIN_MODE:
      mafp_start_capture_mode (self, ssm, (guint8) self->calib_mid, 0x4C, 0x54);
      return;

    case MAFP_CALIB_GAIN_READ:
      xfer = mafp_build_read_calib_chunk (self, self->spi_buf);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_GAIN_EVAL:
      {
        /* Sum the first 8 rows as a brightness estimate. The unpack
         * byte-swaps to LE, so summing LE words here equals the original
         * driver's big-endian sum over unswapped data. */
        guint8 rows[8 * MAFP_ROW_BYTES] = { 0 };
        long total = 0;

        mafp_unpack_frame (self->spi_buf, 0x400 - 1, rows, 8);
        for (int j = 0; j < (int) sizeof (rows); j += 2)
          total += (long) rows[j] | ((long) rows[j + 1] << 8);

        fp_dbg ("calibrate gain search: gain=%d total=%ld (target=%d)",
                self->calib_mid, total, 0x4007F);

        if (total > 0x4007F)
          self->calib_high = self->calib_mid;
        else
          self->calib_low = self->calib_mid;

        if (++self->calib_iter < 8)
          {
            fpi_ssm_jump_to_state (ssm, MAFP_CALIB_GAIN_RESET);
            return;
          }

        self->calib[1] = (guint8) self->calib_mid;
        fp_dbg ("calibration: gain=%d", self->calib_mid);
        fpi_ssm_next_state (ssm);
        return;
      }

    case MAFP_CALIB_BG_CAPTURE:
      mafp_start_capture (self, ssm, self->bg_frame);
      return;

    case MAFP_CALIB_BG_STORE:
      memcpy (self->calib + 8, self->bg_frame,
              MIN ((gsize) (MAFP_CALIB_SZ - 8), (gsize) MAFP_FRAME_BYTES));
      self->thresh_pass = 0;
      self->thresh_final = 0;
      mafp_calib_enter_pass (self, ssm);
      return;

    case MAFP_CALIB_THRESH_RESET:
      mafp_start_reset (self, ssm);
      return;

    case MAFP_CALIB_THRESH_INTCTL:
      mafp_start_int_ctl_init (self, ssm);
      return;

    case MAFP_CALIB_GREY_SET:
      {
        const MafpRegEntry seq[] = {
          { 0x18, (guint8) self->thresh_v, FALSE }, { 0x84, 0x00, FALSE },
        };
        mafp_start_reg_seq (self, ssm, seq, G_N_ELEMENTS (seq));
        return;
      }

    case MAFP_CALIB_GREY_WAIT:
      fpi_ssm_next_state_delayed (ssm, 10);
      return;

    case MAFP_CALIB_GREY_TRIG:
      self->poll_retries = 0;
      xfer = mafp_build_write_reg (self, 0x88, 0x00);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_GREY_POLL:
      xfer = mafp_build_write_reg_read (self, 0x04, 0x00, self->reg_response);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_GREY_CHECK:
      /* Read the grey levels once the chip settles or the poll budget is
       * exhausted; the original driver reads them either way. */
      if (self->reg_response[0] != MAFP_CHIPID_FP36 && ++self->poll_retries < 20)
        {
          fpi_ssm_jump_to_state_delayed (ssm, MAFP_CALIB_GREY_POLL, 1);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_CALIB_GREY_G0:
      xfer = mafp_build_write_reg_read (self, 0x54, 0x00, self->grey_resp[0]);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_GREY_G1:
      xfer = mafp_build_write_reg_read (self, 0x58, 0x00, self->grey_resp[1]);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_GREY_G2:
      xfer = mafp_build_write_reg_read (self, 0x5C, 0x00, self->grey_resp[2]);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev),
                               fpi_ssm_spi_transfer_cb, NULL);
      return;

    case MAFP_CALIB_THRESH_EVAL:
      {
        guint8 g0 = self->grey_resp[0][0];
        guint8 g1 = self->grey_resp[1][0];
        guint8 g2 = self->grey_resp[2][0];

        if (g0 > 100 && g1 > 100 && g2 > 100)
          {
            int back = mafp_thresh_passes[self->thresh_pass].back;

            self->thresh_final = self->thresh_v > back ? self->thresh_v - back : 0;
            self->thresh_pass++;
            mafp_calib_enter_pass (self, ssm);
            return;
          }

        self->thresh_v += mafp_thresh_passes[self->thresh_pass].step;
        if (self->thresh_v < self->thresh_hi)
          {
            fpi_ssm_jump_to_state (ssm, MAFP_CALIB_THRESH_RESET);
            return;
          }

        self->thresh_pass++;
        mafp_calib_enter_pass (self, ssm);
        return;
      }

    case MAFP_CALIB_SAVE:
      self->calib[2] = self->thresh_final > 0 ? self->thresh_final - 1 : 0;
      self->calib[3] = 0xFF;
      self->calib[4] = self->grey_resp[0][0] > 20 ? self->grey_resp[0][0] - 20 : 0;
      self->calib[5] = self->grey_resp[1][0] > 20 ? self->grey_resp[1][0] - 20 : 0;
      self->calib[6] = self->grey_resp[2][0] > 20 ? self->grey_resp[2][0] - 20 : 0;
      fp_dbg ("calibration: detect_int=%d thresh=%d/%d/%d",
              self->calib[2], self->calib[4], self->calib[5], self->calib[6]);
      mafp_save_calib (self);
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

static void
mafp_start_calibration (FpiDeviceMafp8800 *self, FpiSsm *parent)
{
  fpi_ssm_start_subsm (parent, fpi_ssm_new (FP_DEVICE (self),
                                            mafp_calib_ssm_handler,
                                            MAFP_CALIB_NSTATES));
}

/* Image enhancement: background subtraction + normalization */
static void
mafp_fp36_enhance (FpiDeviceMafp8800 *self)
{
  guint16 *out = self->enhanced;
  const guint8 *bg = self->bg_frame;       /* no-finger background */
  const guint8 *finger = self->cur_frame;  /* current capture with finger */

  guint16 px_min = 0xFFFF, px_max = 0;

  /* Background subtract with +10000 offset, skip column 0 */
  for (int row = 0; row < MAFP_ROWS; row++)
    for (int col = 1; col < MAFP_COLS; col++)
      {
        guint16 bg_val = frame_pixel (bg, row, col);
        guint16 fg_val = frame_pixel (finger, row, col);
        guint16 val = (guint16) ((bg_val + 10000 - fg_val) & 0xFFFF);
        int idx = row * MAFP_ENHANCED_COLS + (col - 1);
        out[idx] = val;
        if (val < px_min)
          px_min = val;
        if (val > px_max)
          px_max = val;
      }

  guint16 range = px_max - px_min;
  if (range <= 50)
    {
      memset (out, 0, MAFP_ENHANCED_PIXELS * sizeof (guint16));
      return;
    }

  /* Normalize to full 16-bit range */
  for (int i = 0; i < MAFP_ENHANCED_PIXELS; i++)
    {
      guint32 val = out[i] - px_min;
      val = (val * 0xFFFF) / range;
      out[i] = (guint16) MIN (val, 0xFFFF);
    }
}

/* Scale-space keypoint matching */
static const guint16 kern7[]  = {291, 3539, 15862, 26152, 15862, 3539, 291};
static const guint16 kern9[]  = {339, 1951, 6809, 14415, 18508, 14415, 6809, 1951, 339};
static const guint16 kern13[] = {145, 575, 1771, 4248, 7937, 11549, 13086, 11549, 7937, 4248, 1771, 575, 145};
static const guint16 kern17[] = {170, 433, 977, 1942, 3409, 5280, 7217, 8706, 9268, 8706, 7217, 5280, 3409, 1942, 977, 433, 170};

static void
mafp_compute_gradients (const guint16 *img, int rows, int cols,
                        gint32 *mag, guint16 *ori)
{
  memset (mag, 0, rows * cols * sizeof (gint32));
  memset (ori, 0, rows * cols * sizeof (guint16));

  for (int r = 1; r < rows - 1; r++)
    for (int c = 1; c < cols - 1; c++)
      {
        gint32 gx = (gint32) img[r * cols + c + 1] - (gint32) img[r * cols + c - 1];
        gint32 gy = (gint32) img[(r + 1) * cols + c] - (gint32) img[(r - 1) * cols + c];
        mag[r * cols + c] = (gint32) sqrt ((double) (gx * gx + gy * gy));
        gdouble a = atan2 ((double) gy, (double) gx);
        if (a < 0)
          a += 2.0 * G_PI;
        ori[r * cols + c] = (guint16) (a / (2.0 * G_PI) * 65536.0);
      }
}

/* Internal keypoint representation */
typedef struct
{
  guint8  row;
  guint8  col;
  guint8  dog_layer;
  guint8  polarity;    /* 0 = DoG maximum, 1 = DoG minimum */
  gint16  response;    /* absolute value of DoG extremum */
  gdouble orientation;
  guint8  desc[MAFP_DESC_BYTES];
} MafpKeypoint;

/* Correspondence for RANSAC geometric verification */
typedef struct
{
  guint8  pr, pc, gr, gc;
  guint16 p_ori, g_ori;   /* keypoint orientations for angular consistency */
} MafpCorr;

/* Gaussian blur (separable, u16 fixed-point) */

static void
mafp_blur_h (const guint16 *src, guint16 *dst,
             const guint16 *kern, int ksize, int rows, int cols)
{
  int half = ksize / 2;

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        guint32 sum = 0;
        for (int k = -half; k <= half; k++)
          {
            int cc = CLAMP (c + k, 0, cols - 1);
            sum += (guint32) src[r * cols + cc] * kern[k + half];
          }
        dst[r * cols + c] = (guint16) (sum >> 16);
      }
}

static void
mafp_blur_v (const guint16 *src, guint16 *dst,
             const guint16 *kern, int ksize, int rows, int cols)
{
  int half = ksize / 2;

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        guint32 sum = 0;
        for (int k = -half; k <= half; k++)
          {
            int rr = CLAMP (r + k, 0, rows - 1);
            sum += (guint32) src[rr * cols + c] * kern[k + half];
          }
        dst[r * cols + c] = (guint16) (sum >> 16);
      }
}

static void
mafp_gauss_blur (const guint16 *src, guint16 *dst, guint16 *tmp,
                 const guint16 *kern, int ksize, int rows, int cols)
{
  mafp_blur_h (src, tmp, kern, ksize, rows, cols);
  mafp_blur_v (tmp, dst, kern, ksize, rows, cols);
}

/* DoG keypoint detection */

static int
mafp_detect_keypoints (gint16 **dog, MafpKeypoint *kps)
{
  int W = MAFP_ENHANCED_COLS;
  int count = 0;

  /* Exclude keypoints near edges: the 17×17 descriptor grid needs
   * ±16 pixels of margin. On a 36-wide image, that means col 5-30. */
  int margin_r = 8, margin_c = 5;

  for (int layer = 1; layer <= 2; layer++)
    for (int r = margin_r; r < MAFP_ROWS - margin_r; r++)
      for (int c = margin_c; c < W - margin_c; c++)
        {
          gint16 val = dog[layer][r * W + c];
          if (val == 0)
            continue;

          gboolean is_max = (val > 0), is_min = (val < 0);
          for (int dl = -1; dl <= 1 && (is_max || is_min); dl++)
            for (int dr = -1; dr <= 1 && (is_max || is_min); dr++)
              for (int dc = -1; dc <= 1 && (is_max || is_min); dc++)
                {
                  if (dl == 0 && dr == 0 && dc == 0)
                    continue;
                  gint16 nb = dog[layer + dl][(r + dr) * W + (c + dc)];
                  if (nb >= val)
                    is_max = FALSE;
                  if (nb <= val)
                    is_min = FALSE;
                }

          if ((is_max || is_min) && count < MAFP_MAX_KP_TOTAL)
            {
              kps[count].row       = (guint8) r;
              kps[count].col       = (guint8) c;
              kps[count].dog_layer = (guint8) layer;
              kps[count].polarity  = is_min ? 1 : 0;
              kps[count].response  = is_min ? -val : val;
              count++;
            }
        }

  /* If we hit the cap, keep strongest by sorting on response */
  if (count >= MAFP_MAX_KP_TOTAL)
    {
      for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
          if (kps[j].response > kps[i].response)
            {
              MafpKeypoint tmp = kps[i];
              kps[i] = kps[j];
              kps[j] = tmp;
            }
      count = MAFP_MAX_KP_TOTAL;
    }

  return count;
}

/* Orientation assignment */

static void
mafp_assign_orientations (guint16 **pyr, MafpKeypoint *kps, int count)
{
  int W = MAFP_ENHANCED_COLS;

  for (int ki = 0; ki < count; ki++)
    {
      int r0 = kps[ki].row, c0 = kps[ki].col;
      const guint16 *img = pyr[kps[ki].dog_layer];
      gdouble hist[36] = {0};
      int radius = 4;
      gdouble sigma = radius * 0.5;

      for (int dr = -radius; dr <= radius; dr++)
        {
          int r = r0 + dr;
          if (r < 1 || r >= MAFP_ROWS - 1)
            continue;
          for (int dc = -radius; dc <= radius; dc++)
            {
              int c = c0 + dc;
              if (c < 1 || c >= W - 1)
                continue;
              gdouble gx = (gdouble) img[r * W + c + 1] - (gdouble) img[r * W + c - 1];
              gdouble gy = (gdouble) img[(r + 1) * W + c] - (gdouble) img[(r - 1) * W + c];
              gdouble mag = sqrt (gx * gx + gy * gy);
              gdouble angle = atan2 (gy, gx);
              if (angle < 0)
                angle += 2.0 * G_PI;
              gdouble w = exp (-(dr * dr + dc * dc) / (2.0 * sigma * sigma));
              int bin = (int) (angle / (2.0 * G_PI) * 36.0);
              if (bin >= 36)
                bin = 35;
              hist[bin] += mag * w;
            }
        }

      /* Smooth with [0.25, 0.5, 0.25], 2 passes */
      for (int pass = 0; pass < 2; pass++)
        {
          gdouble tmp[36];
          for (int b = 0; b < 36; b++)
            tmp[b] = hist[(b + 35) % 36] * 0.25
                     + hist[b] * 0.5
                     + hist[(b + 1) % 36] * 0.25;
          memcpy (hist, tmp, sizeof (hist));
        }

      int best_bin = 0;
      gdouble best_val = hist[0];
      for (int b = 1; b < 36; b++)
        if (hist[b] > best_val)
          {
            best_val = hist[b];
            best_bin = b;
          }

      if (best_val < 48.0)
        {
          kps[ki].orientation = -1.0;
          continue;
        }

      /* Sub-bin parabolic interpolation */
      gdouble lv = hist[(best_bin + 35) % 36];
      gdouble rv = hist[(best_bin + 1) % 36];
      gdouble offset = 0;
      gdouble denom = lv + rv - 2.0 * best_val;
      if (fabs (denom) > 1e-6)
        offset = 0.5 * (lv - rv) / denom;
      kps[ki].orientation = ((best_bin + offset + 0.5) / 36.0) * 2.0 * G_PI;
    }
}

/* SIFT-WHT binary descriptor: 17x17 rotated gradient grid -> 128-bit output */

static void
mafp_compute_descriptor (const gint32 *grad_mag, const guint16 *grad_ori,
                         int rows, int cols,
                         int r0, int c0, guint16 kp_ori, guint8 *desc)
{
  gdouble angle = (gdouble) kp_ori / 65536.0 * 2.0 * G_PI;
  gdouble ca = cos (angle), sa = sin (angle);
  gint32 hist[128] = {0};
  gdouble sigma = 10.0;

  /* Phase 1: 17×17 grid, 2-pixel spacing, rotated by orientation */
  for (int gr = -8; gr <= 8; gr++)
    for (int gc = -8; gc <= 8; gc++)
      {
        gdouble rx = gc * 2.0 * ca - gr * 2.0 * sa;
        gdouble ry = gc * 2.0 * sa + gr * 2.0 * ca;
        int sr = r0 + (int) (ry + 0.5);
        int sc = c0 + (int) (rx + 0.5);
        if (sr < 1 || sr >= rows - 1 || sc < 1 || sc >= cols - 1)
          continue;

        gint32 mag = grad_mag[sr * cols + sc];
        if (mag == 0)
          continue;
        guint16 gori = grad_ori[sr * cols + sc];

        gdouble gauss = exp (-(rx * rx + ry * ry) / (2.0 * sigma * sigma));
        gint32 weight = (gint32) (mag * gauss);

        /* Trilinear interpolation into 4×4×8 histogram (matching binary) */
        gdouble sx_f = (rx + 7.5) / 5.0;
        gdouble sy_f = (ry + 7.5) / 5.0;
        guint16 rel = gori - kp_ori;
        gdouble o_f = (gdouble) rel / 8192.0;

        int sx0 = (int) floor (sx_f), sy0 = (int) floor (sy_f);
        int o0 = (int) floor (o_f);
        gdouble fx = sx_f - sx0, fy = sy_f - sy0, fo = o_f - o0;

        for (int si = 0; si <= 1; si++)
          for (int sj = 0; sj <= 1; sj++)
            for (int oi = 0; oi <= 1; oi++)
              {
                int bx = sx0 + si, by = sy0 + sj, bo = (o0 + oi) & 7;
                if (bx < 0 || bx > 3 || by < 0 || by > 3)
                  continue;
                gdouble w = weight;
                w *= si ? fx : (1.0 - fx);
                w *= sj ? fy : (1.0 - fy);
                w *= oi ? fo : (1.0 - fo);
                hist[bo * 16 + by * 4 + bx] += (gint32) w;
              }
      }

  /* Phase 2a: Rearrange to spatial-group-major */
  gint32 work[128];
  for (int sy = 0; sy < 4; sy++)
    for (int sx = 0; sx < 4; sx++)
      for (int o = 0; o < 8; o++)
        work[sy * 32 + sx * 8 + o] = hist[o * 16 + sy * 4 + sx];

  /* Phase 2b: Reduce orientations 8→4 */
  gint32 wht[64];
  for (int sp = 0; sp < 16; sp++)
    {
      int b = sp * 8;
      wht[sp * 4 + 0] = work[b + 0];
      wht[sp * 4 + 1] = work[b + 1] + work[b + 2] + work[b + 3];
      wht[sp * 4 + 2] = work[b + 4];
      wht[sp * 4 + 3] = work[b + 5] + work[b + 6] + work[b + 7];
    }

  /* Phase 2c: 4-point WHT on orientation */
  for (int sp = 0; sp < 16; sp++)
    {
      gint32 *p = &wht[sp * 4];
      gint32 a = p[0] + p[1], b = p[0] - p[1];
      gint32 c = p[2] + p[3], d = p[2] - p[3];
      p[0] = a + c;
      p[1] = a - c;
      p[2] = b - d;
      p[3] = b + d;
    }

  /* Phase 2d-g: 16-point spatial WHT (4 butterfly levels) */
  for (int stride = 4; stride <= 32; stride <<= 1)
    for (int i = 0; i < 64; i += stride * 2)
      for (int j = 0; j < stride; j++)
        {
          gint32 a = wht[i + j], b = wht[i + j + stride];
          wht[i + j] = a + b;
          wht[i + j + stride] = a - b;
        }

  /* Phase 2h: Bits 0-63 = sign of WHT coefficients (skip DC) */
  memset (desc, 0, MAFP_DESC_BYTES);
  for (int i = 1; i < 64; i++)
    if (wht[i] > 0)
      desc[i / 8] |= (guint8) (1 << (i % 8));

  /* Phase 2i: Bits 64-127 = median threshold of odd orientation bins */
  gint32 sorted[128];
  memcpy (sorted, work, sizeof (sorted));
  for (int i = 1; i < 128; i++)
    {
      gint32 key = sorted[i];
      int j = i - 1;
      while (j >= 0 && sorted[j] > key)
        {
          sorted[j + 1] = sorted[j];
          j--;
        }
      sorted[j + 1] = key;
    }
  gint32 median = sorted[64];

  for (int i = 0; i < 64; i++)
    if (work[2 * i + 1] > median)
      desc[8 + i / 8] |= (guint8) (1 << (i % 8));
}

/* Feature extraction: enhanced image -> 2012-byte template */

static int
mafp_extract_features (const guint16 *enhanced, guint8 *tpl)
{
  int R = MAFP_ROWS, C = MAFP_ENHANCED_COLS, N = MAFP_ENHANCED_PIXELS;

  /* Allocate pyramid and DoG on heap */
  guint16 *pyr[MAFP_PYR_LEVELS];
  gint16 *dog[MAFP_DOG_LEVELS];
  guint16 *tmp = g_new (guint16, N);

  for (int l = 0; l < MAFP_PYR_LEVELS; l++)
    pyr[l] = g_new (guint16, N);
  for (int l = 0; l < MAFP_DOG_LEVELS; l++)
    dog[l] = g_new (gint16, N);

  /* Build pyramid */
  memcpy (pyr[0], enhanced, N * sizeof (guint16));
  mafp_gauss_blur (pyr[0], pyr[1], tmp, kern7,  7,  R, C);
  mafp_gauss_blur (pyr[1], pyr[2], tmp, kern9,  9,  R, C);
  mafp_gauss_blur (pyr[2], pyr[3], tmp, kern13, 13, R, C);
  mafp_gauss_blur (pyr[3], pyr[4], tmp, kern17, 17, R, C);
  g_free (tmp);

  /* DoG = adjacent level difference */
  for (int l = 0; l < MAFP_DOG_LEVELS; l++)
    for (int i = 0; i < N; i++)
      dog[l][i] = (gint16) pyr[l][i] - (gint16) pyr[l + 1][i];

  /* Detect keypoints */
  MafpKeypoint kps[MAFP_MAX_KP_TOTAL];
  int n_kps = mafp_detect_keypoints (dog, kps);

  /* Assign orientations */
  mafp_assign_orientations (pyr, kps, n_kps);

  /* Compute gradient magnitude and orientation from original image */
  gint32 *grad_mag = g_new0 (gint32, N);
  guint16 *grad_ori = g_new0 (guint16, N);
  mafp_compute_gradients (pyr[1], R, C, grad_mag, grad_ori);

  /* Compute SIFT-WHT descriptors from gradient fields */
  for (int i = 0; i < n_kps; i++)
    {
      if (kps[i].orientation < 0)
        continue;
      guint16 ori16 = (guint16) (kps[i].orientation / (2.0 * G_PI) * 65536.0);
      mafp_compute_descriptor (grad_mag, grad_ori, R, C,
                               kps[i].row, kps[i].col,
                               ori16, kps[i].desc);
    }
  g_free (grad_mag);
  g_free (grad_ori);

  /* Serialize: [4-byte magic/pad] [bank0: 4+1000] [bank1: 4+1000] */
  memset (tpl, 0, MAFP_TPL_SAMPLE_SZ);
  tpl[0] = MAFP_TPL_MAGIC;

  for (int bank = 0; bank < MAFP_NUM_BANKS; bank++)
    {
      int target_polarity = bank;  /* bank 0 = maxima, bank 1 = minima */
      gint32 bcount = 0;
      guint8 *bdata = tpl + 4 + bank * MAFP_BANK_SZ;
      guint8 *kpdata = bdata + 4;

      for (int i = 0; i < n_kps && bcount < MAFP_MAX_KP; i++)
        {
          if (kps[i].polarity != target_polarity || kps[i].orientation < 0)
            continue;
          guint8 *p = kpdata + bcount * MAFP_KP_SIZE;
          memcpy (p, kps[i].desc, MAFP_DESC_BYTES);
          p[16] = kps[i].row;
          p[17] = kps[i].col;
          guint16 ori16 = (guint16) (kps[i].orientation / (2.0 * G_PI) * 65536.0);
          memcpy (p + 18, &ori16, 2);
          bcount++;
        }
      memcpy (bdata, &bcount, sizeof (gint32));
    }

  /* Cleanup */
  for (int l = 0; l < MAFP_PYR_LEVELS; l++)
    g_free (pyr[l]);
  for (int l = 0; l < MAFP_DOG_LEVELS; l++)
    g_free (dog[l]);

  gint32 b0, b1;
  memcpy (&b0, tpl + 4, 4);
  memcpy (&b1, tpl + 4 + MAFP_BANK_SZ, 4);
  fp_dbg ("extract: %d keypoints (%d + %d)", b0 + b1, b0, b1);
  return b0 + b1;
}

/* Template matching */

static inline int
mafp_hamming (const guint8 *a, const guint8 *b)
{
  const guint64 *a64 = (const guint64 *) a;
  const guint64 *b64 = (const guint64 *) b;

  return __builtin_popcountll (a64[0] ^ b64[0])
         + __builtin_popcountll (a64[1] ^ b64[1]);
}

static gboolean
mafp_solve_similarity (const MafpCorr *c,
                       gdouble *a, gdouble *b, gdouble *tx, gdouble *ty)
{
  gdouble px1 = c[0].pc, py1 = c[0].pr;
  gdouble gx1 = c[0].gc, gy1 = c[0].gr;
  gdouble px2 = c[1].pc, py2 = c[1].pr;
  gdouble gx2 = c[1].gc, gy2 = c[1].gr;

  gdouble dxp = px2 - px1, dyp = py2 - py1;
  gdouble dxg = gx2 - gx1, dyg = gy2 - gy1;
  gdouble den = dxp * dxp + dyp * dyp;

  if (den < 1.0)
    return FALSE;

  *a  = (dxp * dxg + dyp * dyg) / den;
  *b  = (dxp * dyg - dyp * dxg) / den;
  *tx = gx1 - (*a * px1 - *b * py1);
  *ty = gy1 - (*b * px1 + *a * py1);
  return fabs (*a) <= 4.0 && fabs (*b) <= 4.0;
}

static int
mafp_count_inliers (const MafpCorr *corrs, int n,
                    gdouble a, gdouble b, gdouble tx, gdouble ty,
                    MafpCorr *inlier_out, gdouble *avg_dist)
{
  int count = 0;
  gdouble total = 0;

  for (int i = 0; i < n && count < MAFP_MAX_INLIERS; i++)
    {
      gdouble ex = a * corrs[i].pc - b * corrs[i].pr + tx - corrs[i].gc;
      gdouble ey = b * corrs[i].pc + a * corrs[i].pr + ty - corrs[i].gr;
      gdouble dsq = ex * ex + ey * ey;
      if (dsq <= MAFP_INLIER_DIST_SQ)
        {
          if (inlier_out)
            inlier_out[count] = corrs[i];
          total += dsq;
          count++;
        }
    }

  if (avg_dist)
    *avg_dist = count > 0 ? total / count : 0;
  return count;
}

static int
mafp_compute_match_score (int n_inliers)
{
  if (n_inliers < MAFP_MIN_MATCH_PTS)
    return 0;
  if (n_inliers >= 8)
    return 10000;
  return MIN (n_inliers * 1250, 10000);
}

static int
mafp_match_templates (const guint8 *probe, const guint8 *gallery)
{
  MafpCorr corrs[60];   /* 2 banks × max 15 matches */
  int n_corrs = 0;

  for (int bank = 0; bank < MAFP_NUM_BANKS; bank++)
    {
      const guint8 *pb = probe   + 4 + bank * MAFP_BANK_SZ;
      const guint8 *gb = gallery + 4 + bank * MAFP_BANK_SZ;
      int bank_matches = 0;

      gint32 np, ng;
      memcpy (&np, pb, 4);
      np = MIN (np, MAFP_MAX_KP);
      memcpy (&ng, gb, 4);
      ng = MIN (ng, MAFP_MAX_KP);

      const guint8 *pk = pb + 4, *gk = gb + 4;

      for (int pi = 0; pi < np && bank_matches < MAFP_MAX_MATCHES; pi++)
        {
          const guint8 *pd = pk + pi * MAFP_KP_SIZE;
          int best = 999, second = 999, best_gi = -1;

          for (int gi = 0; gi < ng; gi++)
            {
              int d = mafp_hamming (pd, gk + gi * MAFP_KP_SIZE);
              if (d < best)
                {
                  second = best;
                  best = d;
                  best_gi = gi;
                }
              else if (d < second)
                {
                  second = d;
                }
            }

          /* Ratio test */
          if (best_gi >= 0 && best < MAFP_HAMMING_THRESH &&
              (second >= 999 || best * 256 < second * MAFP_HAMMING_RATIO))
            {
              fp_dbg ("  corr: p(%d,%d)->g(%d,%d) ham=%d/%d",
                      pd[16], pd[17],
                      gk[best_gi * MAFP_KP_SIZE + 16],
                      gk[best_gi * MAFP_KP_SIZE + 17],
                      best, second);
              corrs[n_corrs].pr = pd[16];
              corrs[n_corrs].pc = pd[17];
              memcpy (&corrs[n_corrs].p_ori, pd + 18, 2);
              corrs[n_corrs].gr = gk[best_gi * MAFP_KP_SIZE + 16];
              corrs[n_corrs].gc = gk[best_gi * MAFP_KP_SIZE + 17];
              memcpy (&corrs[n_corrs].g_ori, gk + best_gi * MAFP_KP_SIZE + 18, 2);
              n_corrs++;
              bank_matches++;
            }
        }
    }

  /* Deduplicate: keep only the best match per gallery keypoint */
  {
    int deduped = 0;
    for (int i = 0; i < n_corrs; i++)
      {
        /* Check if this gallery position was already matched by a better pair */
        gboolean dup = FALSE;
        for (int j = 0; j < deduped; j++)
          if (corrs[j].gr == corrs[i].gr && corrs[j].gc == corrs[i].gc)
            {
              dup = TRUE;
              break;
            }
        if (!dup)
          corrs[deduped++] = corrs[i];
      }
    fp_dbg ("match: %d correspondences (%d after dedup)", n_corrs, deduped);
    n_corrs = deduped;
  }

  if (n_corrs < MAFP_MIN_MATCH_PTS)
    return 0;

  /* Exhaustive pairwise RANSAC */
  int best_inliers = 0;
  gdouble best_avg_dist = 1e9;

  for (int i = 0; i < n_corrs - 1; i++)
    for (int j = i + 1; j < n_corrs; j++)
      {
        /* Angular consistency: both correspondences must agree on rotation */
        guint16 dp = corrs[i].p_ori - corrs[j].p_ori;
        guint16 dg = corrs[i].g_ori - corrs[j].g_ori;
        gint16 adiff = (gint16) (dp - dg);
        if (adiff < 0)
          adiff = -adiff;
        if (adiff > 1822)  /* ~10 degrees */
          continue;

        MafpCorr pair[2] = { corrs[i], corrs[j] };
        gdouble a, b, tx, ty;
        if (!mafp_solve_similarity (pair, &a, &b, &tx, &ty))
          continue;

        /* Count inliers */
        gdouble avg_d = 0;
        MafpCorr inliers_buf[MAFP_MAX_INLIERS];
        int inl = mafp_count_inliers (corrs, n_corrs, a, b, tx, ty,
                                      inliers_buf, &avg_d);

        if (inl >= MAFP_MAX_INLIERS)
          {
            best_inliers = inl;
            best_avg_dist = avg_d;
            goto done;
          }

        if (inl >= MAFP_MIN_MATCH_PTS)
          {
            /* Refit transform from inliers */
            if (inl >= 2)
              {
                MafpCorr rpair[2] = { inliers_buf[0], inliers_buf[1] };
                gdouble ra, rb, rtx, rty;
                if (mafp_solve_similarity (rpair, &ra, &rb, &rtx, &rty))
                  {
                    gdouble ravg = 0;
                    int rinl = mafp_count_inliers (corrs, n_corrs,
                                                   ra, rb, rtx, rty,
                                                   NULL, &ravg);
                    if (rinl > inl || (rinl == inl && ravg < avg_d))
                      {
                        inl = rinl;
                        avg_d = ravg;
                      }
                  }
              }

            if (inl > best_inliers ||
                (inl == best_inliers && avg_d < best_avg_dist))
              {
                best_inliers = inl;
                best_avg_dist = avg_d;
              }
          }
      }

done:;
  int score = mafp_compute_match_score (best_inliers);
  fp_dbg ("match: inliers=%d avg_dist=%.1f score=%d (thresh=%d)",
          best_inliers, best_avg_dist, score, MAFP_MATCH_THRESH);
  return score;
}

/* Cleanup used by mafp_open on SSM failure and by mafp_close normally. */
static void
mafp_release_session (FpiDeviceMafp8800 *self)
{
  g_clear_pointer (&self->bg_frame, g_free);
  g_clear_pointer (&self->cur_frame, g_free);
  g_clear_pointer (&self->stab_frame, g_free);
  g_clear_pointer (&self->detect_ref, g_free);
  g_clear_pointer (&self->enhanced, g_free);
  g_clear_pointer (&self->spi_buf, g_free);
  g_clear_pointer (&self->tpl_buf, g_free);

  if (self->spi_fd >= 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
    }
}

static void
mafp_open_done_cb (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);

  if (error)
    {
      mafp_release_session (self);
      fpi_device_open_complete (dev, error);
      return;
    }

  fp_info ("chip ID=0x%02x", self->reg_response[0]);
  fpi_device_open_complete (dev, NULL);
}

static void
mafp_open (FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  const char *path = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);

  fp_info ("opening %s", path ? path : "(null)");

  if (!path)
    {
      fpi_device_open_complete (dev,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                          "no spidev path"));
      return;
    }

  self->spi_fd = open (path, O_RDWR);
  if (self->spi_fd < 0)
    {
      fpi_device_open_complete (dev,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                          "open %s: %s",
                                                          path, g_strerror (errno)));
      return;
    }

  guint8 mode = SPI_MODE_0;
  guint8 bpw = 8;
  guint32 speed = MAFP_SPI_SPEED;
  if (ioctl (self->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
    {
      int e = errno;
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                          "SPI configuration failed: %s",
                                                          g_strerror (e)));
      return;
    }

  /* Allocate session buffers up front. They're needed by all subsequent
   * operations and freeing/reallocating between open->enroll->verify would
   * just churn. Freed by mafp_release_session on error or close.
   */
  self->bg_frame   = g_malloc0 (MAFP_FRAME_BYTES);
  self->cur_frame  = g_malloc0 (MAFP_FRAME_BYTES);
  self->stab_frame = g_malloc0 (MAFP_FRAME_BYTES);
  self->detect_ref = g_malloc0 (MAFP_FRAME_BYTES);
  self->enhanced   = g_new0 (guint16, MAFP_ENHANCED_PIXELS);
  self->spi_buf    = g_malloc0 (MAFP_RAW_READ_SZ);

  fpi_ssm_start (fpi_ssm_new (dev, mafp_reset_ssm_handler, MAFP_RESET_NSTATES),
                 mafp_open_done_cb);
}

static void
mafp_close (FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);

  mafp_release_session (self);
  fpi_device_close_complete (dev, NULL);
}

/* CPU-bound frame processing (enhancement + feature extraction, roughly
 * 50-150 ms) runs on a GTask worker thread so the main loop is not blocked,
 * mirroring how the image-device layer runs minutiae detection (see
 * fp_image_detect_minutiae). The parent SSM is parked in its current state
 * while the task runs; the completion callback advances or fails it. */

static void
mafp_process_thread_func (GTask *task, gpointer source_object,
                          gpointer task_data, GCancellable *cancellable)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (source_object);
  guint8 *tpl_dest = task_data;

  mafp_fp36_enhance (self);
  mafp_extract_features (self->enhanced, tpl_dest);
  g_task_return_boolean (task, TRUE);
}

static void
mafp_start_process (FpiDeviceMafp8800 *self, guint8 *tpl_dest,
                    GAsyncReadyCallback callback, FpiSsm *ssm)
{
  g_autoptr(GTask) task = NULL;

  task = g_task_new (self, fpi_device_get_cancellable (FP_DEVICE (self)),
                     callback, ssm);
  g_task_set_task_data (task, tpl_dest, NULL);
  g_task_run_in_thread (task, mafp_process_thread_func);
}

static void
mafp_enroll_process_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (source);
  FpiSsm *ssm = user_data;
  GError *error = NULL;

  if (!g_task_propagate_boolean (G_TASK (res), &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->tpl_count++;
  fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);
  fpi_ssm_next_state (ssm);
}

/* Enroll SSM: calibrate, enter detection mode, then for each of the 8
 * stages wait for a finger, wait for it to stabilize, extract features,
 * and wait for removal. Unstable captures retry the same stage. */

enum {
  MAFP_ENROLL_CALIB,
  MAFP_ENROLL_DM_RESET,
  MAFP_ENROLL_DM_REGS,
  MAFP_ENROLL_STAGE_INIT,
  MAFP_ENROLL_WAIT_CAPTURE,
  MAFP_ENROLL_WAIT_CHECK,
  MAFP_ENROLL_STABLE_DELAY,
  MAFP_ENROLL_STABLE_CAPTURE,
  MAFP_ENROLL_STABLE_CHECK,
  MAFP_ENROLL_PROCESS,
  MAFP_ENROLL_REMOVE_DELAY,
  MAFP_ENROLL_REMOVE_CAPTURE,
  MAFP_ENROLL_REMOVE_CHECK,
  MAFP_ENROLL_NSTATES
};

static void
mafp_enroll_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_ENROLL_CALIB:
      fp_dbg ("enroll: starting");
      mafp_start_calibration (self, ssm);
      return;

    case MAFP_ENROLL_DM_RESET:
      mafp_start_reset (self, ssm);
      return;

    case MAFP_ENROLL_DM_REGS:
      mafp_start_detect_mode (self, ssm);
      return;

    case MAFP_ENROLL_STAGE_INIT:
      fp_dbg ("enroll: stage %d/%d", self->enroll_stage + 1, MAFP_ENROLL_STAGES);
      self->detect_flag = FALSE;
      self->gray_value = 0;
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_ENROLL_WAIT_CAPTURE:
      mafp_start_capture (self, ssm, self->cur_frame);
      return;

    case MAFP_ENROLL_WAIT_CHECK:
      if (!mafp_evaluate_detect (self))
        {
          fpi_ssm_jump_to_state_delayed (ssm, MAFP_ENROLL_WAIT_CAPTURE, 50);
          return;
        }
      memcpy (self->stab_frame, self->cur_frame, MAFP_FRAME_BYTES);
      self->stable_tries = 0;
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_ENROLL_STABLE_DELAY:
      fpi_ssm_next_state_delayed (ssm, 50);
      return;

    case MAFP_ENROLL_STABLE_CAPTURE:
      mafp_start_capture (self, ssm, self->cur_frame);
      return;

    case MAFP_ENROLL_STABLE_CHECK:
      if (mafp_evaluate_detect (self))
        {
          if (mafp_fp36_finger_is_stable (self))
            {
              fpi_ssm_next_state (ssm);
              return;
            }
          memcpy (self->stab_frame, self->cur_frame, MAFP_FRAME_BYTES);
          if (++self->stable_tries < 20)
            {
              fpi_ssm_jump_to_state (ssm, MAFP_ENROLL_STABLE_DELAY);
              return;
            }
        }
      /* Finger lifted or never settled: retry this stage. */
      fp_dbg ("enroll: not stable, retrying stage %d", self->enroll_stage + 1);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      fpi_ssm_jump_to_state (ssm, MAFP_ENROLL_STAGE_INIT);
      return;

    case MAFP_ENROLL_PROCESS:
      mafp_start_process (self,
                          self->tpl_buf + MAFP_TPL_HDR_SZ +
                          self->tpl_count * MAFP_TPL_SAMPLE_SZ,
                          mafp_enroll_process_cb, ssm);
      return;

    case MAFP_ENROLL_REMOVE_DELAY:
      fpi_ssm_next_state_delayed (ssm, 100);
      return;

    case MAFP_ENROLL_REMOVE_CAPTURE:
      self->detect_flag = FALSE;
      self->gray_value = 0;
      mafp_start_capture (self, ssm, self->cur_frame);
      return;

    case MAFP_ENROLL_REMOVE_CHECK:
      if (mafp_evaluate_detect (self))
        {
          fpi_ssm_jump_to_state (ssm, MAFP_ENROLL_REMOVE_DELAY);
          return;
        }
      if (++self->enroll_stage < MAFP_ENROLL_STAGES)
        fpi_ssm_jump_to_state (ssm, MAFP_ENROLL_STAGE_INIT);
      else
        fpi_ssm_mark_completed (ssm);
      return;
    }
}

static void
mafp_enroll_done_cb (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);
  FpPrint *print = NULL;
  GVariant *data;

  if (error)
    {
      g_clear_pointer (&self->tpl_buf, g_free);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  memcpy (self->tpl_buf, &self->tpl_count, sizeof (gint32));

  fpi_device_get_enroll_data (dev, &print);
  data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                    self->tpl_buf, MAFP_TPL_BUF_SZ, 1);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", data, NULL);
  g_clear_pointer (&self->tpl_buf, g_free);

  fp_dbg ("enroll: complete, %d templates", self->tpl_count);
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
mafp_enroll (FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);

  self->tpl_buf = g_malloc0 (MAFP_TPL_BUF_SZ);
  self->tpl_count = 0;
  self->enroll_stage = 0;
  fpi_ssm_start (fpi_ssm_new (dev, mafp_enroll_ssm_handler, MAFP_ENROLL_NSTATES),
                 mafp_enroll_done_cb);
}

/* Verify/identify SSM: calibrate, enter detection mode, wait for a stable
 * finger, extract the probe template and match. Unlike enroll, the probe
 * is used after 20 stability tries even if never fully stable. */

enum {
  MAFP_VERIFY_CALIB,
  MAFP_VERIFY_DM_RESET,
  MAFP_VERIFY_DM_REGS,
  MAFP_VERIFY_DETECT_INIT,
  MAFP_VERIFY_WAIT_CAPTURE,
  MAFP_VERIFY_WAIT_CHECK,
  MAFP_VERIFY_STABLE_DELAY,
  MAFP_VERIFY_STABLE_CAPTURE,
  MAFP_VERIFY_STABLE_CHECK,
  MAFP_VERIFY_MATCH,
  MAFP_VERIFY_NSTATES
};

/* Runs on the main loop once the worker thread has extracted the probe
 * template; matching itself is a few milliseconds so it stays inline. */
static void
mafp_verify_match_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (source);
  FpDevice *dev = FP_DEVICE (self);
  FpiSsm *ssm = user_data;
  GError *error = NULL;

  if (!g_task_propagate_boolean (G_TASK (res), &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *enrolled = NULL;
      g_autoptr(GVariant) var = NULL;
      gboolean matched = FALSE;

      fpi_device_get_verify_data (dev, &enrolled);
      g_object_get (enrolled, "fpi-data", &var, NULL);

      if (var)
        {
          gsize tpl_sz = 0;
          const guint8 *tpl = g_variant_get_fixed_array (var, &tpl_sz, 1);
          if (tpl_sz >= MAFP_TPL_HDR_SZ)
            {
              gint32 count = 0;
              memcpy (&count, tpl, sizeof (gint32));
              /* count comes from storage; clamp to what the blob holds */
              count = MIN (count, (gint32) ((tpl_sz - MAFP_TPL_HDR_SZ) / MAFP_TPL_SAMPLE_SZ));
              for (int i = 0; i < count && i < MAFP_ENROLL_STAGES; i++)
                {
                  const guint8 *sample = tpl + MAFP_TPL_HDR_SZ + i * MAFP_TPL_SAMPLE_SZ;
                  int score = mafp_match_templates (self->probe_tpl, sample);
                  fp_dbg ("verify: template %d score=%d (thresh=%d)",
                          i, score, MAFP_MATCH_THRESH);
                  if (score >= MAFP_MATCH_THRESH)
                    {
                      matched = TRUE;
                      break;
                    }
                }
            }
        }

      fpi_device_verify_report (dev,
                                matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
    }
  else /* IDENTIFY */
    {
      GPtrArray *gallery = NULL;
      FpPrint *matched_print = NULL;

      fpi_device_get_identify_data (dev, &gallery);

      for (guint gi = 0; gi < gallery->len && !matched_print; gi++)
        {
          FpPrint *p = g_ptr_array_index (gallery, gi);
          g_autoptr(GVariant) var = NULL;
          g_object_get (p, "fpi-data", &var, NULL);
          if (!var)
            continue;

          gsize tpl_sz = 0;
          const guint8 *tpl = g_variant_get_fixed_array (var, &tpl_sz, 1);
          if (tpl_sz < MAFP_TPL_HDR_SZ)
            continue;

          gint32 count = 0;
          memcpy (&count, tpl, sizeof (gint32));
          /* count comes from storage; clamp to what the blob holds */
          count = MIN (count, (gint32) ((tpl_sz - MAFP_TPL_HDR_SZ) / MAFP_TPL_SAMPLE_SZ));
          for (int i = 0; i < count && i < MAFP_ENROLL_STAGES; i++)
            {
              const guint8 *sample = tpl + MAFP_TPL_HDR_SZ + i * MAFP_TPL_SAMPLE_SZ;
              if (mafp_match_templates (self->probe_tpl, sample) >= MAFP_MATCH_THRESH)
                {
                  matched_print = p;
                  break;
                }
            }
        }

      fpi_device_identify_report (dev, matched_print, NULL, NULL);
    }

  fpi_ssm_mark_completed (ssm);
}

static void
mafp_verify_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MAFP_VERIFY_CALIB:
      fp_dbg ("verify/identify: starting");
      mafp_start_calibration (self, ssm);
      return;

    case MAFP_VERIFY_DM_RESET:
      mafp_start_reset (self, ssm);
      return;

    case MAFP_VERIFY_DM_REGS:
      mafp_start_detect_mode (self, ssm);
      return;

    case MAFP_VERIFY_DETECT_INIT:
      self->detect_flag = FALSE;
      self->gray_value = 0;
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_VERIFY_WAIT_CAPTURE:
      mafp_start_capture (self, ssm, self->cur_frame);
      return;

    case MAFP_VERIFY_WAIT_CHECK:
      if (!mafp_evaluate_detect (self))
        {
          fpi_ssm_jump_to_state_delayed (ssm, MAFP_VERIFY_WAIT_CAPTURE, 50);
          return;
        }
      memcpy (self->stab_frame, self->cur_frame, MAFP_FRAME_BYTES);
      self->stable_tries = 0;
      fpi_ssm_next_state (ssm);
      return;

    case MAFP_VERIFY_STABLE_DELAY:
      fpi_ssm_next_state_delayed (ssm, 50);
      return;

    case MAFP_VERIFY_STABLE_CAPTURE:
      mafp_start_capture (self, ssm, self->cur_frame);
      return;

    case MAFP_VERIFY_STABLE_CHECK:
      if (mafp_fp36_finger_is_stable (self) || ++self->stable_tries >= 20)
        {
          fpi_ssm_next_state (ssm);
          return;
        }
      memcpy (self->stab_frame, self->cur_frame, MAFP_FRAME_BYTES);
      fpi_ssm_jump_to_state (ssm, MAFP_VERIFY_STABLE_DELAY);
      return;

    case MAFP_VERIFY_MATCH:
      mafp_start_process (self, self->probe_tpl, mafp_verify_match_cb, ssm);
      return;
    }
}

static void
mafp_verify_done_cb (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);
}

static void
mafp_verify (FpDevice *dev)
{
  fpi_ssm_start (fpi_ssm_new (dev, mafp_verify_ssm_handler, MAFP_VERIFY_NSTATES),
                 mafp_verify_done_cb);
}


/* GObject boilerplate */

static const FpIdEntry mafp_id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV,
    .spi_acpi_id = "MAFP8800", .driver_data = 0 },
  { .udev_types = 0 }
};

static void
fpi_device_mafp8800_init (FpiDeviceMafp8800 *self)
{
  self->spi_fd = -1;
}

static void
fpi_device_mafp8800_finalize (GObject *obj)
{
  FpiDeviceMafp8800 *self = FPI_DEVICE_MAFP8800 (obj);

  if (self->spi_fd >= 0)
    close (self->spi_fd);
  G_OBJECT_CLASS (fpi_device_mafp8800_parent_class)->finalize (obj);
}

static void
fpi_device_mafp8800_class_init (FpiDeviceMafp8800Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id               = "mafp8800";
  dev_class->full_name        = "Microarray MAFP Fingerprint Sensor";
  dev_class->type             = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table         = mafp_id_table;
  dev_class->scan_type        = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = MAFP_ENROLL_STAGES;

  dev_class->open     = mafp_open;
  dev_class->close    = mafp_close;
  dev_class->enroll   = mafp_enroll;
  dev_class->verify   = mafp_verify;
  dev_class->identify = mafp_verify;
  /* No dev_class->cancel — cancellation flows through GCancellable passed
   * to fpi_spi_transfer_submit and the SSM's built-in cancel handling. */

  G_OBJECT_CLASS (klass)->finalize = fpi_device_mafp8800_finalize;

  fpi_device_class_auto_initialize_features (dev_class);
}
