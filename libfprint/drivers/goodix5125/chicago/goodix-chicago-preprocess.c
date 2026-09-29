// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: adaptive state (kr/gain/multiplier), quality,
 * capture policy, output marking and the per-frame context reworked. */

/* Native state for the Chicago preprocessing boundary. */

#include <math.h>
#include <string.h>

#include <gio/gio.h>

#include "goodix-chicago-preprocess.h"

#define CHICAGO_STAGE_WIDTH 80u
#define CHICAGO_STAGE_HEIGHT 64u
#define CHICAGO_REJECT_HISTOGRAM_BINS 200u
#define CHICAGO_REJECT_FLAT_RANGE 50u /* 50 for mode 24 (150 only for modes in 0x2473800) */

typedef struct
{
  gboolean active;
  guint    flat_count;
  guint    histogram[CHICAGO_REJECT_HISTOGRAM_BINS];
} GoodixChicagoCandidateRejectStats;

static guint reflect_101 (gint  coordinate,
                          guint length);
static void compute_metrics (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                             guint8      *quality_out,
                             guint8      *coverage_out);
static gint compute_base_quality_annotated (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                            const guint8 quality_mask[GOODIX_CHICAGO_PIXELS],
                                            guint8       annotated[GOODIX_CHICAGO_PIXELS]);
static gint compute_ring_label4_percent (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                         const guint8 mask[GOODIX_CHICAGO_PIXELS]);
static guint32 contrast_map_q16 (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                 guint        threshold,
                                 guint8      *map,
                                 guint8       value);

/* Frames with framenum <= this use unity gain (limit 3 for mode 24 with
 * coating; otherwise 30). */
#define CHICAGO_GAIN_LIMIT 3u
#define CHICAGO_KR_REFERENCE 2000
#define CHICAGO_KR_THRESHOLD 1600
#define CHICAGO_REPEAT_WIDTH (CHICAGO_STAGE_WIDTH / 2)
#define CHICAGO_REPEAT_HEIGHT (CHICAGO_STAGE_HEIGHT / 2)

struct _GoodixChicagoPreprocessor
{
  GBytes *calibration;
  guint16 image_base[GOODIX_CHICAGO_PIXELS];
  guint16 gain[GOODIX_CHICAGO_PIXELS];        /* kr plane as loaded */
  guint16 offset[GOODIX_CHICAGO_PIXELS];      /* b plane as loaded */

  /* Adaptive state. framenum and kr live in the calibration block G
   * and are saved with it; everything else lives for the whole process
   * lifetime and is not reset when the preprocessor is re-initialised. */
  guint32  framenum;                                   /* G+0 */
  guint16  kr[GOODIX_CHICAGO_PIXELS];                  /* G+4 */
  guint16  normalized_gain[GOODIX_CHICAGO_PIXELS];
  guint16  multiplier[GOODIX_CHICAGO_PIXELS];
  guint16  auxiliary[GOODIX_CHICAGO_PIXELS];
  guint32  multiplier_count;
  gboolean gain_selected;
  guint32  repeat_count;
  guint16  repeat_reference[CHICAGO_REPEAT_WIDTH * CHICAGO_REPEAT_HEIGHT];
  guint8   previous_output[GOODIX_CHICAGO_PIXELS];

  /* Per-frame context (cbuf[0..5]) and the history behind it, also kept
   * for the process lifetime.  The history is serialized with the rest of
   * the adaptive state (goodix_chicago_preprocessor_save_state()). */
  guint8   context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE];
  gboolean history_started;
  gint     history_count;
  gint16   history[GOODIX_CHICAGO_PIXELS];
  guint8   presence[GOODIX_CHICAGO_PIXELS];
  gint     history_threshold;                          /* initially 60 */
  gint     history_coverage;                           /* initially 60 */
  guint8   residue[GOODIX_CHICAGO_PIXELS];
  gint     residue_count;
  gint     residue_coverage;
  gint     held_state;
  gint     held_code;
};

GoodixChicagoPreprocessor *
goodix_chicago_preprocessor_new (GBytes       *calibration,
                                 const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                 GError      **error)
{
  GoodixChicagoPreprocessor *self;

  g_return_val_if_fail (calibration != NULL, NULL);
  g_return_val_if_fail (image_base != NULL, NULL);

  if (!goodix_chicago_calibration_validate_payload (calibration, error))
    return NULL;

  self = g_new0 (GoodixChicagoPreprocessor, 1);
  self->calibration = g_bytes_ref (calibration);
  memcpy (self->image_base, image_base, sizeof (self->image_base));
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      gboolean ok;

      ok = goodix_chicago_calibration_get_corrections (calibration, pixel,
                                                       &self->gain[pixel],
                                                       &self->offset[pixel],
                                                       NULL);
      g_assert (ok);
      self->kr[pixel] = self->gain[pixel];
    }
  if (!goodix_chicago_calibration_get_temporal_sample_count (
        calibration, &self->framenum, error))
    {
      goodix_chicago_preprocessor_free (self);
      return NULL;
    }
  /* The gain planes are chosen on the first frame (select_gain). */
  self->history_threshold = 60;
  self->history_coverage = 60;

  return self;
}

static guint
temporal_reflect_101 (gint  coordinate,
                      guint length)
{
  while (coordinate < 0 || coordinate >= (gint) length)
    {
      if (coordinate < 0)
        coordinate = -coordinate;
      else
        coordinate = 2 * (gint) length - coordinate - 2;
    }

  return coordinate;
}

/* E^a for a Q16 argument 0 <= a < ln(65536), by
 * shift-and-add over a table of logarithms, result in Q16. */
static guint32
fixed_exp_positive (guint32 a)
{
  static const guint32 ln_shift[4] = { 0x58b90, 0x2c5c8, 0x162e4, 0xb172 };
  static const guint32 ln_step[16] = {
    0x67cd, 0x3920, 0x1e27, 0xf85, 0x7e1, 0x3f8, 0x1fe, 0x100,
    0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1,
  };
  guint32 result = 0x10000;

  for (guint i = 0; i < G_N_ELEMENTS (ln_shift); i++)
    if (a > ln_shift[i])
      {
        a -= ln_shift[i];
        result <<= 1u << (3 - i);
      }
  for (guint i = 0; i < G_N_ELEMENTS (ln_step); i++)
    if (a > ln_step[i])
      {
        a -= ln_step[i];
        result += result >> (i + 1);
      }
  return result;
}

/* E^-|x| in Q16 (0 beyond ln(65536)). */
static guint32
fixed_exp_negative (gint32 x)
{
  const guint32 a = ABS (x);

  if (a >= 0xb1721)
    return 0;
  return G_GUINT64_CONSTANT (0x100000000) / fixed_exp_positive (a);
}

/* Q16 Gaussian kernel of `size` taps for a Q16
 * sigma, normalized with rounding to a sum of about 65536. */
static void
fixed_gaussian_kernel (guint32 size,
                       gint32  sigma_q16,
                       gint32 *kernel)
{
  gint32 coefficient;
  gint32 sum = 0;
  const gint32 half = ((gint32) size - 1) / 2;

  if (sigma_q16 != 0)
    coefficient = G_GINT64_CONSTANT (0x1000000000000) /
                  ((gint64) sigma_q16 * sigma_q16 * 2);
  else
    coefficient = 0xb1721;
  for (gint32 x = -half; x - (-half) < (gint32) size; x++)
    {
      kernel[x + half] = fixed_exp_negative (-(x * x * coefficient));
      sum += kernel[x + half];
    }
  if (sum == 0)
    return;
  for (guint i = 0; i < size; i++)
    kernel[i] = (((gint64) kernel[i] << 16) + (sum >> 1)) / sum;
}

/* kr_update's gain smoothing: 5x5 Gaussian, sigma = 50 / framenum. */
static void
temporal_smooth_normalized_gain (GoodixChicagoPreprocessor *self)
{
  gint32 kernel[5];
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];
  const guint32 sigma_q16 = 0x320000u / self->framenum;

  fixed_gaussian_kernel (5, sigma_q16, kernel);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint delta = -2; delta <= 2; delta++)
          {
            const guint source_x = temporal_reflect_101 (
              (gint) x + delta, CHICAGO_STAGE_WIDTH);

            sum += (guint32) self->normalized_gain[
              y * CHICAGO_STAGE_WIDTH + source_x] * kernel[delta + 2];
          }
        horizontal[y * CHICAGO_STAGE_WIDTH + x] =
          MIN (sum >> 16, G_MAXUINT16);
      }

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint delta = -2; delta <= 2; delta++)
          {
            const guint source_y = temporal_reflect_101 (
              (gint) y + delta, CHICAGO_STAGE_HEIGHT);

            sum += (guint32) horizontal[
              source_y * CHICAGO_STAGE_WIDTH + x] * kernel[delta + 2];
          }
        self->normalized_gain[y * CHICAGO_STAGE_WIDTH + x] =
          MIN (sum >> 16, G_MAXUINT16);
      }
}

/* Selector 9 is a separable sigma=1.5 Gaussian. It runs
 * in the preprocessing plane's 80x64 geometry, truncates after both Q16
 * passes, and uses reflect-101 borders. */
static void
gaussian_selector9 (const guint16 source[GOODIX_CHICAGO_PIXELS],
                    guint16       filtered[GOODIX_CHICAGO_PIXELS])
{
  static const guint32 kernel[5] = {
    7869u, 15328u, 19142u, 15328u, 7869u,
  };
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint delta = -2; delta <= 2; delta++)
          {
            const guint source_x = temporal_reflect_101 (
              (gint) x + delta, CHICAGO_STAGE_WIDTH);

            sum += (guint32) source[y * CHICAGO_STAGE_WIDTH + source_x] *
                   kernel[delta + 2];
          }
        horizontal[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint delta = -2; delta <= 2; delta++)
          {
            const guint source_y = temporal_reflect_101 (
              (gint) y + delta, CHICAGO_STAGE_HEIGHT);

            sum += (guint32) horizontal[
              source_y * CHICAGO_STAGE_WIDTH + x] * kernel[delta + 2];
          }
        filtered[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }
}

/* Gain selection at the start of every frame. On the
 * first call of the process all gain planes become unity; framenum 0 also
 * clears kr and the multiplier count; framenum <= 3 forces unity gain; a
 * calibration loaded with framenum > 3 derives the gain from kr once. */
static void
select_gain (GoodixChicagoPreprocessor *self)
{
  if (!self->gain_selected)
    for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
      self->normalized_gain[pixel] = self->multiplier[pixel] =
        self->auxiliary[pixel] = 0x2000u;

  if (self->framenum == 0)
    {
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        {
          self->kr[pixel] = 0;
          self->normalized_gain[pixel] = self->multiplier[pixel] =
            self->auxiliary[pixel] = 0x2000u;
        }
      self->multiplier_count = 0;
    }
  else if (self->framenum <= CHICAGO_GAIN_LIMIT)
    {
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        self->normalized_gain[pixel] = 0x2000u;
    }
  else if (!self->gain_selected)
    {
      gint32 sum = 0;
      gint32 mean;

      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        sum += self->kr[pixel];
      mean = (sum + (gint32) GOODIX_CHICAGO_PIXELS / 2) / (gint32) GOODIX_CHICAGO_PIXELS;
      if (mean == 0)
        self->framenum = 0;
      else
        for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
          self->normalized_gain[pixel] =
            (((gint32) self->kr[pixel] << 13) + (mean >> 1)) / mean;
    }
  self->gain_selected = TRUE;
}

/* (ISFLOATING, no pixel cancel): more than 205/256 of
 * the pixels must carry a finger signal above 100. */
static gboolean
signal_covers_frame (const guint16 signal[GOODIX_CHICAGO_PIXELS])
{
  gint count = 0;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if ((gint16) signal[pixel] > 100)
      count++;
  return count > ((gint) GOODIX_CHICAGO_PIXELS * 0xcd) >> 8;
}

/* Stops the adaptive updates after more than ten
 * consecutive frames whose half-resolution signal differs from the stored
 * reference by less than 15 on average. The reference is replaced whenever a
 * frame is not a repeat (and on the first frame after a reset). */
static gboolean
check_repeated_signal (GoodixChicagoPreprocessor *self,
                       const guint16              signal[GOODIX_CHICAGO_PIXELS],
                       guint32                    multiplier_count)
{
  guint32 count;

  if (multiplier_count != 0)
    {
      gint32 sum = 0;

      for (guint y = 0; y < CHICAGO_REPEAT_HEIGHT; y++)
        for (guint x = 0; x < CHICAGO_REPEAT_WIDTH; x++)
          sum += ABS ((gint) (gint16) self->repeat_reference[y * CHICAGO_REPEAT_WIDTH + x] -
                      (gint) (gint16) signal[2 * y * CHICAGO_STAGE_WIDTH + 2 * x]);
      if (sum / (gint32) (CHICAGO_REPEAT_WIDTH * CHICAGO_REPEAT_HEIGHT) < 15)
        {
          count = self->repeat_count + 1;
          goto done;
        }
    }
  for (guint y = 0; y < CHICAGO_REPEAT_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_REPEAT_WIDTH; x++)
      self->repeat_reference[y * CHICAGO_REPEAT_WIDTH + x] =
        signal[2 * y * CHICAGO_STAGE_WIDTH + 2 * x];
  count = 0;

done:
  self->repeat_count = count;
  return count <= 10;
}

/* Updates one local-ratio plane (the multiplier when
 * `gain` is given, the auxiliary plane otherwise) and returns the amplitude
 * map v used by kr_update. */
static void
update_ratio_plane (const guint16  signal[GOODIX_CHICAGO_PIXELS],
                    const guint16 *gain,
                    guint32       *count,
                    guint16        plane[GOODIX_CHICAGO_PIXELS],
                    gint32        *amplitude)
{
  guint16 normalized[GOODIX_CHICAGO_PIXELS];
  guint16 filtered[GOODIX_CHICAGO_PIXELS];
  guint32 sum = 0;
  guint32 mean;
  guint32 scale;
  const guint32 n = *count;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      sum += signal[pixel];
      if (gain)
        {
          const guint32 divisor = gain[pixel];

          normalized[pixel] = divisor == 0 ? (guint32) signal[pixel] << 13 :
                              (((guint32) signal[pixel] << 13) + (divisor >> 1)) / divisor;
        }
      else
        {
          normalized[pixel] = signal[pixel];
        }
    }
  mean = (sum + GOODIX_CHICAGO_PIXELS / 2) / GOODIX_CHICAGO_PIXELS;
  if (mean == 0)
    mean = CHICAGO_KR_REFERENCE;
  scale = ((guint32) CHICAGO_KR_REFERENCE * 0x2000u + mean / 2) / mean;
  gaussian_selector9 (normalized, filtered);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const guint32 local_mean = filtered[pixel];
      const guint16 ratio = local_mean == 0 ? 0x2000u :
                            (((guint32) normalized[pixel] << 13) + local_mean / 2) / local_mean;

      if (amplitude)
        amplitude[pixel] = ((gint64) signal[pixel] * scale + 0x1000) >> 13;
      if (ABS ((gint) ratio - 0x2000) < 0x148)
        plane[pixel] = ((guint32) plane[pixel] * n + ratio + ((n + 1) >> 1)) / (n + 1);
    }
  *count = MIN (n + 1, 30u);
}

/* kr update from the amplitude map of update_ratio_plane(). */
static void
update_kr (GoodixChicagoPreprocessor *self,
           const gint32               amplitude[GOODIX_CHICAGO_PIXELS])
{
  const guint32 n = self->framenum;
  gint32 sum = 0;
  gint32 mean;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      if (ABS (amplitude[pixel] - CHICAGO_KR_REFERENCE) < CHICAGO_KR_THRESHOLD)
        self->kr[pixel] = ((guint32) self->kr[pixel] * n + (guint32) amplitude[pixel] +
                           ((n + 1) >> 1)) / (n + 1);
      sum += self->kr[pixel];
    }
  self->framenum = MIN (n + 1, 400u);
  mean = (sum + (gint32) GOODIX_CHICAGO_PIXELS / 2) / (gint32) GOODIX_CHICAGO_PIXELS;
  if (self->framenum <= CHICAGO_GAIN_LIMIT || mean == 0)
    return;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    self->normalized_gain[pixel] = (((gint32) self->kr[pixel] << 13) + (mean >> 1)) / mean;
  if (self->framenum <= 200)
    temporal_smooth_normalized_gain (self);
}

/* Frame normalization for mode 24 (ISFLOATING, coating):
 * the finger signal max(ImageBase - current, 0) normalized by the adaptive
 * gain. `source` is the plane that feeds the image path;
 * `auxiliary_source` (may be NULL) is the same signal normalized by the
 * auxiliary plane only, used by the capture policy. */
static void
normalize_frame (GoodixChicagoPreprocessor *self,
                 const guint16              current[GOODIX_CHICAGO_PIXELS],
                 const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                 gint                       purpose,
                 guint16                    source[GOODIX_CHICAGO_PIXELS],
                 guint16                   *auxiliary_source)
{
  guint16 signal[GOODIX_CHICAGO_PIXELS];
  guint16 divisor[GOODIX_CHICAGO_PIXELS];
  guint16 auxiliary_divisor[GOODIX_CHICAGO_PIXELS];
  g_autofree gint32 *amplitude = NULL;
  const guint32 count = self->multiplier_count;
  gboolean apply_multiplier = TRUE;

  select_gain (self);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint16 difference = image_base[pixel] - current[pixel];

      signal[pixel] = difference > 0 ? difference : 0;
    }

  if ((count < 5 && self->framenum > CHICAGO_GAIN_LIMIT) || purpose == 2)
    apply_multiplier = FALSE;

  if (signal_covers_frame (signal) &&
      check_repeated_signal (self, signal, count))
    {
      guint32 auxiliary_count = count;

      amplitude = g_new (gint32, GOODIX_CHICAGO_PIXELS);
      update_ratio_plane (signal, self->normalized_gain, &self->multiplier_count,
                          self->multiplier, amplitude);
      update_ratio_plane (signal, NULL, &auxiliary_count, self->auxiliary, NULL);
    }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      divisor[pixel] = apply_multiplier ?
                       ((guint32) self->normalized_gain[pixel] * self->multiplier[pixel] + 0x1000u) >> 13 :
                       self->normalized_gain[pixel];
      auxiliary_divisor[pixel] = apply_multiplier ? self->auxiliary[pixel] : 0x2000u;
    }

  if (amplitude)
    update_kr (self, amplitude);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint32 value = (gint32) signal[pixel] << 13;

      if (self->kr[pixel] == 0)
        {
          source[pixel] = signal[pixel];
          if (auxiliary_source)
            auxiliary_source[pixel] = signal[pixel];
          continue;
        }
      source[pixel] = divisor[pixel] == 0 ? value :
                      (value + (divisor[pixel] >> 1)) / divisor[pixel];
      if (auxiliary_source)
        auxiliary_source[pixel] = auxiliary_divisor[pixel] == 0 ? value :
                                  (value + (auxiliary_divisor[pixel] >> 1)) / auxiliary_divisor[pixel];
    }
}

