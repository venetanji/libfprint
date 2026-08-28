// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Native state for the recovered Chicago preprocessing boundary. */

#include <math.h>
#include <string.h>

#include "goodix-chicago-preprocess.h"

#define CHICAGO_STAGE_WIDTH 80u
#define CHICAGO_STAGE_HEIGHT 64u
#define CHICAGO_REJECT_HISTOGRAM_BINS 200u
#define CHICAGO_REJECT_FLAT_RANGE 150u

typedef struct
{
  gboolean active;
  guint    flat_count;
  guint    histogram[CHICAGO_REJECT_HISTOGRAM_BINS];
} GoodixChicagoCandidateRejectStats;

static guint reflect_101 (gint  coordinate,
                          guint length);

struct _GoodixChicagoPreprocessor
{
  GBytes  *calibration;
  guint16  image_base[GOODIX_CHICAGO_PIXELS];
  guint16  gain[GOODIX_CHICAGO_PIXELS];
  guint16  offset[GOODIX_CHICAGO_PIXELS];
  guint16  temporal_gain_average[GOODIX_CHICAGO_PIXELS];
  guint32  temporal_sample_count;
  guint16  normalized_gain[GOODIX_CHICAGO_PIXELS];
  gboolean normalized_gain_valid;
  guint16  adaptive_multiplier[GOODIX_CHICAGO_PIXELS];
  guint32  adaptive_sample_count;
};

static void
normalize_gain_map (GoodixChicagoPreprocessor *self)
{
  guint64 sum = 0;
  guint32 mean;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    sum += self->gain[pixel];

  /* AlgoChicago+0x4d810 uses (sum + pixels / 2) / pixels. */
  mean = (sum + GOODIX_CHICAGO_PIXELS / 2) / GOODIX_CHICAGO_PIXELS;
  if (mean == 0)
    return;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      guint32 normalized;

      normalized = (self->gain[pixel] * 0x2000u + mean / 2) / mean;
      self->normalized_gain[pixel] = normalized;
    }
  self->normalized_gain_valid = TRUE;
}

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
      self->temporal_gain_average[pixel] = self->gain[pixel];
      self->adaptive_multiplier[pixel] = 0x2000u;
    }
  if (!goodix_chicago_calibration_get_temporal_sample_count (
        calibration, &self->temporal_sample_count, error))
    {
      goodix_chicago_preprocessor_free (self);
      return NULL;
    }
  normalize_gain_map (self);

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

static void
temporal_smooth_normalized_gain (GoodixChicagoPreprocessor *self)
{
  guint32 raw_kernel[5];
  guint32 kernel[5];
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];
  guint64 kernel_sum = 0;
  const guint32 sigma_q16 = 0x320000u / self->temporal_sample_count;
  const double sigma = (double) sigma_q16 / 65536.0;

  for (gint delta = -2; delta <= 2; delta++)
    {
      const double exponent = -(double) (delta * delta) /
                              (2.0 * sigma * sigma);

      /* The production DLL converts the positive floating-point weight to an
       * integer by truncation.  Counts 155 and 156 mask the distinction, but
       * at count 157 the side weight is 473.615... and must become 473, not
       * 474, yielding the official [0,466,64603,466,0] normalized kernel. */
      raw_kernel[delta + 2] = (guint32) (exp (exponent) * 65536.0);
      kernel_sum += raw_kernel[delta + 2];
    }
  for (guint index = 0; index < G_N_ELEMENTS (kernel); index++)
    kernel[index] = ((guint64) raw_kernel[index] * 65536u +
                     kernel_sum / 2) / kernel_sum;

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