void
goodix_chicago_preprocessor_free (GoodixChicagoPreprocessor *self)
{
  if (!self)
    return;

  g_clear_pointer (&self->calibration, g_bytes_unref);
  g_free (self);
}

/* ---- adaptive state serialization (openchicago format) -------------------
 * Every field that changes after preprocessor_new(), little endian, in a
 * fixed order; the calibration payload and ImageBase are saved by the caller
 * (openchicago.c) and fed back to preprocessor_new() first. */

#define STATE_FIELDS(X)                                                        \
  X (framenum, 4)                                                              \
  X (kr, 2)                                                                    \
  X (normalized_gain, 2)                                                       \
  X (multiplier, 2)                                                            \
  X (auxiliary, 2)                                                             \
  X (multiplier_count, 4)                                                      \
  X (gain_selected, 4)                                                         \
  X (repeat_count, 4)                                                          \
  X (repeat_reference, 2)                                                      \
  X (previous_output, 1)                                                       \
  X (context, 1)                                                               \
  X (history_started, 4)                                                       \
  X (history_count, 4)                                                         \
  X (history, 2)                                                               \
  X (presence, 1)                                                              \
  X (history_threshold, 4)                                                     \
  X (history_coverage, 4)                                                      \
  X (residue, 1)                                                               \
  X (residue_count, 4)                                                         \
  X (residue_coverage, 4)                                                      \
  X (held_state, 4)                                                            \
  X (held_code, 4)

G_STATIC_ASSERT (sizeof (gboolean) == 4 && sizeof (gint) == 4);

static void
state_put (GByteArray *out, const void *field, gsize size, guint unit)
{
  const guint8 *p = field;

  for (gsize i = 0; i < size; i += unit)
    for (guint b = 0; b < unit; b++)
      {
        /* host order -> little endian */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        g_byte_array_append (out, p + i + b, 1);
#else
        g_byte_array_append (out, p + i + unit - 1 - b, 1);
#endif
      }
}

static void
state_get (void *field, gsize size, guint unit, const guint8 *in)
{
  guint8 *p = field;

  for (gsize i = 0; i < size; i += unit)
    for (guint b = 0; b < unit; b++)
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      p[i + b] = in[i + b];
#else
      p[i + unit - 1 - b] = in[i + b];
#endif
}

gsize
goodix_chicago_preprocessor_state_size (void)
{
  GoodixChicagoPreprocessor *self = NULL;
  gsize size = 0;

#define X(field, unit) size += sizeof (self->field);
  STATE_FIELDS (X)
#undef X
  return size;
}

void
goodix_chicago_preprocessor_save_state (const GoodixChicagoPreprocessor *self,
                                        GByteArray                      *out)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (out != NULL);
#define X(field, unit) state_put (out, &self->field, sizeof (self->field), unit);
  STATE_FIELDS (X)
#undef X
}

gboolean
goodix_chicago_preprocessor_load_state (GoodixChicagoPreprocessor *self,
                                        const guint8              *data,
                                        gsize                      size,
                                        GError                   **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  if (size != goodix_chicago_preprocessor_state_size ())
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "openchicago: preprocessor state is %" G_GSIZE_FORMAT
                   " bytes, expected %" G_GSIZE_FORMAT,
                   size, goodix_chicago_preprocessor_state_size ());
      return FALSE;
    }
#define X(field, unit) \
  state_get (&self->field, sizeof (self->field), unit, data); \
  data += sizeof (self->field);
  STATE_FIELDS (X)
#undef X
  /* The state is only checksummed, not authenticated: bound the counters
   * that end up in divisors (n + 1) or averages, as the updates do. */
  if (self->framenum == G_MAXUINT32 || self->multiplier_count > 30 ||
      self->history_count < 0 || self->history_count > 50 ||
      self->history_threshold < 0 || self->history_threshold > 85 ||
      self->history_coverage < 0 || self->history_coverage > 100 ||
      self->residue_count < 0 || self->residue_count > 5)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: preprocessor state out of range");
      return FALSE;
    }
  return TRUE;
}

/* Tests: TRUE when both objects hold the same ImageBase, correction planes
 * and adaptive state (every member after the calibration reference; both
 * objects come from g_new0, so padding compares equal). */
gboolean
goodix_chicago_preprocessor_state_equal (const GoodixChicagoPreprocessor *a,
                                         const GoodixChicagoPreprocessor *b)
{
  const gsize start = G_STRUCT_OFFSET (GoodixChicagoPreprocessor, image_base);

  g_return_val_if_fail (a != NULL && b != NULL, FALSE);
  return memcmp ((const guint8 *) a + start, (const guint8 *) b + start,
                 sizeof (GoodixChicagoPreprocessor) - start) == 0;
}

gboolean
goodix_chicago_preprocessor_rebase (GoodixChicagoPreprocessor *self,
                                    GBytes                    *calibration,
                                    const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                    GError                   **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (calibration != NULL && image_base != NULL, FALSE);

  if (!goodix_chicago_calibration_validate_payload (calibration, error))
    return FALSE;
  g_bytes_ref (calibration);
  g_clear_pointer (&self->calibration, g_bytes_unref);
  self->calibration = calibration;
  memcpy (self->image_base, image_base, sizeof (self->image_base));
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      gboolean ok;

      ok = goodix_chicago_calibration_get_corrections (calibration, pixel, NULL,
                                                       &self->offset[pixel], NULL);
      g_assert (ok);
    }
  return TRUE;
}

GBytes *
goodix_chicago_preprocessor_get_calibration (const GoodixChicagoPreprocessor *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->calibration;
}

void
goodix_chicago_preprocessor_prepare_raw (GoodixChicagoPreprocessor *self,
                                         const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                         guint16                    prepared[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (raw != NULL);
  g_return_if_fail (prepared != NULL);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    prepared[pixel] = MIN (raw[pixel], 0x0fffu);

  for (guint column = 1; column + 1 < CHICAGO_STAGE_WIDTH; column++)
    {
      prepared[column] = prepared[CHICAGO_STAGE_WIDTH + column];
      prepared[(CHICAGO_STAGE_HEIGHT - 1) * CHICAGO_STAGE_WIDTH + column] =
        prepared[(CHICAGO_STAGE_HEIGHT - 2) * CHICAGO_STAGE_WIDTH + column];
    }
}

const guint16 *
goodix_chicago_preprocessor_get_image_base (const GoodixChicagoPreprocessor *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return self->image_base;
}

guint16
goodix_chicago_preprocessor_build_mask (
  GoodixChicagoPreprocessor *self,
  const guint16              current[GOODIX_CHICAGO_PIXELS],
  const guint16              image_base[GOODIX_CHICAGO_PIXELS],
  guint8                     mask[GOODIX_CHICAGO_PIXELS])
{
  guint32 sum = 0;
  guint count = 0;
  guint32 threshold = 120;

  g_return_val_if_fail (self != NULL, 0);
  g_return_val_if_fail (current != NULL, 0);
  g_return_val_if_fail (image_base != NULL, 0);
  g_return_val_if_fail (mask != NULL, 0);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint difference = image_base[pixel] - current[pixel];

      if (difference > 120)
        {
          sum += difference;
          count++;
        }
    }

  if (count > 0)
    {
      const guint32 derived = (sum / count) / 5;

      threshold = count > 800 ? derived : MAX (120u, derived);
    }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint difference = image_base[pixel] - current[pixel];

      mask[pixel] = threshold != 0 && difference >= (gint) threshold &&
                    current[pixel] != 0x0fff ? 0xff : 0;
    }

  return threshold;
}

void
goodix_chicago_preprocessor_center_local_mean (
  GoodixChicagoPreprocessor *self,
  const guint16              source[GOODIX_CHICAGO_PIXELS],
  const guint8               mask[GOODIX_CHICAGO_PIXELS],
  guint16                    centered[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (source != NULL);
  g_return_if_fail (mask != NULL);
  g_return_if_fail (centered != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          guint32 sum = 0;
          guint count = 0;
          gint centered_value;

          if (mask[pixel] == 0)
            {
              centered[pixel] = 3000;
              continue;
            }

          for (gint dy = -5; dy <= 5; dy++)
            {
              const gint neighbor_y = (gint) y + dy;

              if (neighbor_y < 0 || neighbor_y >= CHICAGO_STAGE_HEIGHT)
                continue;
              for (gint dx = -5; dx <= 5; dx++)
                {
                  const gint neighbor_x = (gint) x + dx;
                  guint neighbor;

                  if (neighbor_x < 0 || neighbor_x >= CHICAGO_STAGE_WIDTH)
                    continue;
                  neighbor = neighbor_y * CHICAGO_STAGE_WIDTH + neighbor_x;
                  if (mask[neighbor] == 0)
                    continue;
                  sum += source[neighbor];
                  count++;
                }
            }

          g_assert (count > 0);
          centered_value = (gint) source[pixel] -
                           (gint) ((sum + count / 2) / count) + 3000;
          centered[pixel] = centered_value < 0 ? 0 : (guint16) centered_value;
        }
    }
}

void
goodix_chicago_preprocessor_smooth_centered (
  GoodixChicagoPreprocessor *self,
  guint16                    centered[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint16 *source = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (centered != NULL);

  source = g_memdup2 (centered, sizeof (guint16) * GOODIX_CHICAGO_PIXELS);
  for (guint y = 1; y + 1 < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 1; x + 1 < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          guint32 weighted_sum;

          weighted_sum = source[pixel - CHICAGO_STAGE_WIDTH - 1] +
                         2u * source[pixel - CHICAGO_STAGE_WIDTH] +
                         source[pixel - CHICAGO_STAGE_WIDTH + 1] +
                         2u * source[pixel - 1] +
                         4u * source[pixel] +
                         2u * source[pixel + 1] +
                         source[pixel + CHICAGO_STAGE_WIDTH - 1] +
                         2u * source[pixel + CHICAGO_STAGE_WIDTH] +
                         source[pixel + CHICAGO_STAGE_WIDTH + 1];
          centered[pixel] = (weighted_sum + 8) >> 4;
        }
    }
}

void
goodix_chicago_preprocessor_build_local_envelopes (
  GoodixChicagoPreprocessor *self,
  const guint16              source[GOODIX_CHICAGO_PIXELS],
  guint16                    local_max[GOODIX_CHICAGO_PIXELS],
  guint16                    local_min[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (source != NULL);
  g_return_if_fail (local_max != NULL);
  g_return_if_fail (local_min != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          guint16 horizontal_min = source[pixel];
          guint16 horizontal_max = source[pixel];
          guint16 vertical_min = source[pixel];
          guint16 vertical_max = source[pixel];

          for (gint delta = -5; delta <= 5; delta++)
            {
              const gint neighbor_x = (gint) x + delta;
              const gint neighbor_y = (gint) y + delta;

              if (neighbor_x >= 0 && neighbor_x < CHICAGO_STAGE_WIDTH)
                {
                  const guint neighbor = y * CHICAGO_STAGE_WIDTH + neighbor_x;

                  horizontal_min = MIN (horizontal_min, source[neighbor]);
                  horizontal_max = MAX (horizontal_max, source[neighbor]);
                }
              if (neighbor_y >= 0 && neighbor_y < CHICAGO_STAGE_HEIGHT)
                {
                  const guint neighbor = neighbor_y * CHICAGO_STAGE_WIDTH + x;

                  vertical_min = MIN (vertical_min, source[neighbor]);
                  vertical_max = MAX (vertical_max, source[neighbor]);
                }
            }

          local_max[pixel] = MAX (horizontal_max, vertical_max);
          local_min[pixel] = MIN (horizontal_min, vertical_min);
        }
    }
}

void
goodix_chicago_preprocessor_refine_envelopes (
  GoodixChicagoPreprocessor *self,
  const guint16              local_max[GOODIX_CHICAGO_PIXELS],
  const guint16              local_min[GOODIX_CHICAGO_PIXELS],
  guint16                    refined_min[GOODIX_CHICAGO_PIXELS],
  guint16                    refined_max[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (local_max != NULL);
  g_return_if_fail (local_min != NULL);
  g_return_if_fail (refined_min != NULL);
  g_return_if_fail (refined_max != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          guint16 minimum = local_max[pixel];
          guint16 maximum = local_min[pixel];

          for (gint dy = -1; dy <= 1; dy += 2)
            {
              const gint neighbor_y = (gint) y + dy;

              if (neighbor_y < 0 || neighbor_y >= CHICAGO_STAGE_HEIGHT)
                continue;
              for (gint dx = -1; dx <= 1; dx += 2)
                {
                  const gint neighbor_x = (gint) x + dx;
                  guint neighbor;

                  if (neighbor_x < 0 || neighbor_x >= CHICAGO_STAGE_WIDTH)
                    continue;
                  neighbor = neighbor_y * CHICAGO_STAGE_WIDTH + neighbor_x;
                  minimum = MIN (minimum, local_max[neighbor]);
                  maximum = MAX (maximum, local_min[neighbor]);
                }
            }

          refined_min[pixel] = minimum;
          refined_max[pixel] = maximum;
        }
    }
}

static void
build_candidate_internal (
  GoodixChicagoPreprocessor         *self,
  const guint16                      source[GOODIX_CHICAGO_PIXELS],
  const guint8                       mask[GOODIX_CHICAGO_PIXELS],
  guint8                             candidate[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoCandidateRejectStats *reject_stats)
{
  g_autofree guint16 *centered = NULL;
  g_autofree guint16 *local_max = NULL;
  g_autofree guint16 *local_min = NULL;
  g_autofree guint16 *refined_min = NULL;
  g_autofree guint16 *refined_max = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (source != NULL);
  g_return_if_fail (mask != NULL);
  g_return_if_fail (candidate != NULL);

  if (reject_stats)
    memset (reject_stats, 0, sizeof (*reject_stats));

  centered = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  local_max = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  local_min = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  refined_min = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  refined_max = g_new (guint16, GOODIX_CHICAGO_PIXELS);

  goodix_chicago_preprocessor_center_local_mean (self, source, mask, centered);
  goodix_chicago_preprocessor_smooth_centered (self, centered);
  goodix_chicago_preprocessor_build_local_envelopes (self, centered,
                                                     local_max, local_min);
  goodix_chicago_preprocessor_refine_envelopes (self, local_max, local_min,
                                                refined_min, refined_max);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      gint normalized;

      if (mask[pixel] == 0)
        {
          candidate[pixel] = 0xff;
          continue;
        }
      if (reject_stats && refined_min[pixel] - refined_max[pixel] <
          CHICAGO_REJECT_FLAT_RANGE)
        reject_stats->flat_count++;
      if (refined_min[pixel] == refined_max[pixel])
        normalized = 0xff;
      else
        normalized = ((gint) centered[pixel] - refined_max[pixel]) * 0xff /
                     (refined_min[pixel] - refined_max[pixel]);
      candidate[pixel] = 0xff - CLAMP (normalized, 0, 0xff);
    }

  /* Only materializes the contrast histogram when more than one
   * fifth of the full image is enabled and locally flat. The histogram itself
   * includes every enabled pixel and clamps envelope ranges at bin 199. */
  if (reject_stats &&
      reject_stats->flat_count > GOODIX_CHICAGO_PIXELS / 5)
    {
      reject_stats->active = TRUE;
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (mask[pixel] != 0)
          {
            guint range = refined_min[pixel] - refined_max[pixel];

            reject_stats->histogram[MIN (
                                      range, CHICAGO_REJECT_HISTOGRAM_BINS - 1)]++;
          }
    }
}

void
goodix_chicago_preprocessor_build_candidate (
  GoodixChicagoPreprocessor *self,
  const guint16              source[GOODIX_CHICAGO_PIXELS],
  const guint8               mask[GOODIX_CHICAGO_PIXELS],
  guint8                     candidate[GOODIX_CHICAGO_PIXELS])
{
  build_candidate_internal (self, source, mask, candidate, NULL);
}

/* Column-detrended source for the alternate
 * candidate. With more than 95 % of the pixels in the mask the
 * column mean covers all pixels; otherwise only masked pixels
 * are used and unmasked pixels become 5000. */
static void
build_alternate_source (
  const guint16 source[GOODIX_CHICAGO_PIXELS],
  const guint8  mask[GOODIX_CHICAGO_PIXELS],
  guint16       alternate[GOODIX_CHICAGO_PIXELS])
{
  guint masked = 0;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    masked += mask[pixel] != 0;

  for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
    {
      guint32 sum = 0;
      guint16 count = 0;
      guint16 mean = 0;

      if (masked * 100 / GOODIX_CHICAGO_PIXELS > 95)
        {
          for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
            sum += source[y * CHICAGO_STAGE_WIDTH + x];
          mean = sum / CHICAGO_STAGE_HEIGHT;

          for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
            {
              const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
              const gint value = (gint) source[pixel] - mean + 5000;

              alternate[pixel] = MAX (value, 0);
            }
          continue;
        }

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        if (mask[y * CHICAGO_STAGE_WIDTH + x] != 0)
          {
            sum += source[y * CHICAGO_STAGE_WIDTH + x];
            count++;
          }
      if (count != 0)
        mean = sum / count;
      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          const gint16 value = (gint16) (source[pixel] - mean + 5000);

          alternate[pixel] = mask[pixel] == 0 ? 5000 : value > 0 ? value : 0;
        }
    }
}