static void
update_adaptive_multiplier (GoodixChicagoPreprocessor *self,
                            const guint16              source[GOODIX_CHICAGO_PIXELS])
{
  static const guint32 kernel[5] = {
    7869u, 15328u, 19142u, 15328u, 7869u,
  };
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];
  guint16 filtered[GOODIX_CHICAGO_PIXELS];
  const guint32 new_count = MIN (self->adaptive_sample_count + 1, 30u);

  /* AlgoChicago+0x4b8c0 selector 9 is a separable sigma=1.5 Gaussian.
   * It runs in the preprocessing plane's 80x64 geometry, truncates after
   * both Q16 passes, and uses reflect-101 borders. */
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

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const guint32 local_mean = filtered[pixel];
      const guint32 ratio = local_mean == 0 ? 0x2000u :
                            ((guint32) source[pixel] * 0x2000u + local_mean / 2) / local_mean;

      if (ABS ((gint64) ratio - 0x2000) < 0x148)
        self->adaptive_multiplier[pixel] =
          ((guint64) self->adaptive_multiplier[pixel] *
           self->adaptive_sample_count + ratio + new_count / 2) / new_count;
    }
  self->adaptive_sample_count = new_count;
}

static void
update_temporal_gain (GoodixChicagoPreprocessor *self,
                      const guint16              current[GOODIX_CHICAGO_PIXELS],
                      const guint16              image_base[GOODIX_CHICAGO_PIXELS])
{
  guint32 candidate[GOODIX_CHICAGO_PIXELS];
  guint32 difference_sum = 0;
  guint32 average_sum = 0;
  guint32 difference_mean;
  guint32 scale;
  guint32 new_count;
  guint32 average_mean;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      candidate[pixel] = image_base[pixel] > current[pixel] ?
                         image_base[pixel] - current[pixel] : 0;
      difference_sum += candidate[pixel];
    }
  difference_mean = (difference_sum + GOODIX_CHICAGO_PIXELS / 2) /
                    GOODIX_CHICAGO_PIXELS;
  if (difference_mean == 0)
    difference_mean = 2000;
  scale = (2000u * 0x2000u + difference_mean / 2) / difference_mean;
  new_count = self->temporal_sample_count + 1;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      guint32 value = ((guint64) candidate[pixel] * scale + 0x1000u) >> 13;
      guint32 average = self->temporal_gain_average[pixel];

      if (ABS ((gint64) value - 2000) < 1600)
        average = ((guint64) average * self->temporal_sample_count + value +
                   new_count / 2) / new_count;
      self->temporal_gain_average[pixel] = average;
      average_sum += average;
    }
  self->temporal_sample_count = MIN (new_count, 400u);
  average_mean = (average_sum + GOODIX_CHICAGO_PIXELS / 2) /
                 GOODIX_CHICAGO_PIXELS;
  if (average_mean == 0)
    return;

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    self->normalized_gain[pixel] =
      ((guint32) self->temporal_gain_average[pixel] * 0x2000u +
       average_mean / 2) / average_mean;
  if (self->temporal_sample_count > 3 && self->temporal_sample_count <= 200)
    temporal_smooth_normalized_gain (self);
}

void
goodix_chicago_preprocessor_free (GoodixChicagoPreprocessor *self)
{
  if (!self)
    return;

  g_clear_pointer (&self->calibration, g_bytes_unref);
  g_free (self);
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

gboolean
goodix_chicago_preprocessor_get_corrections (const GoodixChicagoPreprocessor *self,
                                             guint                            pixel,
                                             guint16                         *gain,
                                             guint16                         *offset)
{
  g_return_val_if_fail (self != NULL, FALSE);

  if (pixel >= GOODIX_CHICAGO_PIXELS)
    return FALSE;

  if (gain)
    *gain = self->gain[pixel];
  if (offset)
    *offset = self->offset[pixel];
  return TRUE;
}

gboolean
goodix_chicago_preprocessor_get_normalized_gain (
  const GoodixChicagoPreprocessor *self,
  guint                            pixel,
  guint16                         *normalized_gain)
{
  g_return_val_if_fail (self != NULL, FALSE);

  if (!self->normalized_gain_valid || pixel >= GOODIX_CHICAGO_PIXELS)
    return FALSE;

  if (normalized_gain)
    *normalized_gain = self->normalized_gain[pixel];
  return TRUE;
}

static void
build_source_plane_internal (
  GoodixChicagoPreprocessor *self,
  const guint16              current[GOODIX_CHICAGO_PIXELS],
  const guint16              image_base[GOODIX_CHICAGO_PIXELS],
  gboolean                   apply_adaptive_multiplier,
  guint16                    source[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->normalized_gain_valid);
  g_return_if_fail (current != NULL);
  g_return_if_fail (image_base != NULL);
  g_return_if_fail (source != NULL);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const guint32 difference = image_base[pixel] > current[pixel] ?
                                 image_base[pixel] - current[pixel] : 0;
      guint32 gain = self->normalized_gain[pixel];
      guint32 value;

      if (apply_adaptive_multiplier)
        gain = ((guint32) gain * self->adaptive_multiplier[pixel] +
                0x1000u) >> 13;

      if (gain == 0)
        value = difference << 13;
      else
        value = (difference * 0x2000u + gain / 2) / gain;
      source[pixel] = value;
    }
}