/* Column-axis variation of a candidate.  The mask selects real candidate bytes; disabled bytes
 * use the enabled-pixel mean. The returned value is the mean absolute change
 * between adjacent Q8 column means, normalized to the full image width. */
static gint
candidate_column_variation_score (
  const guint8 candidate[GOODIX_CHICAGO_PIXELS],
  const guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  gint column_mean_q8[CHICAGO_STAGE_WIDTH];
  guint32 enabled_sum = 0;
  guint enabled_count = 0;
  guint first = 0;
  guint last = 0;
  guint8 enabled_mean;
  guint64 variation = 0;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (mask[pixel] != 0)
      {
        enabled_sum += candidate[pixel];
        enabled_count++;
      }

  /* Deliberately divides by count + 1. */
  enabled_mean = enabled_sum / (enabled_count + 1);

  for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
    {
      gboolean enabled = FALSE;

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        enabled |= mask[y * CHICAGO_STAGE_WIDTH + x] != 0;
      if (enabled)
        {
          first = x;
          break;
        }
    }
  for (gint x = CHICAGO_STAGE_WIDTH - 1; x > 0; x--)
    {
      gboolean enabled = FALSE;

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        enabled |= mask[y * CHICAGO_STAGE_WIDTH + x] != 0;
      if (enabled)
        {
          last = x;
          break;
        }
    }

  for (guint x = first; x <= last; x++)
    {
      guint32 sum = 0;

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;

          sum += mask[pixel] != 0 ? candidate[pixel] : enabled_mean;
        }
      column_mean_q8[x] = (sum << 8) / CHICAGO_STAGE_HEIGHT;
    }
  for (guint x = first; x < last; x++)
    variation += ABS (column_mean_q8[x + 1] - column_mean_q8[x]);

  if (last != first)
    return (((CHICAGO_STAGE_WIDTH - 1) * variation) / (last - first)) >> 8;
  return variation;
}

static guint32
integer_sqrt_u64 (guint64 value)
{
  guint64 remainder = value;
  guint32 root = 0;
  guint32 bit = 0x80000000u;
  gint shift = 31;

  if (value <= 1)
    return value;

  while (bit != 0)
    {
      const guint64 trial = ((guint64) root * 2 + bit) << shift;

      shift--;
      if (remainder >= trial)
        {
          root += bit;
          remainder -= trial;
        }
      bit >>= 1;
    }
  return root;
}

/* Masked correlation of two candidates.  Disabled pixels become neutral gray 122;
 * enabled pixels retain the selected candidate byte. The result is a signed
 * Q8 Pearson correlation, with integer means and an integer square root. */
static gint
candidate_correlation_q8 (
  const guint8 primary[GOODIX_CHICAGO_PIXELS],
  const guint8 alternate[GOODIX_CHICAGO_PIXELS],
  const guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  guint32 primary_sum = 0;
  guint32 alternate_sum = 0;
  gint64 covariance = 0;
  guint64 primary_variance = 0;
  guint64 alternate_variance = 0;
  guint32 denominator;
  guint8 primary_mean;
  guint8 alternate_mean;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      primary_sum += mask[pixel] != 0 ? primary[pixel] : 122;
      alternate_sum += mask[pixel] != 0 ? alternate[pixel] : 122;
    }
  primary_mean = primary_sum / GOODIX_CHICAGO_PIXELS;
  alternate_mean = alternate_sum / GOODIX_CHICAGO_PIXELS;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint primary_value =
        (mask[pixel] != 0 ? primary[pixel] : 122) - primary_mean;
      const gint alternate_value =
        (mask[pixel] != 0 ? alternate[pixel] : 122) - alternate_mean;

      covariance += primary_value * alternate_value;
      primary_variance += primary_value * primary_value;
      alternate_variance += alternate_value * alternate_value;
    }

  denominator = integer_sqrt_u64 (primary_variance * alternate_variance);
  return denominator == 0 ? 0 : (covariance * 256) / denominator;
}

gboolean
goodix_chicago_preprocessor_select_alternate_mode24 (
  gint primary_score,
  gint alternate_score,
  gint correlation_q8,
  gint reference_score)
{
  const gint low = 2500;
  const gint high = 5000;
  gint threshold;

  if (primary_score < low)
    {
      threshold = (reference_score * 205) >> 8;
      return alternate_score < threshold && correlation_q8 < 235;
    }

  if (primary_score < high)
    {
      threshold = (((primary_score - low) * reference_score * 179) /
                   (high - low) + reference_score * 205) >> 8;
      if (alternate_score < threshold)
        return TRUE;
      return alternate_score * 75 < threshold * 100 &&
             correlation_q8 < 220;
    }

  if (primary_score >= high + low)
    return TRUE;

  threshold = (((primary_score - high) * reference_score * 2) /
               (high - low) + reference_score * 384) >> 8;
  if (alternate_score < threshold)
    return TRUE;
  return alternate_score * 80 < threshold * 100 && correlation_q8 < 200;
}