void
goodix_chicago_preprocessor_build_source_plane (
  GoodixChicagoPreprocessor *self,
  const guint16              current[GOODIX_CHICAGO_PIXELS],
  const guint16              image_base[GOODIX_CHICAGO_PIXELS],
  guint16                    source[GOODIX_CHICAGO_PIXELS])
{
  build_source_plane_internal (self, current, image_base,
                               self->adaptive_sample_count > 5, source);
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

  /* +0x43d56 only materializes the contrast histogram when more than one
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

static void
build_alternate_source (
  const guint16 source[GOODIX_CHICAGO_PIXELS],
  guint16       alternate[GOODIX_CHICAGO_PIXELS])
{
  for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
    {
      guint32 sum = 0;
      guint16 mean;

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        sum += source[y * CHICAGO_STAGE_WIDTH + x];
      mean = sum / CHICAGO_STAGE_HEIGHT;

      for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
        {
          const guint pixel = y * CHICAGO_STAGE_WIDTH + x;
          const gint value = (gint) source[pixel] - mean + 5000;

          alternate[pixel] = MAX (value, 0);
        }
    }
}

/* AlgoChicago+0x49610. The mask selects real candidate bytes; disabled bytes
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

  /* The DLL deliberately divides by count + 1. */
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

/* AlgoChicago+0x5efb0 -> +0x5ee90. Disabled pixels become neutral gray 122;
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
  guint8                             enhanced[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoCandidateRejectStats *reject_stats)
{
  guint16 source[GOODIX_CHICAGO_PIXELS];
  guint8 mask[GOODIX_CHICAGO_PIXELS];
  g_autofree guint16 *alternate_source = NULL;
  g_autofree guint8 *alternate = NULL;
  const gboolean bootstrap_gain = self->temporal_sample_count == 0;

  g_return_if_fail (self != NULL);
  g_return_if_fail (current != NULL);
  g_return_if_fail (image_base != NULL);
  g_return_if_fail (enhanced != NULL);

  /* The official preprocessor updates its local multiplier from the source
   * normalized only by the temporal gain.  Starting with capture six it then
   * rebuilds the visible source with the newly updated multiplier. A freshly
   * initialized calibration has no temporal samples at all; +0x4a000 uses the
   * first capture itself to bootstrap this plane and applies that first ratio
   * immediately. Production calibration normally enters with a non-zero
   * sample count and therefore follows only the delayed path. */
  build_source_plane_internal (self, current, image_base, FALSE, source);
  update_adaptive_multiplier (self, source);
  if (bootstrap_gain || self->adaptive_sample_count > 5)
    build_source_plane_internal (self, current, image_base, TRUE, source);
  goodix_chicago_preprocessor_build_mask (
    self, current, image_base, mask);
  build_candidate_internal (self, source, mask, enhanced, reject_stats);

  /* ChicagoHS mode 24 always supplies selector flag +0x1c == 1, which bypasses
   * the generic component-count prefilter and enters this recovered policy.
   * The production reference score is 600. */
  alternate_source = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  alternate = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  build_alternate_source (source, alternate_source);
  goodix_chicago_preprocessor_build_candidate (
    self, alternate_source, mask, alternate);
  if (goodix_chicago_preprocessor_select_alternate_mode24 (
        candidate_column_variation_score (enhanced, mask),
        candidate_column_variation_score (alternate, mask),
        candidate_correlation_q8 (enhanced, alternate, mask),
        600))
    memcpy (enhanced, alternate, GOODIX_CHICAGO_PIXELS);
  update_temporal_gain (self, current, image_base);
}