static void
build_enhanced_prepared_internal (
  GoodixChicagoPreprocessor         *self,
  const guint16                      current[GOODIX_CHICAGO_PIXELS],
  const guint16                      image_base[GOODIX_CHICAGO_PIXELS],
  gint                               purpose,
  guint8                             enhanced[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoCandidateRejectStats *reject_stats,
  guint8                            *primary_out,
  guint8                            *mask_out,
  guint16                           *auxiliary_source,
  gboolean                          *alternate_selected)
{
  guint16 source[GOODIX_CHICAGO_PIXELS];
  guint8 mask[GOODIX_CHICAGO_PIXELS];
  g_autofree guint16 *alternate_source = NULL;
  g_autofree guint8 *alternate = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (current != NULL);
  g_return_if_fail (image_base != NULL);
  g_return_if_fail (enhanced != NULL);

  normalize_frame (self, current, image_base, purpose, source, auxiliary_source);
  goodix_chicago_preprocessor_build_mask (
    self, current, image_base, mask);
  build_candidate_internal (self, source, mask, enhanced, reject_stats);
  if (primary_out)
    memcpy (primary_out, enhanced, GOODIX_CHICAGO_PIXELS);
  if (mask_out)
    memcpy (mask_out, mask, GOODIX_CHICAGO_PIXELS);

  /* ChicagoHS mode 24 always supplies selector flag +0x1c == 1, which bypasses
   * the generic component-count prefilter and enters this policy.
   * The reference score is 600. */
  alternate_source = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  alternate = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  build_alternate_source (source, mask, alternate_source);
  goodix_chicago_preprocessor_build_candidate (
    self, alternate_source, mask, alternate);
  if (goodix_chicago_preprocessor_select_alternate_mode24 (
        candidate_column_variation_score (enhanced, mask),
        candidate_column_variation_score (alternate, mask),
        candidate_correlation_q8 (enhanced, alternate, mask),
        600))
    {
      memcpy (enhanced, alternate, GOODIX_CHICAGO_PIXELS);
      if (alternate_selected)
        *alternate_selected = TRUE;
    }
}

static gboolean
candidate_reject_stats_is_poor_capture (
  GoodixChicagoCandidateRejectStats *stats)
{
  guint total = 0;
  guint cumulative = 0;
  guint lower_30 = CHICAGO_REJECT_HISTOGRAM_BINS - 1;
  guint upper_10 = 0;
  guint bin_199;

  if (!stats->active)
    return FALSE;

  for (guint bin = 0; bin < CHICAGO_REJECT_HISTOGRAM_BINS; bin++)
    total += stats->histogram[bin];
  if (total == 0)
    return FALSE;

  /* First lower-tail bin exceeding 30 percent. */
  for (guint bin = 0; bin < CHICAGO_REJECT_HISTOGRAM_BINS; bin++)
    {
      cumulative += stats->histogram[bin];
      if (cumulative * 100 > total * 30)
        {
          lower_30 = bin;
          break;
        }
    }

  /* The same routine records the first descending bin whose upper tail
   * exceeds ten percent. */
  cumulative = 0;
  for (gint bin = CHICAGO_REJECT_HISTOGRAM_BINS - 1; bin >= 0; bin--)
    {
      cumulative += stats->histogram[bin];
      if (cumulative > total / 10)
        {
          upper_10 = bin;
          break;
        }
    }

  bin_199 = stats->histogram[CHICAGO_REJECT_HISTOGRAM_BINS - 1];
  if (lower_30 < 20)
    return TRUE;
  if (upper_10 > lower_30 * 3 && lower_30 < 40)
    return TRUE;
  return bin_199 > total / 4 && lower_30 < 60;
}

/* Mode 24. The two-pixel interior excludes transport
 * borders and must contain strictly more than 50 percent non-saturated
 * samples. The bounds are also strict. */
static gboolean
mode24_raw_input_is_valid (
  const guint16 prepared[GOODIX_CHICAGO_PIXELS])
{
  guint valid = 0;
  guint total = 0;

  for (guint y = 2; y < CHICAGO_STAGE_HEIGHT - 2; y++)
    for (guint x = 2; x < CHICAGO_STAGE_WIDTH - 2; x++)
      {
        guint16 value = prepared[y * CHICAGO_STAGE_WIDTH + x];

        total++;
        if (value > 50 && value < 4050)
          valid++;
      }

  return valid * 100 > total * 50;
}

/* (mode 24, end of every frame): when the output is
 * nearly identical to the previous frame's output (best mean absolute
 * difference over +-2 pixel shifts of the 70x54 interior below 50), the kr
 * and framenum changes of this frame are undone. The gain, multiplier and
 * auxiliary planes keep their update. The output is then remembered. */
static void
rollback_repeated_output (GoodixChicagoPreprocessor *self,
                          const guint8               output[GOODIX_CHICAGO_PIXELS],
                          const guint16              saved_kr[GOODIX_CHICAGO_PIXELS],
                          guint32                    saved_framenum)
{
  if (self->framenum > 1)
    {
      gint best = 0xff * GOODIX_CHICAGO_PIXELS;

      for (gint dy = -2; dy <= 2; dy++)
        for (gint dx = -2; dx <= 2; dx++)
          {
            gint sum = 0;

            for (guint y = 5; y < 5 + 54; y++)
              for (guint x = 5; x < 5 + 70; x++)
                sum += ABS ((gint) self->previous_output[y * CHICAGO_STAGE_WIDTH + x] -
                            (gint) output[(y + dy) * CHICAGO_STAGE_WIDTH + x + dx]);
            best = MIN (best, sum);
          }
      if (best / (gint) ((CHICAGO_STAGE_HEIGHT - 10) * (CHICAGO_STAGE_WIDTH - 10)) < 50)
        {
          memcpy (self->kr, saved_kr, sizeof (self->kr));
          self->framenum = saved_framenum;
        }
    }
  memcpy (self->previous_output, output, sizeof (self->previous_output));
}

/* Marks the output in the least significant bits
 * of the first row. Columns 0..71 carry the alternating 0/1 signature (set
 * once), columns 72, 73, 75, 76 and 77 encode `code`; with `region` the LSB of
 * every pixel below the first row flags whether it lies outside `region`
 * (column 74 records that the region map is present). */
static void
mark_output (guint8        image[GOODIX_CHICAGO_PIXELS],
             const guint8 *region,
             gint          code,
             gboolean      with_region)
{
  const guint width = CHICAGO_STAGE_WIDTH;
  const guint signature = width - 8;
  gboolean marked = TRUE;
  guint8 *tail = image + width;

  if (code == 0)
    return;
  for (guint x = 0; x < signature; x += 2)
    if (image[x] & 1)
      {
        marked = FALSE;
        break;
      }
  if (marked)
    {
      for (guint x = 1; x < signature; x += 2)
        if (!(image[x] & 1))
          {
            marked = FALSE;
            break;
          }
    }
  if (!marked)
    {
      for (guint x = signature; x < width; x++)
        image[x] &= 0xfe;
      for (guint x = 0; x < signature; x += 2)
        image[x] &= 0xfe;
      for (guint x = 1; x < signature; x += 2)
        image[x] |= 1;
    }

#define SET(offset) (tail[-(offset)] |= 1)
#define CLR(offset) (tail[-(offset)] &= 0xfe)
  switch (code)
    {
    case 1: SET (8);
      CLR (7);
      break;

    case 2: SET (8);
      SET (7);
      break;

    case 3: CLR (8);
      SET (7);
      break;

    case 4: CLR (8);
      CLR (7);
      break;

    case 5: SET (5);
      CLR (4);
      CLR (3);
      break;

    case 6: CLR (5);
      SET (4);
      CLR (3);
      break;

    case 7: SET (5);
      SET (4);
      CLR (3);
      break;

    case 8: CLR (5);
      CLR (4);
      SET (3);
      break;

    case 9: SET (5);
      CLR (4);
      SET (3);
      break;

    default: CLR (8);
      CLR (7);
      CLR (5);
      CLR (4);
      CLR (3);
      break;
    }
#undef SET
#undef CLR

  if (!with_region)
    return;
  if (tail[-6] & 1)
    {
      for (guint pixel = width; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (region[pixel] > 0)
          image[pixel] &= 0xfe;
    }
  else
    {
      tail[-6] |= 1;
      for (guint pixel = width; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        image[pixel] = region[pixel] > 0 ? image[pixel] & 0xfe : image[pixel] | 1;
    }
}

/* Plain local-contrast image of a 16-bit plane
 * (11-sample cross envelopes, no centering), 0xff outside the mask. */
static void
local_contrast_image (GoodixChicagoPreprocessor *self,
                      const guint16              plane[GOODIX_CHICAGO_PIXELS],
                      const guint8               mask[GOODIX_CHICAGO_PIXELS],
                      guint8                     out[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint16 *local_max = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  g_autofree guint16 *local_min = g_new (guint16, GOODIX_CHICAGO_PIXELS);

  goodix_chicago_preprocessor_build_local_envelopes (self, plane, local_max, local_min);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      gint value;

      if (mask[pixel] == 0)
        value = 0;
      else if (local_max[pixel] == local_min[pixel])
        value = 0xff;
      else
        value = CLAMP (((gint) plane[pixel] - local_min[pixel]) * 0xff /
                       ((gint) local_max[pixel] - local_min[pixel]), 0, 0xff);
      out[pixel] = 0xff - value;
    }
}

/* Centered and smoothed plane stretched between its 5th and 85th percentile over the
 * mask, inverted; 0xff outside the mask. */
static void
stretch_candidate (GoodixChicagoPreprocessor *self,
                   const guint16              plane[GOODIX_CHICAGO_PIXELS],
                   const guint8               mask[GOODIX_CHICAGO_PIXELS],
                   guint8                     out[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint16 *centered = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  g_autofree guint32 *histogram = g_new0 (guint32, 5000);
  gint count = 0;
  gint high = 0;
  gint low = G_MAXINT32;
  gint32 cumulative;
  gint32 high4;
  gint32 low4;

  goodix_chicago_preprocessor_center_local_mean (self, plane, mask, centered);
  goodix_chicago_preprocessor_smooth_centered (self, centered);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (mask[pixel] != 0)
      {
        histogram[MIN (centered[pixel] >> 2, 4999)]++;
        count++;
      }
  cumulative = 0;
  for (gint bin = 4999; bin >= 0; bin--)
    {
      cumulative += histogram[bin];
      if (cumulative * 100 >= count * 15)
        {
          high = bin;
          break;
        }
    }
  cumulative = 0;
  for (gint bin = 0; bin < 5000; bin++)
    {
      cumulative += histogram[bin];
      if (cumulative * 100 >= count * 5)
        {
          low = bin;
          break;
        }
    }
  high4 = (gint32) ((guint32) high * 4);
  low4 = (gint32) ((guint32) low * 4);
  if (high4 - low4 < 100)
    high4 = low4 + 100;
  memset (out, 0, GOODIX_CHICAGO_PIXELS);
  if (low4 < high4)
    {
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (mask[pixel] != 0)
          {
            const gint value = ((gint) centered[pixel] - low4) * 0xff / (high4 - low4);

            out[pixel] = value < 0 ? 0 : value > 0xff ? 0xff : value;
          }
    }
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    out[pixel] = 0xff - out[pixel];
}

/* With {0, 1, 0, 20, 70, 20, 15}:
 * base quality over `mask`, the ring-label penalty below 20 % or a bonus of
 * 15 otherwise (only below quality 70), clamped to 0..100. */
static gint
region_quality (const guint8 image[GOODIX_CHICAGO_PIXELS],
                const guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  gint quality = compute_base_quality_annotated (image, mask, NULL);

  if (quality < 70)
    {
      const gint percent = compute_ring_label4_percent (image, mask);

      if (percent < 20)
        {
          const gint factor = (percent << 8) / 20;

          quality = (((quality * factor) >> 8) * factor) >> 8;
        }
      else
        {
          quality += 15;
        }
    }
  return CLAMP (quality, 0, 100);
}

/* Percentage of masked pixels; when more than 30 %
 * of them lack contrast in the stretched auxiliary candidate and that
 * low-contrast region has quality below 15, the region is returned instead
 * (with its percentage). */
static gint
capture_mask_score (GoodixChicagoPreprocessor *self,
                    const guint16              auxiliary_source[GOODIX_CHICAGO_PIXELS],
                    const guint8               mask[GOODIX_CHICAGO_PIXELS],
                    guint8                     region[GOODIX_CHICAGO_PIXELS],
                    gboolean                  *have_region)
{
  guint8 candidate[GOODIX_CHICAGO_PIXELS];
  guint8 contrast[GOODIX_CHICAGO_PIXELS];
  guint8 flat[GOODIX_CHICAGO_PIXELS];
  gint masked = 0;
  gint masked_flat = 0;

  *have_region = FALSE;
  stretch_candidate (self, auxiliary_source, mask, candidate);
  contrast_map_q16 (candidate, 80, contrast, 0xff);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      flat[pixel] = mask[pixel];
      if (mask[pixel] != 0)
        {
          masked++;
          if (contrast[pixel] == 0)
            masked_flat++;
        }
    }
  if (masked_flat * 100 > 30 * masked)
    {
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (flat[pixel] != 0 && contrast[pixel] != 0)
          flat[pixel] = 0;
      if (region_quality (candidate, flat) < 15)
        {
          memcpy (region, flat, GOODIX_CHICAGO_PIXELS);
          *have_region = TRUE;
        }
    }
  return (*have_region ? masked_flat : masked) * 100 / (gint) GOODIX_CHICAGO_PIXELS;
}

/* Rounded mean |a - b| over the pixels of `region`. */
static gint
mean_difference_in_region (const guint8 a[GOODIX_CHICAGO_PIXELS],
                           const guint8 b[GOODIX_CHICAGO_PIXELS],
                           const guint8 region[GOODIX_CHICAGO_PIXELS])
{
  gint sum = 0;
  gint count = 0;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (region[pixel] != 0)
      {
        sum += ABS ((gint) a[pixel] - (gint) b[pixel]);
        count++;
      }
  return count == 0 ? sum : (sum + (count >> 1)) / count;
}

static GoodixChicagoPreprocessStatus context_build (GoodixChicagoPreprocessor *self,
                                                    const guint16              current[GOODIX_CHICAGO_PIXELS],
                                                    const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                                    const guint8               mask[GOODIX_CHICAGO_PIXELS],
                                                    gint                       framenum,
                                                    gboolean                   hold,
                                                    guint8                     context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE]);

static GoodixChicagoPreprocessStatus
process_internal (GoodixChicagoPreprocessor *self,
                  const guint16              raw[GOODIX_CHICAGO_PIXELS],
                  gint                       purpose,
                  gboolean                   hold,
                  guint8                     enhanced[GOODIX_CHICAGO_PIXELS],
                  guint8                    *quality_out,
                  guint8                    *coverage_out)
{
  GoodixChicagoPreprocessStatus context_status;
  guint16 current[GOODIX_CHICAGO_PIXELS];
  guint16 image_base[GOODIX_CHICAGO_PIXELS];
  guint16 saved_kr[GOODIX_CHICAGO_PIXELS];
  guint16 auxiliary_source[GOODIX_CHICAGO_PIXELS];
  guint16 rerun_source[GOODIX_CHICAGO_PIXELS];
  guint8 primary[GOODIX_CHICAGO_PIXELS];
  guint8 mask[GOODIX_CHICAGO_PIXELS];
  guint8 region[GOODIX_CHICAGO_PIXELS];
  guint8 contrast[GOODIX_CHICAGO_PIXELS];
  const guint32 saved_framenum = self->framenum;
  GoodixChicagoCandidateRejectStats reject_stats;
  GoodixChicagoPreprocessStatus status = GOODIX_CHICAGO_PREPROCESS_STATUS_OK;
  gboolean have_region;
  gboolean alternate_selected = FALSE;
  gint mask_percent;
  gint difference;
  gint threshold;
  gint code = 0;
  guint8 quality;
  guint8 coverage;

  goodix_chicago_preprocessor_prepare_raw (self, raw, current);
  memset (self->context, 0, sizeof (self->context));
  if (!mode24_raw_input_is_valid (current))
    {
      memset (enhanced, 0, GOODIX_CHICAGO_PIXELS);
      if (quality_out)
        *quality_out = 0;
      if (coverage_out)
        *coverage_out = 0;
      return GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT;
    }
  memcpy (saved_kr, self->kr, sizeof (saved_kr));
  goodix_chicago_preprocessor_prepare_raw (
    self, self->image_base, image_base);
  build_enhanced_prepared_internal (
    self, current, image_base, purpose, enhanced, &reject_stats,
    primary, mask, auxiliary_source, &alternate_selected);
  /* alternate_selected: the capture policy then works on the
   * column-detrended auxiliary plane. */
  if (alternate_selected)
    {
      guint16 detrended[GOODIX_CHICAGO_PIXELS];

      build_alternate_source (auxiliary_source, mask, detrended);
      memcpy (auxiliary_source, detrended, sizeof (detrended));
    }

  /* Capture policy (mode 24). score1 is the mask
   * coverage in percent; the region is the contrast map of the primary
   * candidate. */
  mask_percent = capture_mask_score (self, auxiliary_source, mask, region, &have_region);
  if (!have_region)
    contrast_map_q16 (primary, 100, region, 0xff);
  local_contrast_image (self, auxiliary_source, mask, contrast);
  difference = MIN (mean_difference_in_region (enhanced, contrast, region), 200);
  threshold = 56;
  if (self->framenum < 100 && purpose == 1)
    threshold = 64;
  if (difference > threshold)
    {
      if (self->framenum < 100 && mask_percent > 90)
        {
          /* Re-run with framenum forced to 5 */
          const guint32 framenum = self->framenum;

          self->framenum = 5;
          normalize_frame (self, current, image_base, purpose, rerun_source, NULL);
          self->framenum = framenum;
        }
      code = 4;
      if (mask_percent > 40)
        {
          status = GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE;
          code = 2;
        }
    }
  /* Per-frame context (cbuf[0..5]); its class-9 verdict
   * replaces the status at the end. */
  context_status = context_build (self, current, image_base, mask,
                                  (gint) self->framenum, hold, self->context);
  mark_output (enhanced, region, code, have_region);

  compute_metrics (enhanced, &quality, &coverage);

  /* The poor-capture check runs only below quality 35 and only when
   * the capture policy did not already flag the frame. */
  if (code == 0 && quality < 35 && candidate_reject_stats_is_poor_capture (&reject_stats))
    {
      status = GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE;
      mark_output (enhanced, NULL, 1, FALSE);
    }
  /* No quality without coverage */
  if (coverage <= 5)
    quality = 0;
  if (context_status == GOODIX_CHICAGO_PREPROCESS_STATUS_CLASS_REJECT)
    status = context_status;
  rollback_repeated_output (self, enhanced, saved_kr, saved_framenum);
  if (quality_out)
    *quality_out = quality;
  if (coverage_out)
    *coverage_out = coverage;
  return status;
}

static void
expand_resolution_promotion_mask (
  const guint16 primary[GOODIX_CHICAGO_PIXELS],
  gint          threshold,
  guint8        mask[GOODIX_CHICAGO_PIXELS])
{
  guint queue[GOODIX_CHICAGO_PIXELS];

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    mask[pixel] = mask[pixel] != 0;

  for (guint y = 1; y + 1 < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 1; x + 1 < CHICAGO_STAGE_WIDTH; x++)
      {
        const guint seed = y * CHICAGO_STAGE_WIDTH + x;
        guint head = 0;
        guint tail = 0;

        if (mask[seed] != 1)
          continue;

        queue[tail++] = seed;
        while (head < tail)
          {
            const guint pixel = queue[head++];
            const guint pixel_x = pixel % CHICAGO_STAGE_WIDTH;
            const guint pixel_y = pixel / CHICAGO_STAGE_WIDTH;

            if (pixel_x == 0 || pixel_x + 1 >= CHICAGO_STAGE_WIDTH ||
                pixel_y == 0 || pixel_y + 1 >= CHICAGO_STAGE_HEIGHT)
              continue;

            for (gint dy = -1; dy <= 1; dy++)
              for (gint dx = -1; dx <= 1; dx++)
                {
                  const guint neighbor =
                    (pixel_y + dy) * CHICAGO_STAGE_WIDTH + pixel_x + dx;

                  if ((dx == 0 && dy == 0) || mask[neighbor] != 0)
                    continue;
                  if ((gint16) primary[neighbor] < threshold)
                    continue;

                  mask[neighbor] = 2;
                  queue[tail++] = neighbor;
                }
          }
      }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    mask[pixel] = mask[pixel] != 0 ? 0xff : 0;
}

void
goodix_chicago_preprocessor_build_resolution_base_plane (
  const guint16 current[GOODIX_CHICAGO_PIXELS],
  const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  guint16       resolution_base[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (current != NULL);
  g_return_if_fail (image_base != NULL);
  g_return_if_fail (resolution_base != NULL);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint value = (gint) image_base[pixel] - current[pixel] + 0x1bb7;

      resolution_base[pixel] = value < 0 ? 0 : (guint16) value;
    }
}

void
goodix_chicago_preprocessor_build_resolution_secondary_plane (
  const guint16 resolution_base[GOODIX_CHICAGO_PIXELS],
  guint16       secondary[GOODIX_CHICAGO_PIXELS])
{
  static const guint32 kernel[3] = { 0x1b44, 0xc978, 0x1b44 };
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];

  g_return_if_fail (resolution_base != NULL);
  g_return_if_fail (secondary != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dx = -1; dx <= 1; dx++)
          sum += (guint32) resolution_base[
            y * CHICAGO_STAGE_WIDTH +
            reflect_101 ((gint) x + dx, CHICAGO_STAGE_WIDTH)] *
                 kernel[dx + 1];
        horizontal[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dy = -1; dy <= 1; dy++)
          sum += (guint32) horizontal[
            reflect_101 ((gint) y + dy, CHICAGO_STAGE_HEIGHT) *
            CHICAGO_STAGE_WIDTH + x] * kernel[dy + 1];
        secondary[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }
}

guint
goodix_chicago_preprocessor_build_resolution_gradients (
  const guint16 secondary[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
  guint16       falling[GOODIX_CHICAGO_PIXELS],
  guint8        falling_directions[GOODIX_CHICAGO_PIXELS],
  guint16       rising[GOODIX_CHICAGO_PIXELS],
  guint8        rising_directions[GOODIX_CHICAGO_PIXELS])
{
  static const gint delta_x[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
  static const gint delta_y[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
  guint sum = 0;
  guint count = 1;

  g_return_val_if_fail (secondary != NULL, 0);
  g_return_val_if_fail (input_mask != NULL, 0);
  g_return_val_if_fail (falling != NULL, 0);
  g_return_val_if_fail (falling_directions != NULL, 0);
  g_return_val_if_fail (rising != NULL, 0);
  g_return_val_if_fail (rising_directions != NULL, 0);

  memset (falling, 0, GOODIX_CHICAGO_PIXELS * sizeof (*falling));
  memset (rising, 0, GOODIX_CHICAGO_PIXELS * sizeof (*rising));
  memset (falling_directions, 0xff, GOODIX_CHICAGO_PIXELS);
  memset (rising_directions, 0xff, GOODIX_CHICAGO_PIXELS);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        const guint pixel = y * CHICAGO_STAGE_WIDTH + x;

        if (input_mask[pixel] == 0)
          continue;
        for (guint direction = 0; direction < G_N_ELEMENTS (delta_x);
             direction++)
          {
            const gint neighbor_x = (gint) x + delta_x[direction];
            const gint neighbor_y = (gint) y + delta_y[direction];
            guint neighbor;
            gint difference;

            if (neighbor_x < 0 || neighbor_x >= CHICAGO_STAGE_WIDTH ||
                neighbor_y < 0 || neighbor_y >= CHICAGO_STAGE_HEIGHT)
              continue;
            neighbor = neighbor_y * CHICAGO_STAGE_WIDTH + neighbor_x;
            if (input_mask[neighbor] == 0)
              continue;

            difference = (gint) secondary[pixel] - secondary[neighbor];
            if (difference > falling[pixel])
              {
                falling[pixel] = difference;
                falling_directions[pixel] = direction;
              }
            difference = -difference;
            if (difference > rising[pixel])
              {
                rising[pixel] = difference;
                rising_directions[pixel] = direction;
              }
          }
        if (falling_directions[pixel] != 0xff)
          {
            sum += falling[pixel];
            count++;
          }
        if (rising_directions[pixel] != 0xff)
          {
            sum += rising[pixel];
            count++;
          }
      }

  return (sum + (count >> 1)) / count;
}

static void
filter_resolution_gradient_maximum (
  const guint16 falling[GOODIX_CHICAGO_PIXELS],
  const guint16 rising[GOODIX_CHICAGO_PIXELS],
  guint16       filtered[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint16 *maximum = g_new (guint16,
                                       GOODIX_CHICAGO_PIXELS);
  g_autofree guint16 *horizontal = g_new (guint16,
                                          GOODIX_CHICAGO_PIXELS);
  const guint32 coefficient = 0x5555;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    maximum[pixel] = MAX (falling[pixel], rising[pixel]);
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dx = -1; dx <= 1; dx++)
          sum += (guint32) maximum[
            y * CHICAGO_STAGE_WIDTH +
            reflect_101 ((gint) x + dx, CHICAGO_STAGE_WIDTH)] * coefficient;
        horizontal[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dy = -1; dy <= 1; dy++)
          sum += (guint32) horizontal[
            reflect_101 ((gint) y + dy, CHICAGO_STAGE_HEIGHT) *
            CHICAGO_STAGE_WIDTH + x] * coefficient;
        filtered[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
      }
}

void
goodix_chicago_preprocessor_build_resolution_filtered_gradient (
  const guint16 secondary[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
  guint16       filtered_gradient[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint16 *falling = NULL;
  g_autofree guint16 *rising = NULL;
  g_autofree guint8 *falling_directions = NULL;
  g_autofree guint8 *rising_directions = NULL;

  g_return_if_fail (secondary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (filtered_gradient != NULL);

  falling = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  rising = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  falling_directions = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  rising_directions = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  goodix_chicago_preprocessor_build_resolution_gradients (
    secondary, input_mask, falling, falling_directions,
    rising, rising_directions);
  filter_resolution_gradient_maximum (falling, rising, filtered_gradient);
}

void
goodix_chicago_preprocessor_build_resolution_primary_plane (
  const guint16 secondary[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
  gint          secondary_threshold,
  gint          gradient_threshold,
  gboolean      exceptional,
  guint16       primary[GOODIX_CHICAGO_PIXELS])
{
  static const gint offsets[8] = {
    -1, 1, -CHICAGO_STAGE_WIDTH, CHICAGO_STAGE_WIDTH,
    -1 - CHICAGO_STAGE_WIDTH, CHICAGO_STAGE_WIDTH - 1,
    1 - CHICAGO_STAGE_WIDTH, CHICAGO_STAGE_WIDTH + 1,
  };
  g_autofree guint16 *falling = NULL;
  g_autofree guint16 *rising = NULL;
  g_autofree guint8 *falling_directions = NULL;
  g_autofree guint8 *rising_directions = NULL;
  g_autofree guint16 *filtered = NULL;

  g_return_if_fail (secondary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (primary != NULL);

  falling = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  rising = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  falling_directions = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  rising_directions = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  filtered = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  goodix_chicago_preprocessor_build_resolution_gradients (
    secondary, input_mask, falling, falling_directions,
    rising, rising_directions);

  filter_resolution_gradient_maximum (falling, rising, filtered);

  memset (primary, 0, GOODIX_CHICAGO_PIXELS * sizeof (*primary));
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const guint16 *gradient = exceptional ? falling : rising;
      const guint8 *directions = exceptional ? falling_directions :
                                 rising_directions;
      const gboolean secondary_trigger = exceptional ?
                                         (gint16) secondary[pixel] > secondary_threshold :
                                         (gint16) secondary[pixel] < secondary_threshold;
      gint current = pixel;

      if (input_mask[pixel] == 0 ||
          (!secondary_trigger &&
           (gint16) filtered[pixel] <= gradient_threshold))
        continue;
      for (guint step = 0; step < 6; step++)
        {
          const guint direction = directions[current];

          if (direction == 0xff)
            break;
          primary[pixel] = (guint16) (primary[pixel] + gradient[current]);
          current += offsets[direction];
        }
    }
}

static void
calculate_resolution_histogram_statistics (
  const guint16 values[GOODIX_CHICAGO_PIXELS],
  const guint8  mask[GOODIX_CHICAGO_PIXELS],
  gint          lower,
  gboolean      include_tails,
  gint          statistics[34])
{
  gint histogram[400] = { 0, };
  gint quantiles[10] = { 0, };
  gint targets[10];
  gint minimum = 0x7fffff00;
  gint maximum = 0;
  gint valid = 0;
  gint range;
  gint64 cumulative = 0;
  guint quantile = 0;

  memset (statistics, 0, 34 * sizeof (*statistics));
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (mask[pixel] != 0)
      {
        const gint value = (gint16) values[pixel];

        statistics[32]++;
        if (value > G_MAXINT16)
          {
            statistics[30]++;
          }
        else if (value < lower)
          {
            statistics[31]++;
          }
        else
          {
            minimum = MIN (minimum, value);
            maximum = MAX (maximum, value);
            valid++;
          }
      }
  range = maximum - minimum;
  statistics[33] = valid;
  if (range < 1)
    {
      range = 0;
      minimum = MIN (minimum, 5000);
      goto statistics_ready;
    }
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (mask[pixel] != 0)
      {
        const gint value = (gint16) values[pixel];

        if (value >= lower && value <= G_MAXINT16)
          {
            const gint numerator = (value - minimum) * 399;
            const gint bin = numerator / range;
            const gint remainder = numerator - bin * range;

            histogram[bin] += range - remainder;
            if (bin < 399)
              histogram[bin + 1] += remainder;
          }
      }
  for (guint index = 0; index < G_N_ELEMENTS (targets); index++)
    targets[index] = (gint) (((gint64) (index + 1) * 10 * valid * range) /
                             100);
  for (gint bin = 0; bin < 399 && quantile < G_N_ELEMENTS (quantiles); bin++)
    {
      cumulative += histogram[bin];
      if (cumulative < targets[quantile])
        {
          if (targets[quantile] <= cumulative + histogram[bin + 1])
            quantiles[quantile++] = bin;
        }
      else
        {
          quantiles[quantile++] = MAX (bin - 1, 0);
        }
    }
  for (guint index = 1; index < G_N_ELEMENTS (quantiles); index++)
    quantiles[index] = MAX (quantiles[index], quantiles[index - 1]);

statistics_ready:
#define SCALE_BIN(bin) (((bin) * range + 200) / 399 + minimum)
  statistics[5] = SCALE_BIN (quantiles[4]);
  statistics[7] = SCALE_BIN (quantiles[8]);
  statistics[8] = SCALE_BIN (quantiles[7]);
  statistics[12] = SCALE_BIN (quantiles[1]);
  statistics[13] = SCALE_BIN (quantiles[2]);
  {
    gint64 count = 0;
    gint64 weighted = 0;

    for (gint bin = quantiles[2]; bin <= quantiles[6]; bin++)
      {
        count += histogram[bin];
        weighted += (gint64) histogram[bin] * bin;
      }
    statistics[6] = SCALE_BIN ((gint) (weighted / (count + 1)));
  }
  if (include_tails)
    {
      gint64 count = 0;
      gint64 weighted = 0;

      for (gint bin = 399; bin >= quantiles[8]; bin--)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      statistics[17] = SCALE_BIN ((gint) (weighted / (count + 1)));
      for (gint bin = quantiles[8] - 1; bin >= quantiles[7]; bin--)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      statistics[18] = SCALE_BIN ((gint) (weighted / (count + 1)));

      count = 0;
      weighted = 0;
      for (gint bin = 0; bin <= quantiles[0]; bin++)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      statistics[22] = SCALE_BIN ((gint) (weighted / (count + 1)));
      for (gint bin = quantiles[0] + 1; bin <= quantiles[1]; bin++)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      statistics[23] = SCALE_BIN ((gint) (weighted / (count + 1)));
    }
  statistics[28] = range;
  statistics[29] = minimum;
#undef SCALE_BIN
}

gint
goodix_chicago_preprocessor_calculate_resolution_gradient_threshold (
  const guint16 filtered_gradient[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS])
{
  gint statistics[34];
  gint center;
  gint threshold;

  g_return_val_if_fail (filtered_gradient != NULL, 0);
  g_return_val_if_fail (input_mask != NULL, 0);

  calculate_resolution_histogram_statistics (
    filtered_gradient, input_mask, 1, FALSE, statistics);
  center = (statistics[5] + statistics[6]) >> 1;
  threshold = MAX ((statistics[8] - center) * 40 / 100, 30) + center;
  return CLAMP (threshold, 150, 300);
}

void
goodix_chicago_preprocessor_calculate_resolution_primary_statistics (
  const guint16                      primary[GOODIX_CHICAGO_PIXELS],
  const guint8                       input_mask[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoResolutionStatistics *statistics)
{
  gint raw_statistics[34];

  g_return_if_fail (primary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (statistics != NULL);

  calculate_resolution_histogram_statistics (
    primary, input_mask, 2, TRUE, raw_statistics);
  statistics->primary_center_a = raw_statistics[5];
  statistics->primary_center_b = raw_statistics[6];
  statistics->primary_high_sample = raw_statistics[7];
  statistics->primary_mid_sample = raw_statistics[8];
}

static gint
resolution_histogram_otsu (const gint histogram[400])
{
  gint normalized[400];
  gint64 total = 0;
  gint64 total_weight = 0;
  gint64 total_index = 0;
  gint64 prefix_weight = 0;
  gint64 prefix_index = 0;
  gint64 best_score = -1;
  gint best_index = 399;

  for (guint index = 0; index < 400; index++)
    total += histogram[index];
  if (total == 0)
    return 0;
  for (guint index = 0; index < 400; index++)
    {
      normalized[index] = ((gint64) histogram[index] << 16) / total;
      total_weight += normalized[index];
      total_index += (gint64) normalized[index] * index;
    }
  for (gint index = 0; index < 399; index++)
    {
      gint64 difference;
      gint64 score;

      prefix_weight += normalized[index];
      prefix_index += (gint64) normalized[index] * index;
      if (prefix_weight == 0 || prefix_weight == total_weight)
        continue;
      difference = (prefix_index << 16) / prefix_weight -
                   ((total_index - prefix_index) << 16) /
                   (total_weight - prefix_weight);
      score = (((total_weight - prefix_weight) * prefix_weight) >> 16) *
              ((difference * difference) >> 16);
      if (score >= best_score)
        {
          best_score = score;
          best_index = index;
        }
    }
  return best_index;
}

static gint
analyze_resolution_histogram_peaks (const gint histogram[400],
                                    gint      *peak_index)
{
  static const gint neighbor_offsets[20] = {
    -10, 10, -9, 9, -8, 8, -7, 7, -6, 6,
    -5, 5, -4, 4, -3, 3, -2, 2, -1, 1,
  };
  gint raw[400];
  gint smoothed[400];
  gint temporary[400];
  gint window_sums[400] = { 0, };
  gint maximum_index = -1;
  gint maximum = 0;
  gint low_index = 200;
  gint high_index = 200;
  gint state = 0;

  for (guint index = 0; index < 400; index++)
    {
      raw[index] = histogram[index] >> 4;
      smoothed[index] = raw[index];
    }
  for (guint pass = 0; pass < 2; pass++)
    {
      memcpy (temporary, smoothed, sizeof (temporary));
      for (gint index = 10; index < 390; index++)
        {
          gint sum = 0;

          for (gint offset = -10; offset <= 10; offset++)
            sum += smoothed[index + offset];
          window_sums[index] = sum;
          temporary[index] = (sum + 10) / 21;
        }
      memcpy (smoothed, temporary, sizeof (smoothed));
    }
  for (gint index = 0; index < 10; index++)
    smoothed[index] = smoothed[10];
  for (gint index = 390; index < 400; index++)
    smoothed[index] = smoothed[389];
  for (gint index = 0; index < 400; index++)
    if (window_sums[index] > maximum)
      {
        maximum = window_sums[index];
        maximum_index = index;
      }
  *peak_index = resolution_histogram_otsu (smoothed);
  if (maximum_index < 0)
    return 0;

  /* The two lobes are located on the smoothed histogram, the
   * ratio below uses the window sums at those positions. */
  for (gint index = 201; index < 390; index++)
    if (smoothed[index] > smoothed[high_index])
      high_index = index;
  for (gint index = 199; index >= 10; index--)
    if (smoothed[index] > smoothed[low_index])
      low_index = index;
  {
    const gint ratio = ((window_sums[low_index] + 1) * 100) /
                       (window_sums[high_index] + 1);

    if (high_index - low_index > 160 && ratio >= 31 && ratio <= 332)
      state = 1;
  }

  for (gint candidate = 10; candidate < 390; candidate++)
    {
      gboolean local_maximum = TRUE;
      gint adjusted;
      gint valley = maximum;

      for (guint offset = 0; offset < G_N_ELEMENTS (neighbor_offsets); offset++)
        if (smoothed[candidate + neighbor_offsets[offset]] >
            smoothed[candidate])
          {
            local_maximum = FALSE;
            break;
          }
      if (!local_maximum)
        continue;
      adjusted = smoothed[candidate] - smoothed[maximum_index] / 150;
      if (smoothed[candidate + 10] > adjusted ||
          smoothed[candidate - 10] > adjusted ||
          window_sums[candidate] * 100 < maximum * 30)
        {
          candidate += 10;
          continue;
        }
      if (candidate < maximum_index)
        for (gint index = candidate; index < maximum_index; index++)
          valley = MIN (valley, window_sums[index]);
      else
        for (gint index = maximum_index; index < candidate; index++)
          valley = MIN (valley, window_sums[index]);
      if (valley * 100 > window_sums[candidate] * 65)
        {
          candidate += 10;
          continue;
        }
      if (valley * 100 <= maximum * 40)
        {
          state = 2;
          break;
        }
      candidate += 10;
    }
  return state;
}

void
goodix_chicago_preprocessor_calculate_resolution_secondary_analysis (
  const guint16                             secondary[GOODIX_CHICAGO_PIXELS],
  const guint8                              input_mask[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoResolutionSecondaryAnalysis *analysis)
{
  gint histogram[400] = { 0, };
  gint quantiles[20] = { 0, };
  gint targets[20];
  gint lower_quantiles[5];
  gint lower_means[5];
  gint upper_quantiles[5];
  gint upper_means[5];
  gint minimum = G_MAXINT;
  gint maximum = G_MININT;
  gint valid = 0;
  gint range;
  gint64 cumulative = 0;
  guint quantile = 0;
  gint peak_index;
  gint median;
  gint central_mean;
  gint center;

  g_return_if_fail (secondary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (analysis != NULL);
  memset (analysis, 0, sizeof (*analysis));

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (input_mask[pixel] != 0)
      {
        const gint value = (gint16) secondary[pixel];

        if (value >= 5250 && value <= 9150)
          {
            minimum = MIN (minimum, value);
            maximum = MAX (maximum, value);
            valid++;
          }
      }
  range = valid ? maximum - minimum : 0;
  if (valid == 0 || range < 1)
    return;
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    if (input_mask[pixel] != 0)
      {
        const gint value = (gint16) secondary[pixel];

        if (value >= 5250 && value <= 9150)
          {
            const gint numerator = (value - minimum) * 399;
            const gint bin = numerator / range;
            const gint remainder = numerator - bin * range;

            histogram[bin] += range - remainder;
            if (bin < 399)
              histogram[bin + 1] += remainder;
          }
      }
  for (guint index = 0; index < G_N_ELEMENTS (targets); index++)
    targets[index] = (gint) (((gint64) (index + 1) * 5 * valid * range) /
                             100);
  for (gint bin = 0; bin < 399 && quantile < G_N_ELEMENTS (quantiles); bin++)
    {
      cumulative += histogram[bin];
      if (cumulative < targets[quantile])
        {
          if (targets[quantile] <= cumulative + histogram[bin + 1])
            quantiles[quantile++] = bin;
        }
      else
        {
          quantiles[quantile++] = MAX (bin - 1, 0);
        }
    }
  for (guint index = 1; index < G_N_ELEMENTS (quantiles); index++)
    quantiles[index] = MAX (quantiles[index], quantiles[index - 1]);

#define SCALE_SECONDARY_BIN(bin) (((bin) * range + 200) / 399 + minimum)
  for (guint index = 0; index < 5; index++)
    {
      gint64 count = 0;
      gint64 weighted = 0;
      const gint lower_bin = quantiles[index];
      const gint upper_bin = quantiles[18 - index];

      lower_quantiles[index] = SCALE_SECONDARY_BIN (lower_bin);
      upper_quantiles[index] = SCALE_SECONDARY_BIN (upper_bin);
      for (gint bin = 0; bin < lower_bin; bin++)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      lower_means[index] = SCALE_SECONDARY_BIN (
        (gint) (weighted / (count + 1)));
      count = 0;
      weighted = 0;
      for (gint bin = upper_bin; bin < 400; bin++)
        {
          count += histogram[bin];
          weighted += (gint64) histogram[bin] * bin;
        }
      upper_means[index] = SCALE_SECONDARY_BIN (
        (gint) (weighted / (count + 1)));
    }
  median = SCALE_SECONDARY_BIN (quantiles[9]);
  {
    gint64 count = 0;
    gint64 weighted = 0;

    for (gint bin = quantiles[6]; bin < quantiles[12]; bin++)
      {
        count += histogram[bin];
        weighted += (gint64) histogram[bin] * bin;
      }
    central_mean = SCALE_SECONDARY_BIN (
      (gint) (weighted / (count + 1)));
  }
  center = (median + central_mean) >> 1;
  analysis->lower_cutoff = -1;
  for (guint index = 0; index < 5; index++)
    {
      const gint gap = center - lower_means[index];

      if (index == 0 && gap < 400)
        break;
      if (index > 0 && gap < 400)
        {
          const gint previous_gap = center - lower_means[index - 1];

          analysis->lower_state = index;
          analysis->lower_cutoff = lower_quantiles[index - 1] +
                                   ((lower_quantiles[index] - lower_quantiles[index - 1]) *
                                    (previous_gap - 400)) /
                                   (previous_gap - gap + 1);
          break;
        }
      if (index == 4)
        {
          analysis->lower_state = 5;
          analysis->lower_cutoff = lower_quantiles[4] +
                                   ((center - lower_quantiles[4]) * (gap - 400)) / (gap + 1);
          analysis->lower_cutoff = MIN (analysis->lower_cutoff, center - 400);
        }
    }
  analysis->upper_cutoff = -1;
  for (guint index = 0; index < 5; index++)
    {
      const gint gap = upper_means[index] - center;

      if (index == 0 && gap < 400)
        break;
      if (index > 0 && gap < 400)
        {
          const gint previous_gap = upper_means[index - 1] - center;

          analysis->upper_state = index;
          analysis->upper_cutoff = upper_quantiles[index] +
                                   ((upper_quantiles[index - 1] - upper_quantiles[index]) *
                                    (400 - gap)) /
                                   (previous_gap - gap + 1);
          break;
        }
      if (index == 4)
        {
          analysis->upper_state = 5;
          analysis->upper_cutoff = center +
                                   ((upper_quantiles[4] - center) * 400) /
                                   (upper_quantiles[4] - center + 1);
          analysis->upper_cutoff = MAX (analysis->upper_cutoff, center + 400);
        }
    }
  analysis->upper_mid_sample = upper_quantiles[3];
  analysis->balance_center = central_mean;
  analysis->upper_outer_mean = upper_means[0];
  analysis->upper_inner_mean = upper_means[1];
  analysis->lower_outer_mean = lower_means[0];
  analysis->lower_inner_mean = lower_means[1];
  analysis->upper_innermost_mean = upper_means[4];
  analysis->peak_state = analyze_resolution_histogram_peaks (
    histogram, &peak_index);
  analysis->peak_value = SCALE_SECONDARY_BIN (peak_index);
#undef SCALE_SECONDARY_BIN
}

void
goodix_chicago_preprocessor_select_resolution_branches (
  const GoodixChicagoResolutionSecondaryAnalysis *analysis,
  const GoodixChicagoResolutionStatistics        *gradient_statistics,
  gboolean                                       *use_exceptional,
  gboolean                                       *use_normal,
  gint                                           *branch_state)
{
  gint left;
  gint right;

  g_return_if_fail (analysis != NULL);
  g_return_if_fail (gradient_statistics != NULL);
  g_return_if_fail (use_exceptional != NULL);
  g_return_if_fail (use_normal != NULL);
  g_return_if_fail (branch_state != NULL);

  *use_exceptional = analysis->upper_cutoff > 0 &&
                     analysis->upper_state > 0;
  *use_normal = analysis->lower_cutoff > 0 && analysis->lower_state > 0;
  *branch_state = 0;

  if ((*use_exceptional || *use_normal) &&
      (gradient_statistics->primary_high_sample -
       gradient_statistics->primary_center_a < 40 ||
       (gradient_statistics->primary_mid_sample -
        gradient_statistics->primary_center_a < 30 &&
        gradient_statistics->primary_high_sample -
        gradient_statistics->primary_center_a < 50) ||
       gradient_statistics->primary_high_sample < 100 ||
       (gradient_statistics->primary_mid_sample < 90 &&
        gradient_statistics->primary_high_sample < 120)))
    {
      *use_exceptional = FALSE;
      *use_normal = FALSE;
      return;
    }

  if (*use_exceptional)
    {
      if (!*use_normal)
        {
          *branch_state = 1;
          return;
        }

      left = analysis->balance_center - analysis->lower_outer_mean;
      right = analysis->upper_outer_mean - analysis->balance_center;
      if (ABS (left - right) > 100)
        {
          *branch_state = (left > right) + 1;
          return;
        }
      left = analysis->balance_center - analysis->lower_inner_mean;
      right = analysis->upper_inner_mean - analysis->balance_center;
      if (ABS (left - right) > 80)
        *branch_state = (left > right) + 1;
      return;
    }

  if (*use_normal)
    *branch_state = 2;
}

void
goodix_chicago_preprocessor_calculate_resolution_thresholds (
  const GoodixChicagoResolutionStatistics *statistics,
  gboolean                                 exceptional,
  GoodixChicagoResolutionThresholds       *thresholds)
{
  gint center;
  gint mid_delta;

  g_return_if_fail (statistics != NULL);
  g_return_if_fail (thresholds != NULL);

  memset (thresholds, 0, sizeof (*thresholds));
  center = (statistics->primary_center_a +
            statistics->primary_center_b) >> 1;
  thresholds->primary_high =
    ((statistics->primary_high_sample - center) * 100) / 100 + center;
  if ((!exceptional && statistics->branch_state == 1) ||
      (exceptional && statistics->branch_state == 2))
    thresholds->primary_high += 60;
  thresholds->primary_high = MAX (thresholds->primary_high, 600);

  mid_delta = ((statistics->primary_mid_sample - center) * 50) / 100;
  mid_delta = MAX (mid_delta, 50);
  thresholds->primary_mid = CLAMP (center + mid_delta, 250, 750);
  thresholds->primary_low = exceptional ?
                            MIN (center, thresholds->primary_mid) :
                            MIN (center + 50, thresholds->primary_mid);
  thresholds->primary_low = MAX (thresholds->primary_low, 250);
  thresholds->promotion = MAX (thresholds->primary_mid,
                               thresholds->primary_high - 60);

  if (exceptional)
    {
      thresholds->secondary_hard = 9150;
      thresholds->secondary_mid =
        MAX (statistics->secondary_mid_sample,
             statistics->secondary_base + 120);
    }
  else
    {
      thresholds->secondary_hard = 5250;
    }
}

guint
goodix_chicago_preprocessor_build_resolution_labels (
  const guint16                            primary[GOODIX_CHICAGO_PIXELS],
  const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
  const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
  const GoodixChicagoResolutionThresholds *thresholds,
  guint8                                   labels[GOODIX_CHICAGO_PIXELS],
  guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS])
{
  guint class3_seeds = 0;

  g_return_val_if_fail (primary != NULL, 0);
  g_return_val_if_fail (secondary != NULL, 0);
  g_return_val_if_fail (input_mask != NULL, 0);
  g_return_val_if_fail (thresholds != NULL, 0);
  g_return_val_if_fail (labels != NULL, 0);
  g_return_val_if_fail (promotion_mask != NULL, 0);

  memset (promotion_mask, 0, GOODIX_CHICAGO_PIXELS);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint primary_value = (gint16) primary[pixel];
      const gint secondary_value = (gint16) secondary[pixel];

      if (input_mask[pixel] == 0)
        continue;
      if (secondary_value < thresholds->secondary_hard ||
          primary_value > thresholds->primary_high)
        {
          labels[pixel] = 3;
          promotion_mask[pixel] = 0xff;
          class3_seeds++;
        }
      else if (labels[pixel] == 0)
        {
          if (primary_value > thresholds->primary_mid)
            labels[pixel] = 2;
          else if (primary_value > thresholds->primary_low)
            labels[pixel] = 1;
        }
    }

  if (class3_seeds > 50)
    {
      expand_resolution_promotion_mask (primary, thresholds->promotion,
                                        promotion_mask);
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (promotion_mask[pixel] != 0)
          labels[pixel] = 3;
    }

  return class3_seeds;
}

guint
goodix_chicago_preprocessor_build_exceptional_resolution_labels (
  const guint16                            primary[GOODIX_CHICAGO_PIXELS],
  const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
  const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
  const GoodixChicagoResolutionThresholds *thresholds,
  guint8                                   labels[GOODIX_CHICAGO_PIXELS],
  guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS])
{
  guint class3_seeds = 0;

  g_return_val_if_fail (primary != NULL, 0);
  g_return_val_if_fail (secondary != NULL, 0);
  g_return_val_if_fail (input_mask != NULL, 0);
  g_return_val_if_fail (thresholds != NULL, 0);
  g_return_val_if_fail (labels != NULL, 0);
  g_return_val_if_fail (promotion_mask != NULL, 0);

  memset (promotion_mask, 0, GOODIX_CHICAGO_PIXELS);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gint primary_value = (gint16) primary[pixel];
      const gint secondary_value = (gint16) secondary[pixel];

      if (input_mask[pixel] == 0)
        continue;
      if (secondary_value > thresholds->secondary_hard ||
          primary_value > thresholds->primary_high)
        {
          labels[pixel] = 3;
          promotion_mask[pixel] = 0xff;
          class3_seeds++;
        }
      else if (primary_value > thresholds->primary_mid)
        {
          if (secondary_value > thresholds->secondary_mid)
            {
              labels[pixel] = 3;
              class3_seeds++;
            }
          else
            {
              labels[pixel] = 2;
            }
        }
      else if (primary_value > thresholds->primary_low)
        {
          labels[pixel] = 1;
        }
    }

  if (class3_seeds > 50)
    {
      expand_resolution_promotion_mask (primary, thresholds->promotion,
                                        promotion_mask);
      for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
        if (promotion_mask[pixel] != 0)
          labels[pixel] = 3;
    }

  return class3_seeds;
}

static void build_resolution_map_labels_analysis (const guint16                             secondary[GOODIX_CHICAGO_PIXELS],
                                                  const guint8                              input_mask[GOODIX_CHICAGO_PIXELS],
                                                  guint8                                    labels[GOODIX_CHICAGO_PIXELS],
                                                  GoodixChicagoResolutionSecondaryAnalysis *analysis_out,
                                                  guint16                                  *filtered_gradient_out);

/* The classifier part of the resolution map.  The secondary-plane analysis
 * and the filtered gradient (left in the input plane) are returned for the context stages that follow. */
static void
build_resolution_map_labels_analysis (
  const guint16                             secondary[GOODIX_CHICAGO_PIXELS],
  const guint8                              input_mask[GOODIX_CHICAGO_PIXELS],
  guint8                                    labels[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoResolutionSecondaryAnalysis *analysis_out,
  guint16                                  *filtered_gradient_out)
{
  g_autofree guint16 *filtered_gradient = NULL;
  g_autofree guint16 *primary = NULL;
  g_autofree guint8 *promotion_mask = NULL;
  GoodixChicagoResolutionSecondaryAnalysis analysis = { 0, };
  GoodixChicagoResolutionStatistics gradient_statistics = { 0, };
  GoodixChicagoResolutionStatistics primary_statistics = { 0, };
  GoodixChicagoResolutionThresholds thresholds = { 0, };
  gboolean use_exceptional;
  gboolean use_normal;
  gint branch_state;
  gint gradient_threshold;

  filtered_gradient = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  primary = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  promotion_mask = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  memset (labels, 0, GOODIX_CHICAGO_PIXELS);

  goodix_chicago_preprocessor_calculate_resolution_secondary_analysis (
    secondary, input_mask, &analysis);
  *analysis_out = analysis;
  goodix_chicago_preprocessor_build_resolution_filtered_gradient (
    secondary, input_mask, filtered_gradient);
  if (filtered_gradient_out)
    memcpy (filtered_gradient_out, filtered_gradient, GOODIX_CHICAGO_PIXELS * sizeof (guint16));
  goodix_chicago_preprocessor_calculate_resolution_primary_statistics (
    filtered_gradient, input_mask, &gradient_statistics);
  goodix_chicago_preprocessor_select_resolution_branches (
    &analysis, &gradient_statistics,
    &use_exceptional, &use_normal, &branch_state);
  gradient_threshold =
    goodix_chicago_preprocessor_calculate_resolution_gradient_threshold (
      filtered_gradient, input_mask);

  if (use_exceptional)
    {
      goodix_chicago_preprocessor_build_resolution_primary_plane (
        secondary, input_mask, analysis.upper_cutoff, gradient_threshold,
        TRUE, primary);
      goodix_chicago_preprocessor_calculate_resolution_primary_statistics (
        primary, input_mask, &primary_statistics);
      primary_statistics.branch_state = branch_state;
      primary_statistics.secondary_base = analysis.upper_cutoff;
      primary_statistics.secondary_mid_sample = analysis.upper_mid_sample;
      goodix_chicago_preprocessor_calculate_resolution_thresholds (
        &primary_statistics, TRUE, &thresholds);
      goodix_chicago_preprocessor_build_exceptional_resolution_labels (
        primary, secondary, input_mask, &thresholds,
        labels, promotion_mask);
    }

  if (use_normal)
    {
      memset (&primary_statistics, 0, sizeof (primary_statistics));
      goodix_chicago_preprocessor_build_resolution_primary_plane (
        secondary, input_mask, analysis.lower_cutoff, gradient_threshold,
        FALSE, primary);
      goodix_chicago_preprocessor_calculate_resolution_primary_statistics (
        primary, input_mask, &primary_statistics);
      primary_statistics.branch_state = branch_state;
      primary_statistics.secondary_base = analysis.lower_cutoff;
      primary_statistics.secondary_mid_sample = analysis.upper_mid_sample;
      goodix_chicago_preprocessor_calculate_resolution_thresholds (
        &primary_statistics, FALSE, &thresholds);
      goodix_chicago_preprocessor_build_resolution_labels (
        primary, secondary, input_mask, &thresholds,
        labels, promotion_mask);
    }
}

guint
goodix_chicago_preprocessor_classify_resolution_labels (
  guint     mode,
  guint8    labels[GOODIX_CHICAGO_PIXELS],
  guint     pixel_count,
  guint     valid_pixel_count,
  guint    *code_out,
  gboolean *auxiliary_out)
{
  guint class1_and_3 = 0;
  guint class2 = 0;
  guint class3 = 0;
  guint percentage = 15;
  guint class3_limit = 300;
  guint code = 0;
  gboolean auxiliary = FALSE;

  g_return_val_if_fail (labels != NULL, 0);
  g_return_val_if_fail (pixel_count <= GOODIX_CHICAGO_PIXELS, 0);

  for (guint pixel = 0; pixel < pixel_count; pixel++)
    switch (labels[pixel])
      {
      case 1:
        class1_and_3++;
        labels[pixel] = 0;
        break;

      case 2:
        class2++;
        labels[pixel] = 0;
        break;

      case 3:
        class3++;
        break;

      default:
        break;
      }
  class1_and_3 += class3;

  if (mode == 0x18 || mode == 0x1a)
    {
      percentage = 20;
      class3_limit = 770;
    }

  if ((guint64) class3 * 100u >
      (guint64) valid_pixel_count * percentage)
    {
      code = 8;
      auxiliary = TRUE;
    }
  else if (class3 > class3_limit)
    {
      code = 7;
      auxiliary = TRUE;
    }
  else if ((guint64) (class1_and_3 + class2) * 100u >
           (guint64) valid_pixel_count * 25u)
    {
      code = 7;
      auxiliary = class3 > 150;
    }
  else if (class3 > 150)
    {
      code = 6;
      auxiliary = TRUE;
    }
  else if (class2 > 600 || class1_and_3 > 500)
    {
      code = 6;
    }
  else if (class2 > 300 || class1_and_3 > 300 ||
           class1_and_3 + class2 > 300)
    {
      code = 5;
    }

  if (code_out)
    *code_out = code;
  if (auxiliary_out)
    *auxiliary_out = auxiliary;
  return class3;
}

void
goodix_chicago_preprocessor_finalize_metrics (gint    base_quality,
                                              gint    coverage,
                                              guint8 *quality_out,
                                              guint8 *coverage_out)
{
  gint quality = base_quality > 0 ? base_quality + 7 : 0;

  if (coverage < 50)
    quality = quality * coverage * coverage / 2500;

  if (quality_out)
    *quality_out = CLAMP (quality, 0, 100);
  if (coverage_out)
    *coverage_out = CLAMP (coverage, 0, 100);
}

static guint
reflect_101 (gint  coordinate,
             guint length)
{
  while (coordinate < 0 || coordinate >= (gint) length)
    {
      if (coordinate < 0)
        coordinate = -coordinate;
      else
        coordinate = 2 * (gint) length - coordinate - 2;
    }

  return coordinate;
}

/* Fraction (Q16) of pixels whose smoothed gradient
 * local mean exceeds `threshold`; with `map` the pixels are also marked
 * (`value` above the threshold, 0 otherwise). */
static guint32
contrast_map_q16 (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                  guint        threshold,
                  guint8      *map,
                  guint8       value)
{
  static const guint32 gaussian[5] = {
    0x0df3, 0x3e84, 0x6712, 0x3e84, 0x0df3,
  };
  g_autofree guint16 *horizontal = NULL;
  g_autofree guint16 *smoothed = NULL;
  g_autofree gint16 *gradient_x = NULL;
  g_autofree gint16 *gradient_y = NULL;
  g_autofree guint16 *magnitude = NULL;
  guint active_pixels = 0;
  guint32 fixed_coverage;

  g_return_val_if_fail (enhanced != NULL, 0);

  horizontal = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  smoothed = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  gradient_x = g_new0 (gint16, GOODIX_CHICAGO_PIXELS);
  gradient_y = g_new0 (gint16, GOODIX_CHICAGO_PIXELS);
  magnitude = g_new (guint16, GOODIX_CHICAGO_PIXELS);

  /* Selector 7 is a separable five-tap Q16 Gaussian. Both passes
   * truncate independently and use reflect-101 borders. */
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          guint64 sum = 0;

          for (gint dx = -2; dx <= 2; dx++)
            {
              const guint source_x = reflect_101 ((gint) x + dx,
                                                  CHICAGO_STAGE_WIDTH);

              sum += ((guint32) enhanced[y * CHICAGO_STAGE_WIDTH + source_x] << 8) *
                     gaussian[dx + 2];
            }
          horizontal[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
        }
    }

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          guint64 sum = 0;

          for (gint dy = -2; dy <= 2; dy++)
            {
              const guint source_y = reflect_101 ((gint) y + dy,
                                                  CHICAGO_STAGE_HEIGHT);

              sum += (guint32) horizontal[source_y * CHICAGO_STAGE_WIDTH + x] *
                     gaussian[dy + 2];
            }
          smoothed[y * CHICAGO_STAGE_WIDTH + x] = sum >> 16;
        }
    }

  /* Sobel derivatives of the high byte. The derivative
   * axis is cleared at its two outer edges; the smoothing axis reflects. */
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      const guint previous_y = reflect_101 ((gint) y - 1,
                                            CHICAGO_STAGE_HEIGHT);
      const guint next_y = reflect_101 ((gint) y + 1,
                                        CHICAGO_STAGE_HEIGHT);

      for (guint x = 1; x + 1 < CHICAGO_STAGE_WIDTH; x++)
        {
          gint gradient;

          gradient = ((smoothed[previous_y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                      (smoothed[previous_y * CHICAGO_STAGE_WIDTH + x - 1] >> 8)) +
                     2 * ((smoothed[y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                          (smoothed[y * CHICAGO_STAGE_WIDTH + x - 1] >> 8)) +
                     ((smoothed[next_y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                      (smoothed[next_y * CHICAGO_STAGE_WIDTH + x - 1] >> 8));
          gradient_x[y * CHICAGO_STAGE_WIDTH + x] = gradient;
        }
    }

  for (guint y = 1; y + 1 < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint previous_x = reflect_101 ((gint) x - 1,
                                                CHICAGO_STAGE_WIDTH);
          const guint next_x = reflect_101 ((gint) x + 1,
                                            CHICAGO_STAGE_WIDTH);
          gint gradient;

          gradient = ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + previous_x] >> 8) -
                      (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + previous_x] >> 8)) +
                     2 * ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + x] >> 8) -
                          (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + x] >> 8)) +
                     ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + next_x] >> 8) -
                      (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + next_x] >> 8));
          gradient_y[y * CHICAGO_STAGE_WIDTH + x] = gradient;
        }
    }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    magnitude[pixel] = ABS ((gint) gradient_x[pixel]) / 2 +
                       ABS ((gint) gradient_y[pixel]) / 2;

  /* Averages a reflect-101 15x15 window using the truncated Q16
   * reciprocal floor(65536 / 225), then counts values above 120. */
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          guint32 sum = 0;
          guint16 local_mean;

          for (gint dy = -7; dy <= 7; dy++)
            {
              const guint source_y = reflect_101 ((gint) y + dy,
                                                  CHICAGO_STAGE_HEIGHT);

              for (gint dx = -7; dx <= 7; dx++)
                {
                  const guint source_x = reflect_101 ((gint) x + dx,
                                                      CHICAGO_STAGE_WIDTH);

                  sum += magnitude[source_y * CHICAGO_STAGE_WIDTH + source_x];
                }
            }

          local_mean = ((guint64) sum * (0x10000u / 225u)) >> 16;
          if (local_mean > threshold)
            active_pixels++;
          if (map)
            map[y * CHICAGO_STAGE_WIDTH + x] = local_mean > threshold ? value : 0;
        }
    }

  fixed_coverage = (active_pixels << 16) / GOODIX_CHICAGO_PIXELS;
  return fixed_coverage;
}

static guint32
compute_coverage_q16 (const guint8 enhanced[GOODIX_CHICAGO_PIXELS])
{
  return contrast_map_q16 (enhanced, 120, NULL, 0);
}

/* `annotated`, when given, receives the mask annotation: 0xff for
 * the enabled pixels of the 18x18 boxes around windows whose coherence percent
 * is valid and not above the median, 0 elsewhere. */
static gint
compute_base_quality_annotated (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                const guint8 quality_mask[GOODIX_CHICAGO_PIXELS],
                                guint8       annotated[GOODIX_CHICAGO_PIXELS])
{
  g_autofree gint32 *gradient_x = NULL;
  g_autofree gint32 *gradient_y = NULL;
  g_autofree gint32 *gradient_energy = NULL;
  gint coherence_sum = 0;
  guint valid_windows = 0;
  guint percent_histogram[101] = { 0 };
  gint window_percent[128];
  guint window_center[128];
  guint windows = 0;

  gradient_x = g_new0 (gint32, GOODIX_CHICAGO_PIXELS);
  gradient_y = g_new0 (gint32, GOODIX_CHICAGO_PIXELS);
  gradient_energy = g_new0 (gint32, GOODIX_CHICAGO_PIXELS);

  for (guint y = 1; y + 1 < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 1; x + 1 < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          gboolean enabled = TRUE;

          for (gint dy = -1; dy <= 1 && enabled; dy++)
            for (gint dx = -1; dx <= 1; dx++)
              if (quality_mask[(y + dy) * CHICAGO_STAGE_WIDTH + x + dx] == 0)
                {
                  enabled = FALSE;
                  break;
                }

          if (enabled)
            {
              const gint gx = enhanced[pixel + 1] - enhanced[pixel - 1];
              const gint gy = enhanced[pixel + CHICAGO_STAGE_WIDTH] -
                              enhanced[pixel - CHICAGO_STAGE_WIDTH];

              gradient_x[pixel] = gx;
              gradient_y[pixel] = gy;
              gradient_energy[pixel] = gx * gx + gy * gy;
            }
        }
    }

  for (guint center_y = 9; center_y < CHICAGO_STAGE_HEIGHT - 9;
       center_y += 6)
    {
      for (guint center_x = 9; center_x < CHICAGO_STAGE_WIDTH - 9;
           center_x += 6)
        {
          guint mask_count = 0;
          gint64 sum_xx = 0;
          gint64 sum_xy = 0;
          gint64 sum_yy = 0;
          guint gradient_count = 0;
          gint coherence = 0;

          if (quality_mask[center_y * CHICAGO_STAGE_WIDTH + center_x] == 0)
            continue;
          window_center[windows] = center_y * CHICAGO_STAGE_WIDTH + center_x;
          window_percent[windows++] = -1;

          for (guint y = center_y - 9; y <= center_y + 9; y++)
            {
              for (guint x = center_x - 9; x <= center_x + 9; x++)
                {
                  const guint pixel = y * CHICAGO_STAGE_WIDTH + x;

                  if (quality_mask[pixel] != 0)
                    mask_count++;
                }
            }

          if (mask_count < (19u * 19u) / 2)
            continue;

          for (guint y = center_y - 9; y <= center_y + 9; y++)
            {
              for (guint x = center_x - 9; x <= center_x + 9; x++)
                {
                  const guint pixel = y * CHICAGO_STAGE_WIDTH + x;

                  if (gradient_energy[pixel] >= 25)
                    {
                      sum_xx += gradient_x[pixel] * gradient_x[pixel];
                      sum_xy += gradient_x[pixel] * gradient_y[pixel];
                      sum_yy += gradient_y[pixel] * gradient_y[pixel];
                      gradient_count++;
                    }
                }
            }

          if (gradient_count >= (19u * 19u) / 6)
            {
              const gint64 average_xx =
                (gradient_count / 2 + sum_xx) / gradient_count;
              const gint64 average_xy =
                (gradient_count / 2 + sum_xy) / gradient_count;
              const gint64 average_yy =
                (gradient_count / 2 + sum_yy) / gradient_count;
              const gint64 mean_energy = (average_xx + average_yy) / 2;
              const gint64 determinant =
                average_xx * average_yy - average_xy * average_xy;
              const gint64 ratio =
                determinant * 0x10000 / (mean_energy * mean_energy + 1);

              coherence = MAX (0, 0x10000 - MAX ((gint64) 0, ratio));
            }

          coherence_sum += coherence;
          valid_windows++;
          window_percent[windows - 1] = (coherence * 100) >> 16;
          percent_histogram[window_percent[windows - 1]]++;
        }
    }

  if (annotated)
    {
      guint cumulative = 0;
      gint median = 0;

      for (gint percent = 0; percent <= 100; percent++)
        {
          cumulative += percent_histogram[percent];
          if (cumulative >= valid_windows / 2)
            {
              median = percent;
              break;
            }
        }
      memset (annotated, 0, GOODIX_CHICAGO_PIXELS);
      for (guint window = 0; window < windows; window++)
        {
          if (window_percent[window] > median || window_percent[window] < 0)
            continue;
          for (gint dy = -9; dy < 9; dy++)
            for (gint dx = -9; dx < 9; dx++)
              {
                const guint pixel = window_center[window] + dy * (gint) CHICAGO_STAGE_WIDTH + dx;

                if (quality_mask[pixel] != 0)
                  annotated[pixel] = 0xff;
              }
        }
    }

  if (valid_windows == 0)
    return 0;

  return (((valid_windows / 2 + coherence_sum) / valid_windows) * 100) >> 16;
}