void
goodix_chicago_preprocessor_build_enhanced_prepared (
  GoodixChicagoPreprocessor *self,
  const guint16              current[GOODIX_CHICAGO_PIXELS],
  const guint16              image_base[GOODIX_CHICAGO_PIXELS],
  guint8                     enhanced[GOODIX_CHICAGO_PIXELS])
{
  build_enhanced_prepared_internal (
    self, current, image_base, enhanced, NULL);
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

  /* AlgoChicago+0x47920: first lower-tail bin exceeding 30 percent. */
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

/* AlgoChicago+0x49490 mode 24. The two-pixel interior excludes transport
 * borders and must contain strictly more than 50 percent non-saturated
 * samples. The official bounds are also strict. */
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

void
goodix_chicago_preprocessor_build_enhanced (
  GoodixChicagoPreprocessor *self,
  const guint16              raw[GOODIX_CHICAGO_PIXELS],
  guint8                     enhanced[GOODIX_CHICAGO_PIXELS])
{
  goodix_chicago_preprocessor_build_enhanced_checked (self, raw, enhanced);
}

GoodixChicagoPreprocessStatus
goodix_chicago_preprocessor_build_enhanced_checked (
  GoodixChicagoPreprocessor *self,
  const guint16              raw[GOODIX_CHICAGO_PIXELS],
  guint8                     enhanced[GOODIX_CHICAGO_PIXELS])
{
  guint16 current[GOODIX_CHICAGO_PIXELS];
  guint16 image_base[GOODIX_CHICAGO_PIXELS];
  GoodixChicagoCandidateRejectStats reject_stats;
  guint8 quality;
  guint8 coverage;

  g_return_val_if_fail (self != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);
  g_return_val_if_fail (raw != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);
  g_return_val_if_fail (enhanced != NULL, GOODIX_CHICAGO_PREPROCESS_STATUS_OK);

  goodix_chicago_preprocessor_prepare_raw (self, raw, current);
  if (!mode24_raw_input_is_valid (current))
    {
      memset (enhanced, 0, GOODIX_CHICAGO_PIXELS);
      return GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT;
    }
  goodix_chicago_preprocessor_prepare_raw (
    self, self->image_base, image_base);
  build_enhanced_prepared_internal (
    self, current, image_base, enhanced, &reject_stats);
  goodix_chicago_preprocessor_finalize_metrics (
    goodix_chicago_preprocessor_compute_base_quality_from_enhanced (enhanced),
    goodix_chicago_preprocessor_compute_coverage (enhanced),
    &quality, &coverage);
  (void) coverage;

  /* preprocessor+0x492bb enters +0x48870 only below quality 35. */
  if (quality < 35 && candidate_reject_stats_is_poor_capture (&reject_stats))
    return GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE;
  return GOODIX_CHICAGO_PREPROCESS_STATUS_OK;
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
goodix_chicago_preprocessor_build_resolution_input_mask (
  guint8 input_mask[GOODIX_CHICAGO_PIXELS])
{
  g_return_if_fail (input_mask != NULL);

  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    for (guint x = 0; x < CHICAGO_STAGE_WIDTH; x++)
      input_mask[y * CHICAGO_STAGE_WIDTH + x] =
        x >= 6 && x + 6 < CHICAGO_STAGE_WIDTH &&
        y >= 6 && y + 6 < CHICAGO_STAGE_HEIGHT ? 0xff : 0;
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
  gint vendor_statistics[34];

  g_return_if_fail (primary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (statistics != NULL);

  calculate_resolution_histogram_statistics (
    primary, input_mask, 2, TRUE, vendor_statistics);
  statistics->primary_center_a = vendor_statistics[5];
  statistics->primary_center_b = vendor_statistics[6];
  statistics->primary_high_sample = vendor_statistics[7];
  statistics->primary_mid_sample = vendor_statistics[8];
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

  for (gint index = 201; index < 390; index++)
    if (window_sums[index] > window_sums[high_index])
      high_index = index;
  for (gint index = 199; index >= 10; index--)
    if (window_sums[index] > window_sums[low_index])
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
  range = maximum - minimum;
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

void
goodix_chicago_preprocessor_build_resolution_map_labels_full (
  const guint16 secondary[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
  guint8        labels[GOODIX_CHICAGO_PIXELS],
  guint        *peak_state_out)
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

  g_return_if_fail (secondary != NULL);
  g_return_if_fail (input_mask != NULL);
  g_return_if_fail (labels != NULL);

  filtered_gradient = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  primary = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  promotion_mask = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  memset (labels, 0, GOODIX_CHICAGO_PIXELS);

  goodix_chicago_preprocessor_calculate_resolution_secondary_analysis (
    secondary, input_mask, &analysis);
  if (peak_state_out)
    *peak_state_out = analysis.peak_state;
  goodix_chicago_preprocessor_build_resolution_filtered_gradient (
    secondary, input_mask, filtered_gradient);
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

void
goodix_chicago_preprocessor_build_resolution_map_labels (
  const guint16 secondary[GOODIX_CHICAGO_PIXELS],
  const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
  guint8        labels[GOODIX_CHICAGO_PIXELS])
{
  goodix_chicago_preprocessor_build_resolution_map_labels_full (
    secondary, input_mask, labels, NULL);
}

void
goodix_chicago_preprocessor_build_resolution_map_full (
  GoodixChicagoPreprocessor *self,
  const guint16              raw[GOODIX_CHICAGO_PIXELS],
  guint8                     labels[GOODIX_CHICAGO_PIXELS],
  guint                     *peak_state_out)
{
  g_autofree guint16 *current = NULL;
  g_autofree guint16 *image_base = NULL;
  g_autofree guint16 *resolution_base = NULL;
  g_autofree guint16 *secondary = NULL;
  g_autofree guint8 *input_mask = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (raw != NULL);
  g_return_if_fail (labels != NULL);

  current = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  image_base = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  resolution_base = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  secondary = g_new (guint16, GOODIX_CHICAGO_PIXELS);
  input_mask = g_new (guint8, GOODIX_CHICAGO_PIXELS);
  goodix_chicago_preprocessor_prepare_raw (self, raw, current);
  goodix_chicago_preprocessor_prepare_raw (
    self, self->image_base, image_base);
  goodix_chicago_preprocessor_build_resolution_base_plane (
    current, image_base, resolution_base);
  goodix_chicago_preprocessor_build_resolution_secondary_plane (
    resolution_base, secondary);
  goodix_chicago_preprocessor_build_resolution_input_mask (input_mask);
  goodix_chicago_preprocessor_build_resolution_map_labels_full (
    secondary, input_mask, labels, peak_state_out);
}

void
goodix_chicago_preprocessor_build_resolution_map (
  GoodixChicagoPreprocessor *self,
  const guint16              raw[GOODIX_CHICAGO_PIXELS],
  guint8                     labels[GOODIX_CHICAGO_PIXELS])
{
  goodix_chicago_preprocessor_build_resolution_map_full (
    self, raw, labels, NULL);
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

guint
goodix_chicago_preprocessor_pack_resolution_code (guint code)
{
  static const guint scale_by_code[10] = {
    0, 2, 2, 3, 0, 1, 2, 4, 5, 5,
  };

  if (code >= G_N_ELEMENTS (scale_by_code))
    return 0;
  return scale_by_code[code] << 8;
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

gint
goodix_chicago_preprocessor_compute_coverage (
  const guint8 enhanced[GOODIX_CHICAGO_PIXELS])
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

  /* +0x4e570 selector 7 is a separable five-tap Q16 Gaussian. Both passes
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

  /* +0x507e0/+0x50610: Sobel derivatives of the high byte. The derivative
   * axis is cleared at its two outer edges; the smoothing axis reflects. */
  for (guint y = 0; y < CHICAGO_STAGE_HEIGHT; y++)
    {
      const guint previous_y = reflect_101 ((gint) y - 1,
                                            CHICAGO_STAGE_HEIGHT);
      const guint next_y = reflect_101 ((gint) y + 1,
                                        CHICAGO_STAGE_HEIGHT);

      for (guint x = 1; x + 1 < CHICAGO_STAGE_WIDTH; x++)
        {
          gint value;

          value = ((smoothed[previous_y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                   (smoothed[previous_y * CHICAGO_STAGE_WIDTH + x - 1] >> 8)) +
                  2 * ((smoothed[y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                       (smoothed[y * CHICAGO_STAGE_WIDTH + x - 1] >> 8)) +
                  ((smoothed[next_y * CHICAGO_STAGE_WIDTH + x + 1] >> 8) -
                   (smoothed[next_y * CHICAGO_STAGE_WIDTH + x - 1] >> 8));
          gradient_x[y * CHICAGO_STAGE_WIDTH + x] = value;
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
          gint value;

          value = ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + previous_x] >> 8) -
                   (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + previous_x] >> 8)) +
                  2 * ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + x] >> 8) -
                       (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + x] >> 8)) +
                  ((smoothed[(y + 1) * CHICAGO_STAGE_WIDTH + next_x] >> 8) -
                   (smoothed[(y - 1) * CHICAGO_STAGE_WIDTH + next_x] >> 8));
          gradient_y[y * CHICAGO_STAGE_WIDTH + x] = value;
        }
    }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    magnitude[pixel] = ABS ((gint) gradient_x[pixel]) / 2 +
                       ABS ((gint) gradient_y[pixel]) / 2;

  /* +0x4f510 averages a reflect-101 15x15 window using the truncated Q16
   * reciprocal floor(65536 / 225), then +0x509b0 counts values above 120. */
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
          if (local_mean > 120)
            active_pixels++;
        }
    }

  fixed_coverage = (active_pixels << 16) / GOODIX_CHICAGO_PIXELS;
  return (fixed_coverage * 100u) >> 16;
}

gint
goodix_chicago_preprocessor_compute_base_quality (
  const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
  const guint8 quality_mask[GOODIX_CHICAGO_PIXELS])
{
  g_autofree gint32 *gradient_x = NULL;
  g_autofree gint32 *gradient_y = NULL;
  g_autofree gint32 *gradient_energy = NULL;
  gint coherence_sum = 0;
  guint valid_windows = 0;

  g_return_val_if_fail (enhanced != NULL, 0);
  g_return_val_if_fail (quality_mask != NULL, 0);

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
        }
    }

  if (valid_windows == 0)
    return 0;

  return (((valid_windows / 2 + coherence_sum) / valid_windows) * 100) >> 16;
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

gint
goodix_chicago_preprocessor_compute_base_quality_from_enhanced (
  const guint8 enhanced[GOODIX_CHICAGO_PIXELS])
{
  guint8 quality_mask[GOODIX_CHICAGO_PIXELS];

  g_return_val_if_fail (enhanced != NULL, 0);
  goodix_chicago_preprocessor_build_quality_mask (enhanced, quality_mask);
  return goodix_chicago_preprocessor_compute_base_quality (
    enhanced, quality_mask);
}