/* Ring-label term of the quality: percentage of enabled pixels whose
 * 3x3 ring (neighbor brighter than the center by more than 4) has at most two
 * transitions and exactly four bright neighbors, among the enabled pixels
 * with a label; the image border has no label. */
static gint
compute_ring_label4_percent (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                             const guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  static const gint ring_dy[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };
  static const gint ring_dx[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
  guint histogram[10] = { 0 };
  guint total = 0;

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      {
        const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
        guint label = 9;

        if (mask[pixel] == 0)
          continue;
        if (y >= 1 && y + 1 < CHICAGO_STAGE_HEIGHT && x >= 1 && x + 1 < CHICAGO_STAGE_WIDTH)
          {
            gint bits[8];
            gint transitions = 0;
            guint bright = 0;

            for (guint i = 0; i < 8; i++)
              {
                bits[i] = (gint) enhanced[(y + ring_dy[i]) * CHICAGO_STAGE_WIDTH + x + ring_dx[i]] -
                          (gint) enhanced[pixel] > 4;
                bright += bits[i];
              }
            for (guint i = 0; i < 8; i++)
              transitions += ABS (bits[i] - bits[(i + 1) % 8]);
            if (transitions <= 2)
              label = bright;
          }
        histogram[label]++;
      }
  for (guint label = 0; label < 9; label++)
    total += histogram[label];
  return total == 0 ? 0 : (gint) (histogram[4] * 100) / (gint) total;
}

/* For mode 24: quality and coverage of
 * the final 8-bit image. */
static void
compute_metrics (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                 guint8      *quality_out,
                 guint8      *coverage_out)
{
  guint8 mask[GOODIX_CHICAGO_PIXELS];
  guint8 annotated[GOODIX_CHICAGO_PIXELS];
  guint enabled = 0;
  gint mask_q16;
  gint coverage_q16;
  gint quality;

  goodix_chicago_preprocessor_build_quality_mask (enhanced, mask);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    enabled += mask[pixel] != 0;
  mask_q16 = (gint) (enabled << 16) / (gint) GOODIX_CHICAGO_PIXELS;

  quality = compute_base_quality_annotated (enhanced, mask, annotated);
  if (quality < 70)
    {
      const gint percent = compute_ring_label4_percent (enhanced, annotated);

      if (percent < 35)
        {
          const gint factor = (percent << 8) / 35;

          quality = (((quality * factor) >> 8) * factor) >> 8;
        }
    }

  coverage_q16 = compute_coverage_q16 (enhanced);
  if (mask_q16 - coverage_q16 > 0x3333)
    quality = coverage_q16 * quality / mask_q16 * coverage_q16 / mask_q16;

  goodix_chicago_preprocessor_finalize_metrics (quality, (coverage_q16 * 100) >> 16,
                                                quality_out, coverage_out);
}

void
goodix_chicago_preprocessor_build_quality_mask (
  const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
  guint8       quality_mask[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (quality_mask != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          gboolean saturated_cross = enhanced[pixel] == 0xff;

          for (guint distance = 1; distance <= 2 && saturated_cross; distance++)
            {
              if (x >= distance && enhanced[pixel - distance] != 0xff)
                saturated_cross = FALSE;
              if (x + distance < CHICAGO_STAGE_WIDTH &&
                  enhanced[pixel + distance] != 0xff)
                saturated_cross = FALSE;
              if (y >= distance &&
                  enhanced[pixel - distance * CHICAGO_STAGE_WIDTH] != 0xff)
                saturated_cross = FALSE;
              if (y + distance < CHICAGO_STAGE_HEIGHT &&
                  enhanced[pixel + distance * CHICAGO_STAGE_WIDTH] != 0xff)
                saturated_cross = FALSE;
            }

          quality_mask[pixel] = saturated_cross ? 0 : 0xff;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Per-frame context (mode 0x18).                                           */
/* Runs after the capture policy: builds a label map from the eroded        */
/* capture mask and the ImageBase-current difference, keeps a history of    */
/* accepted frames, and turns the labels into a class code.  The result is  */
/* written to cbuf[0..5], which feature extraction reads.                   */
/* ------------------------------------------------------------------------ */

#define CTX_W ((gint) CHICAGO_STAGE_WIDTH)
#define CTX_H ((gint) CHICAGO_STAGE_HEIGHT)
#define CTX_N ((gint) GOODIX_CHICAGO_PIXELS)
#define CTX_FLAG_HOLD 2   /* on entry when the frame is held (hold == TRUE) */

/* Mask erosion (depth 6): in every row and then every column, the first six
 * mask pixels from either end (counted on the input mask) are cleared. */
static void
context_erode_mask (const guint8 mask[GOODIX_CHICAGO_PIXELS],
                    guint8       eroded[GOODIX_CHICAGO_PIXELS])
{
  memcpy (eroded, mask, CTX_N);
  for (gint y = 0; y < CTX_H; y++)
    {
      const guint8 *in = mask + y * CTX_W;
      guint8 *out = eroded + y * CTX_W;
      gint left = 6;

      for (gint x = 0; x < CTX_W && left > 0; x++)
        if (in[x])
          {
            left--;
            out[x] = 0;
          }
      left = 6;
      for (gint x = CTX_W - 1; x >= 0 && left > 0; x--)
        if (in[x])
          {
            left--;
            out[x] = 0;
          }
    }
  for (gint x = 0; x < CTX_W; x++)
    {
      gint left = 6;

      for (gint y = 0; y < CTX_H && left > 0; y++)
        if (mask[y * CTX_W + x])
          {
            left--;
            eroded[y * CTX_W + x] = 0;
          }
      left = 6;
      for (gint y = CTX_H - 1; y >= 0 && left > 0; y--)
        if (mask[y * CTX_W + x])
          {
            left--;
            eroded[y * CTX_W + x] = 0;
          }
    }
}

/* With connectivity 4 and no margins: raster-ordered labels
 * 1, 2, ... (0 = background, -1 = not reached after the label limit).
 * stats[label] counts the pixels of each label. Labelling stops once
 * max_labels + 1 labels exist; the return value is the last label used. */
static gint
context_label_components (const guint8 image[GOODIX_CHICAGO_PIXELS],
                          gint         max_labels,
                          gint         components[GOODIX_CHICAGO_PIXELS],
                          gint         stats[100])
{
  static const gint dx[4] = { -1, 0, 1, 0 };
  static const gint dy[4] = { 0, -1, 0, 1 };
  g_autofree gint *queue = g_new (gint, CTX_N);
  gint label = 1;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    components[pixel] = -1;
  for (gint seed = 0; seed < CTX_N; seed++)
    {
      gint head = 0;
      gint tail = 0;

      if (components[seed] != -1)
        continue;
      if (!image[seed])
        {
          components[seed] = 0;
          continue;
        }
      components[seed] = label;
      stats[label]++;
      queue[tail++] = seed;
      while (head < tail)
        {
          const gint pixel = queue[head++];
          const gint x = pixel % CTX_W;
          const gint y = pixel / CTX_W;

          for (guint k = 0; k < 4; k++)
            {
              const gint nx = x + dx[k];
              const gint ny = y + dy[k];
              gint neighbor;

              if (nx < 0 || nx >= CTX_W || ny < 0 || ny >= CTX_H)
                continue;
              neighbor = ny * CTX_W + nx;
              if (components[neighbor] != -1)
                continue;
              if (!image[neighbor])
                {
                  components[neighbor] = 0;
                  continue;
                }
              components[neighbor] = label;
              stats[label]++;
              queue[tail++] = neighbor;
            }
        }
      label++;
      if (label > max_labels + 1)
        break;
    }

  return label - 1;
}

/* ids[j] = the j-th largest label (0..count) with more than
 * min_size pixels, or -2. */
static void
context_largest_components (gint stats[100],
                            gint count,
                            gint ids[3],
                            gint min_size)
{
  gint saved[3] = { 0, };

  for (guint j = 0; j < 3; j++)
    {
      gint best = min_size;

      for (gint label = 0; label <= count; label++)
        if (stats[label] > best)
          {
            best = stats[label];
            ids[j] = label;
          }
      if (ids[j] > 0)
        {
          saved[j] = stats[ids[j]];
          stats[ids[j]] = 0;
        }
    }
  for (guint j = 0; j < 3; j++)
    if (ids[j] > 0)
      stats[ids[j]] = saved[j];
}

/* Running history of the ImageBase-current difference over
 * frames whose capture mask covers more than an adaptive share. */
static void
context_update_history (GoodixChicagoPreprocessor *self,
                        const guint16              resolution_base[GOODIX_CHICAGO_PIXELS],
                        gint                       framenum,
                        gint                       mask_count,
                        const guint8               mask[GOODIX_CHICAGO_PIXELS])
{
  g_autofree gint16 *scaled = g_new (gint16, CTX_N);
  gint threshold;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    {
      const gint16 value = (gint16) (guint16) (((gint) resolution_base[pixel] - 0xfff) * 3);

      scaled[pixel] = value < 0 ? 0 : value;
    }
  if (framenum <= 1)
    {
      self->history_count = 0;
      self->history_threshold = 60;
    }
  threshold = self->history_threshold;
  if (mask_count * 100 > threshold * CTX_N)
    {
      const gint count = self->history_count;
      const gint percent = mask_count * 100 / CTX_N;
      gint sum = 0;
      gint mean;

      self->history_coverage = (percent + count * self->history_coverage) / (count + 1);
      threshold += 5;
      if (self->history_coverage > 80)
        threshold = 60;
      self->history_threshold = MIN (threshold, 85);
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        sum += scaled[pixel];
      mean = sum / CTX_N;
      if (mean != 0)
        for (gint pixel = 0; pixel < CTX_N; pixel++)
          self->history[pixel] = (gint16) (((((gint) scaled[pixel]) << 13) / mean +
                                            self->history[pixel] * count) / (count + 1));
      self->history_count++;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        if (mask[pixel])
          {
            if (self->presence[pixel] < 50)
              self->presence[pixel]++;
          }
        else if (self->presence[pixel])
          {
            self->presence[pixel]--;
          }
    }
  if (self->history_count > 50)
    self->history_count = 50;
}

/* State 3/4 when the pixels above `threshold` form one to four
 * dominant connected groups. */
static void
context_component_state (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                         const guint8  mask[GOODIX_CHICAGO_PIXELS],
                         gint          threshold,
                         gint         *state)
{
  g_autofree guint8 *map = g_new0 (guint8, CTX_N);
  g_autofree gint *components = g_new (gint, CTX_N);
  gint stats[100] = { 0, };
  gint count = 0;
  gint largest = 0;
  gint groups;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (mask[pixel] && (gint16) secondary[pixel] > threshold)
      {
        map[pixel] = 1;
        count++;
      }
  if (count < 5)
    return;
  groups = context_label_components (map, 25, components, stats);
  for (gint label = 0; label <= groups; label++)
    largest = MAX (largest, stats[label]);
  for (guint label = 0; label < G_N_ELEMENTS (stats); label++)
    if (stats[label] == 1)
      groups--;
  if (largest * 10 < count * 6 && largest * 100 < CTX_N * 15)
    return;
  if (groups > 4)
    return;
  *state = largest * 10 >= count * 8 ? 4 : 3;
}

/* (mode 0x18): a per-pixel counter of strong filtered-gradient
 * pixels (above 1.5x their mean); the three largest groups of persistent
 * ones become class 3. */
static void
context_apply_residue (GoodixChicagoPreprocessor *self,
                       const guint16              gradient[GOODIX_CHICAGO_PIXELS],
                       gint                       mask_count,
                       gint                       flag,
                       guint8                     labels[GOODIX_CHICAGO_PIXELS])
{
  g_autofree guint8 *map = g_new (guint8, CTX_N);
  g_autofree gint *components = g_new (gint, CTX_N);
  const gint percent = mask_count * 100 / CTX_N;
  gint stats[100] = { 0, };
  gint ids[3] = { -2, -2, -2 };
  gint sum = 0;
  gint count = 0;
  gint mean = 0;
  gint threshold;
  gint level;
  gboolean update;

  if (flag != CTX_FLAG_HOLD)
    self->residue_coverage = percent;
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    {
      const gint value = (gint16) gradient[pixel];

      if (value > 0)
        {
          sum += value;
          count++;
        }
    }
  if (count > 0)
    mean = (sum + count / 2) / count;
  threshold = mean * 3 / 2;
  level = MIN (self->residue_count, 2);
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    map[pixel] = (gint16) gradient[pixel] > (gint16) threshold &&
                 self->residue[pixel] >= level ? 0xff : 0;

  if (flag != CTX_FLAG_HOLD)
    update = percent >= 75;
  else
    update = percent >= 75 && self->residue_coverage < 75;
  if (update)
    {
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        if ((gint16) gradient[pixel] > threshold)
          {
            if (self->residue[pixel] < 5)
              self->residue[pixel]++;
          }
        else if (self->residue[pixel])
          {
            self->residue[pixel]--;
          }
      self->residue_count = MIN (self->residue_count + 1, 5);
    }

  count = context_label_components (map, 70, components, stats);
  context_largest_components (stats, count, ids, 75);
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (labels[pixel] == 0 &&
        (components[pixel] == ids[0] || components[pixel] == ids[1] ||
         components[pixel] == ids[2]))
      labels[pixel] = 3;
}

/* Helper: spread the flagged history values over 256 bins
 * (linear split between neighbouring bins, as in the secondary analysis). */
static void
context_history_histogram (const gint16 history[GOODIX_CHICAGO_PIXELS],
                           const guint8 flags[GOODIX_CHICAGO_PIXELS],
                           gint         lower,
                           gint         range,
                           gint         upper,
                           gboolean     bounded,
                           gint         histogram[256])
{
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    {
      const gint value = history[pixel];
      gint scaled;
      gint bin;
      gint remainder;

      if (!flags[pixel])
        continue;
      if (bounded && (value <= lower || value >= upper))
        continue;
      scaled = (value - lower) * 255;
      bin = scaled / range;
      remainder = scaled - bin * range;
      histogram[bin] += range - remainder;
      if (bin < 255)
        histogram[bin + 1] += remainder;
    }
}

/* 1 when the (trimmed) history histogram has a clear second
 * lobe away from its main peak. */
static gint
context_history_second_lobe (const gint histogram[256],
                             gint64     total,
                             gint       peak,
                             gint       range)
{
  static const gint offsets[20] = {
    -10, 10, -9, 9, -8, 8, -7, 7, -6, 6,
    -5, 5, -4, 4, -3, 3, -2, 2, -1, 1,
  };
  gint a[257] = { 0, };
  gint b[256] = { 0, };
  gint w[256] = { 0, };
  const gint64 scale = total >> 5;
  gint span;
  gint maximum = 0;
  gint limit;

  if ((guint) (peak - 10) > 241 || range <= 0)
    return 0;
  span = 128000 / range;
  for (guint bin = 0; bin < 256; bin++)
    a[bin] = histogram[bin] >> 5;
  for (guint pass = 0; pass < 2; pass++)
    {
      gint ring[21];
      gint sum = 0;
      gint oldest = 0;
      gint newest = 20;

      for (guint k = 0; k < 21; k++)
        {
          ring[k] = a[k];
          sum += a[k];
        }
      for (gint bin = 0; bin < 256; bin++)
        {
          if (bin >= 10 && bin <= 245)
            {
              const gint incoming = a[bin + 11];

              w[bin] = sum;
              b[bin] = (sum + 10) / 21;
              sum += incoming - ring[oldest];
              if (++oldest >= 21)
                oldest = 0;
              if (++newest >= 21)
                newest = 0;
              ring[newest] = incoming;
            }
          else
            {
              b[bin] = a[bin];
            }
        }
      memcpy (a, b, sizeof (b));
      a[256] = 0;
    }
  for (gint bin = 0; bin < 256; bin++)
    if (bin < 10 || bin > 245)
      b[bin] = bin >= 246 ? b[245] : b[10];
  for (gint bin = 0; bin < 256; bin++)
    if (w[bin] > maximum)
      {
        maximum = w[bin];
        peak = bin;
      }
  if ((gint64) (maximum * 100) > scale * 35)
    return 0;

  limit = b[peak] / 100;
  for (gint bin = 10; bin < 246; bin++)
    {
      gboolean local_maximum = TRUE;
      gint value = b[bin];
      gint valley = maximum;

      if (bin < span + peak && bin > peak - span)
        continue;
      for (guint k = 0; k < G_N_ELEMENTS (offsets); k++)
        if (b[bin + offsets[k]] > value)
          {
            local_maximum = FALSE;
            break;
          }
      if (!local_maximum)
        continue;
      value -= MAX (value * 5 / 100, limit);
      if (b[bin + 10] > value || b[bin - 10] > value ||
          w[bin] * 100 < maximum * 25)
        {
          bin += 10;
          continue;
        }
      if (bin < peak)
        for (gint k = bin; k < peak; k++)
          valley = MIN (valley, w[k]);
      else
        for (gint k = peak; k < bin; k++)
          valley = MIN (valley, w[k]);
      if (valley * 100 > w[bin] * 85)
        {
          bin += 10;
          continue;
        }
      if (valley * 100 <= maximum * 60)
        return 1;
      bin += 10;
    }
  return 0;
}

/* History threshold (35th, or 50th percentile when that is more
 * than 500 higher, capped at 9000) and the second-lobe flag. */
static gint
context_history_threshold (const gint16 history[GOODIX_CHICAGO_PIXELS],
                           const guint8 flags[GOODIX_CHICAGO_PIXELS],
                           gint        *lobe)
{
  gint histogram[256] = { 0, };
  gint trimmed[256] = { 0, };
  gint maximum = 0;
  gint minimum = 0x7fff;
  gint range;
  gint best = 0;
  gint best_bin = 0;
  gint64 total = 0;
  gint64 cumulative = 0;
  gint64 target;
  gint low = 0;
  gint high = 255;
  gint low_value;
  gint trimmed_range;
  gint64 trimmed_total = 0;
  gint trimmed_peak = 0;
  gint bin_35;
  gint bin_50;
  gint threshold;
  gint threshold_50;
  gboolean found = FALSE;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (flags[pixel])
      {
        maximum = MAX (maximum, history[pixel]);
        minimum = MIN (minimum, history[pixel]);
      }
  range = (gint16) (maximum - minimum);
  if (range <= 0)
    return minimum;

  context_history_histogram (history, flags, minimum, range, 0, FALSE, histogram);
  for (gint bin = 0; bin < 256; bin++)
    {
      if (histogram[bin] > best)
        {
          best = histogram[bin];
          best_bin = bin;
        }
      total += histogram[bin];
    }

  /* 0.5 % / 99.5 % trimmed histogram */
  for (gint bin = 0; bin < 256; bin++)
    {
      cumulative += histogram[bin];
      if ((gint) (total * 5 / 1000) <= cumulative && !found)
        {
          low = bin;
          found = TRUE;
        }
      if ((gint) (total * 995 / 1000) <= cumulative)
        {
          high = bin;
          break;
        }
    }
  low_value = range * low / 255 + minimum;
  trimmed_range = (gint16) (range * high / 255 + minimum - low_value);
  if (trimmed_range > 0)
    {
      gint trimmed_best = 0;

      context_history_histogram (history, flags, low_value, trimmed_range,
                                 range * high / 255 + minimum, TRUE, trimmed);
      for (gint bin = 0; bin < 256; bin++)
        {
          if (trimmed[bin] > trimmed_best)
            {
              trimmed_best = trimmed[bin];
              trimmed_peak = bin;
            }
          trimmed_total += trimmed[bin];
        }
    }
  *lobe = context_history_second_lobe (trimmed, trimmed_total, trimmed_peak,
                                       trimmed_range);

  target = total * 35 / 100;
  cumulative = 0;
  bin_35 = best_bin;
  for (gint bin = 0; bin < 256; bin++)
    {
      cumulative += histogram[bin];
      if (target <= cumulative)
        {
          bin_35 = bin;
          break;
        }
    }
  threshold = (range * bin_35 + 128) / 255 + minimum;
  target = total * 50 / 100;
  cumulative = 0;
  bin_50 = bin_35;
  for (gint bin = 0; bin < 256; bin++)
    {
      cumulative += histogram[bin];
      if (target <= cumulative)
        {
          bin_50 = bin;
          break;
        }
    }
  threshold_50 = (range * bin_50 + 128) / 255 + minimum;
  if (threshold_50 - threshold > 500)
    threshold = threshold_50;
  return MIN (threshold, 9000);
}

static const gint context_offsets[8] = {
  -1, 1, -CTX_W, CTX_W, -1 - CTX_W, CTX_W - 1, 1 - CTX_W, CTX_W + 1,
};

/* For every flagged pixel the steepest descent towards a
 * flagged 8-neighbour (value and direction, -1 when none). */
static void
context_history_descent (const gint16 history[GOODIX_CHICAGO_PIXELS],
                         const guint8 flags[GOODIX_CHICAGO_PIXELS],
                         gint16       descent[GOODIX_CHICAGO_PIXELS],
                         gint16       direction[GOODIX_CHICAGO_PIXELS])
{
  static const gint dx[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
  static const gint dy[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };

  for (gint y = 0; y < CTX_H; y++)
    for (gint x = 0; x < CTX_W; x++)
      {
        const gint pixel = y * CTX_W + x;
        gint16 best = 0;
        gint best_direction = -1;

        descent[pixel] = 0;
        direction[pixel] = -1;
        if (!flags[pixel])
          continue;
        for (gint k = 0; k < 8; k++)
          {
            const gint nx = x + dx[k];
            const gint ny = y + dy[k];
            const gint neighbor = pixel + context_offsets[k];
            gint16 difference;

            if (ny < 0 || ny >= CTX_H || nx < 0 || nx >= CTX_W)
              continue;
            difference = (gint16) (history[pixel] - history[neighbor]);
            if (flags[neighbor] && best < difference)
              {
                best = difference;
                best_direction = k;
              }
          }
        if (best_direction >= 0)
          {
            descent[pixel] = best;
            direction[pixel] = best_direction;
          }
      }
}

/* (checked = TRUE, floor 0) and (checked = FALSE):
 * sum of the descents along up to four steps of the descent path. */
static void
context_history_walk (const gint16 history[GOODIX_CHICAGO_PIXELS],
                      const gint16 descent[GOODIX_CHICAGO_PIXELS],
                      const gint16 direction[GOODIX_CHICAGO_PIXELS],
                      const guint8 flags[GOODIX_CHICAGO_PIXELS],
                      gboolean     checked,
                      gint         threshold,
                      gint16       out[GOODIX_CHICAGO_PIXELS])
{
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    {
      gint16 sum = 0;
      gint position = pixel;

      /* Compares with 0xffff instead, which always holds */
      if (flags[pixel] && (!checked || history[pixel] > threshold))
        {
          for (gint step = 0; step < 4; step++)
            {
              const gint d = direction[position];

              if (d < 0)
                break;
              if (checked &&
                  (history[position] < threshold || history[position + context_offsets[d]] < 0))
                break;
              sum += descent[position];
              position += context_offsets[d];
            }
        }
      out[pixel] = sum;
    }
}

/* Mean of the positive samples below `limit`, rounded. Returns 0 when there are none. */
static gint
context_mean_below (const gint16 plane[GOODIX_CHICAGO_PIXELS],
                    gint         limit,
                    gint        *count_out)
{
  gint sum = 0;
  gint count = 0;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (plane[pixel] > 0 && plane[pixel] < limit)
      {
        sum += plane[pixel];
        count++;
      }
  *count_out = count;
  return count > 0 ? (sum + count / 2) / count : 0;
}

/* Walk regions with the mode-0x18 parameters {1600, 768, 384, 0, 700, 800}:
 * strong walk sums, grown by promotion. */
static void
context_walk_regions (const gint16 walk[GOODIX_CHICAGO_PIXELS],
                      guint8       map[GOODIX_CHICAGO_PIXELS])
{
  gint count;
  const gint mean = context_mean_below (walk, 800, &count);
  gint high;
  gint low;

  memset (map, 0, CTX_N);
  if (count == 0)
    return;
  high = MAX (MIN (1600, mean * 5), (768 * mean) >> 8);
  low = MAX (MAX ((384 * mean) >> 8, mean), 700);
  count = 0;
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (walk[pixel] >= high)
      {
        map[pixel] = 0xff;
        count++;
      }
  if (count > 50)
    expand_resolution_promotion_mask ((const guint16 *) walk, low, map);
}

/* With {600, 640}: walk sums above the adaptive level. */
static void
context_walk_level_map (const gint16 walk[GOODIX_CHICAGO_PIXELS],
                        guint8       map[GOODIX_CHICAGO_PIXELS])
{
  gint count;
  const gint mean = context_mean_below (walk, 600, &count);
  const gint threshold = count > 0 ? MAX ((mean * 640) >> 8, 600) : 0;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    map[pixel] = walk[pixel] > threshold ? 0xff : 0;
}

/* Regions of the history where it falls steeply become class 3;
 * returns the second-lobe flag (context counter) and sets the region level
 * (0..2). */
static gint
context_history_regions (GoodixChicagoPreprocessor *self,
                         const guint8               flags[GOODIX_CHICAGO_PIXELS],
                         guint8                     labels[GOODIX_CHICAGO_PIXELS],
                         gint                      *level)
{
  g_autofree gint16 *descent = g_new (gint16, CTX_N);
  g_autofree gint16 *direction = g_new (gint16, CTX_N);
  g_autofree gint16 *walk = g_new (gint16, CTX_N);
  g_autofree guint8 *map = g_new (guint8, CTX_N);
  gint lobe = 0;
  gint threshold;
  gint regions = 0;

  threshold = context_history_threshold (self->history, flags, &lobe);
  context_history_descent (self->history, flags, descent, direction);
  context_history_walk (self->history, descent, direction, flags, TRUE, threshold, walk);
  context_walk_regions (walk, map);
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    if (map[pixel])
      labels[pixel] = 3;

  context_history_walk (self->history, descent, direction, flags, FALSE, 0, walk);
  context_walk_level_map (walk, map);
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    regions += map[pixel] != 0;
  *level = regions > 600 ? 2 : regions > 300 ? 1 : 0;
  return lobe;
}

/* Enclosure level: holes of the eroded mask and bright current
 * samples that are enclosed on all sides (level 0..2). */
static gint
context_enclosure_level (const guint16 current[GOODIX_CHICAGO_PIXELS],
                         const guint8  eroded[GOODIX_CHICAGO_PIXELS],
                         gint          mask_count)
{
  g_autofree guint8 *left = g_new0 (guint8, CTX_N);
  g_autofree guint8 *right = g_new0 (guint8, CTX_N);
  g_autofree guint8 *up = g_new0 (guint8, CTX_N);
  g_autofree guint8 *down = g_new0 (guint8, CTX_N);
  gint enclosed = 0;
  gint bright = 0;

  for (gint y = 0; y < CTX_H; y++)
    for (gint x = 1; x < CTX_W; x++)
      {
        const gint pixel = y * CTX_W + x;
        const gint mirror = y * CTX_W + CTX_W - 1 - x;

        left[pixel] = left[pixel - 1] + (eroded[pixel - 1] != 0);
        right[mirror] = right[mirror + 1] + (eroded[mirror + 1] != 0);
      }
  for (gint y = 1; y < CTX_H; y++)
    for (gint x = 0; x < CTX_W; x++)
      {
        const gint pixel = y * CTX_W + x;
        const gint mirror = (CTX_H - 1 - y) * CTX_W + x;

        up[pixel] = up[pixel - CTX_W] + (eroded[pixel - CTX_W] != 0);
        down[mirror] = down[mirror + CTX_W] + (eroded[mirror + CTX_W] != 0);
      }
  for (gint pixel = 0; pixel < CTX_N; pixel++)
    {
      const gboolean inside = left[pixel] > 5 && right[pixel] > 5 &&
                              up[pixel] > 5 && down[pixel] > 5;
      const gboolean between = (left[pixel] > 5 && right[pixel] > 5) ||
                               (up[pixel] > 5 && down[pixel] > 5);
      const gboolean high = (gint16) current[pixel] > 3800;

      bright += high && inside;
      enclosed += (!eroded[pixel] && inside) || (high && between);
    }
  if (bright * 10 > mask_count)
    return 2;
  return (bright + enclosed) * 10 > mask_count ? 1 : 0;
}

/* Per-frame context for mode 0x18. `current` and `image_base` are the
 * prepared planes, `mask` the capture mask (0/0xff) with `mask_count` set
 * pixels, `framenum` the calidata frame counter of this frame. */
static GoodixChicagoPreprocessStatus
context_build (GoodixChicagoPreprocessor *self,
               const guint16              current[GOODIX_CHICAGO_PIXELS],
               const guint16              image_base[GOODIX_CHICAGO_PIXELS],
               const guint8               mask[GOODIX_CHICAGO_PIXELS],
               gint                       framenum,
               gboolean                   hold,
               guint8                     context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE])
{
  g_autofree guint8 *eroded = g_new (guint8, CTX_N);
  g_autofree guint8 *labels = g_new0 (guint8, CTX_N);
  g_autofree guint16 *resolution_base = g_new (guint16, CTX_N);
  g_autofree guint16 *secondary = g_new (guint16, CTX_N);
  g_autofree guint16 *gradient = g_new (guint16, CTX_N);
  GoodixChicagoResolutionSecondaryAnalysis analysis;
  GoodixChicagoPreprocessStatus status = GOODIX_CHICAGO_PREPROCESS_STATUS_OK;
  const gint flag = hold ? CTX_FLAG_HOLD : 0;
  gint mask_count = 0;
  gint state;
  gint counter = 0;
  gint region_level = 0;
  gint enclosure;
  guint class3;
  guint code = 0;
  gboolean auxiliary = FALSE;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    mask_count += mask[pixel] != 0;

  context_erode_mask (mask, eroded);
  goodix_chicago_preprocessor_build_resolution_base_plane (current, image_base,
                                                           resolution_base);
  if (flag != CTX_FLAG_HOLD)
    {
      if (self->history_count == 0 && framenum > 1)
        {
          for (gint pixel = 0; pixel < CTX_N; pixel++)
            self->history[pixel] = (gint16) self->kr[pixel];
          self->history_count = MIN (framenum, 21);
          memset (self->presence, self->history_count, CTX_N);
        }
      else
        {
          context_update_history (self, resolution_base, framenum, mask_count, mask);
        }
    }

  goodix_chicago_preprocessor_build_resolution_secondary_plane (resolution_base, secondary);
  build_resolution_map_labels_analysis (secondary, eroded, labels, &analysis, gradient);
  state = analysis.peak_state;
  context_component_state (secondary, eroded,
                           state == 2 ? analysis.peak_value : analysis.upper_innermost_mean,
                           &state);
  context_apply_residue (self, gradient, mask_count, flag, labels);

  if (self->history_count > 20)
    {
      g_autofree guint8 *flags = g_new0 (guint8, CTX_N);

      for (gint y = 3; y < CTX_H - 3; y++)
        for (gint x = 3; x < CTX_W - 3; x++)
          {
            const gint pixel = y * CTX_W + x;

            flags[pixel] = self->presence[pixel] * 100 >= self->history_count * 50 &&
                           eroded[pixel] != 0;
          }
      counter = context_history_regions (self, flags, labels, &region_level);
    }
  for (gint y = 3; y < CTX_H - 3; y++)
    for (gint x = 3; x < CTX_W - 3; x++)
      if ((gint16) current[y * CTX_W + x] < 50)
        labels[y * CTX_W + x] = 3;
  enclosure = context_enclosure_level (current, eroded, mask_count);
  self->history_started = TRUE;

  class3 = goodix_chicago_preprocessor_classify_resolution_labels (
    0x18, labels, CTX_N, mask_count, &code, &auxiliary);
  if ((gint) class3 * 100 > mask_count * 30 ||
      (state == 2 && (gint) class3 * 100 > mask_count * 10))
    {
      code = 9;
      status = GOODIX_CHICAGO_PREPROCESS_STATUS_CLASS_REJECT;
    }
  if (enclosure == 2)
    {
      if (code < 8)
        code = 8;
    }
  else if ((enclosure == 1 || counter >= 1) && code < 6)
    {
      code = 6;
    }
  if (state == 2 || enclosure == 2)
    counter++;
  if (code == 0 && region_level > 0)
    code = 5;
  if (state == 2)
    state = counter >= 2 ? 3 : 2;
  if (flag != CTX_FLAG_HOLD)
    {
      self->held_state = state;
      self->held_code = code;
    }
  else
    {
      code = MAX ((gint) code, self->held_code);
      state = MAX (state, self->held_state);
    }

  context[0] = (guint8) state;
  context[1] = (guint8) flag;
  context[2] = (guint8) counter;
  context[3] = (guint8) code;
  context[4] = (guint8) ((mask_count * 100 + 50) / CTX_N);
  context[5] = 0;
  return status;
}

GoodixChicagoPreprocessStatus
goodix_chicago_preprocessor_process_context (GoodixChicagoPreprocessor *self,
                                             const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                             gint                       purpose,
                                             gboolean                   hold,
                                             guint8                     enhanced[GOODIX_CHICAGO_PIXELS],
                                             guint8                    *quality,
                                             guint8                    *coverage,
                                             guint8                     context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE])
{
  GoodixChicagoPreprocessStatus status;

  g_return_val_if_fail (self != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);
  g_return_val_if_fail (raw != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);
  g_return_val_if_fail (enhanced != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);

  status = process_internal (self, raw, purpose, hold, enhanced, quality, coverage);
  if (context)
    memcpy (context, self->context, sizeof (self->context));
  return status;
}

void
goodix_chicago_preprocessor_get_state (const GoodixChicagoPreprocessor *self,
                                       GoodixChicagoPreprocessState    *state)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (state != NULL);

  state->framenum = self->framenum;
  state->multiplier_count = self->multiplier_count;
  state->kr = self->kr;
  state->gain = self->normalized_gain;
  state->multiplier = self->multiplier;
  state->auxiliary = self->auxiliary;
}
