// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: complete type-24 feature extraction; unused code
 * removed. */

/* Chicago mode-24 feature extraction. */

#include <stdint.h>
#include <string.h>

#include "goodix-chicago-feature.h"


typedef struct
{
  guint   length;
  guint32 coefficient[15];
} ChicagoKernel;

static const ChicagoKernel feature_kernels[GOODIX_CHICAGO_FEATURE_SCALES] = {
  { 7,  { 0x0123, 0x0dd3, 0x3df6, 0x6628, 0x3df6, 0x0dd3, 0x0123 } },
  { 9,  { 0x004d, 0x0391, 0x14e1, 0x3c4f, 0x55e4, 0x3c4f, 0x14e1, 0x0391,
          0x004d } },
  { 7,  { 0x0010, 0x0464, 0x38d5, 0x856e, 0x38d5, 0x0464, 0x0010 } },
  { 7,  { 0x007e, 0x0a09, 0x3d5e, 0x7036, 0x3d5e, 0x0a09, 0x007e } },
  { 9,  { 0x001a, 0x0207, 0x111e, 0x3d94, 0x5e5b, 0x3d94, 0x111e, 0x0207,
          0x001a } },
  { 9,  { 0x00a3, 0x0540, 0x17bd, 0x3ab1, 0x4f5e, 0x3ab1, 0x17bd, 0x0540,
          0x00a3 } },
  { 11, { 0x0052, 0x0232, 0x09c7, 0x1c6c, 0x35ea, 0x42be, 0x35ea, 0x1c6c,
          0x09c7, 0x0232, 0x0052 } },
  { 13, { 0x003f, 0x014b, 0x0505, 0x0e6f, 0x1eb1, 0x3043, 0x381f, 0x3043,
          0x1eb1, 0x0e6f, 0x0505, 0x014b, 0x003f } },
  { 15, { 0x0041, 0x0103, 0x0347, 0x0890, 0x1211, 0x1ece, 0x2a6c, 0x2f34,
          0x2a6c, 0x1ece, 0x1211, 0x0890, 0x0347, 0x0103, 0x0041 } },
};

static const ChicagoKernel gradient_input_kernels[2] = {
  { 9, { 0x0054, 0x03bd, 0x1539, 0x3c27, 0x551e, 0x3c27, 0x1539, 0x03bd,
         0x0054 } },
  { 7, { 0x008b, 0x0a70, 0x3d7e, 0x6f0e, 0x3d7e, 0x0a70, 0x008b } },
};

typedef struct
{
  gint8 x;
  gint8 y;
} ChicagoDirectionOffset;

static const ChicagoDirectionOffset direction_offsets[12][7] = {
  { { -3,  0 }, { -2,  0 }, { -1,  0 }, { 0, 0 }, { 1,  0 }, { 2,  0 }, { 3,  0 } },
  { { -3, -1 }, { -2, -1 }, { -1,  0 }, { 0, 0 }, { 1,  0 }, { 2,  1 }, { 3,  1 } },
  { { -3, -2 }, { -2, -1 }, { -1, -1 }, { 0, 0 }, { 1,  1 }, { 2,  1 }, { 3,  2 } },
  { { -3, -3 }, { -2, -2 }, { -1, -1 }, { 0, 0 }, { 1,  1 }, { 2,  2 }, { 3,  3 } },
  { { -2, -3 }, { -1, -2 }, { -1, -1 }, { 0, 0 }, { 1,  1 }, { 1,  2 }, { 2,  3 } },
  { { -1, -3 }, { -1, -2 }, {  0, -1 }, { 0, 0 }, { 0,  1 }, { 1,  2 }, { 1,  3 } },
  { {  0, -3 }, {  0, -2 }, {  0, -1 }, { 0, 0 }, { 0,  1 }, { 0,  2 }, { 0,  3 } },
  { { -1,  3 }, { -1,  2 }, {  0,  1 }, { 0, 0 }, { 0, -1 }, { 1, -2 }, { 1, -3 } },
  { { -2,  3 }, { -1,  2 }, { -1,  1 }, { 0, 0 }, { 1, -1 }, { 1, -2 }, { 2, -3 } },
  { { -3,  3 }, { -2,  2 }, { -1,  1 }, { 0, 0 }, { 1, -1 }, { 2, -2 }, { 3, -3 } },
  { { -3,  2 }, { -2,  1 }, { -1,  1 }, { 0, 0 }, { 1, -1 }, { 2, -1 }, { 3, -2 } },
  { { -3,  1 }, { -2,  1 }, { -1,  0 }, { 0, 0 }, { 1,  0 }, { 2, -1 }, { 3, -1 } },
};

static const guint direction_weights[7] = { 1, 2, 4, 8, 4, 2, 1 };

static const guint16 cordic_angles[13] = {
  0x0c91, 0x076b, 0x03eb, 0x01fd, 0x0100, 0x0080, 0x0040,
  0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001,
};

static const gint32 cordic_gains[13] = {
  0xb505, 0xa1e9, 0x9d13, 0x9bdd, 0x9b8f, 0x9b7b, 0x9b77,
  0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75,
};

static const guint32 chicago_exp2_thresholds[16] = {
  0x95c0, 0x526a, 0x2b80, 0x1664, 0x0b5d, 0x05ba, 0x02e0, 0x0171,
  0x00b8, 0x005c, 0x002e, 0x0017, 0x000c, 0x0006, 0x0003, 0x0001,
};

G_STATIC_ASSERT (sizeof (GoodixChicagoCandidate) == 24);
G_STATIC_ASSERT (sizeof (GoodixChicagoFeatureRecord) == 0x3c);
G_STATIC_ASSERT (sizeof (GoodixChicagoFeatureRank) == 8);
G_STATIC_ASSERT (sizeof (GoodixChicagoFeatureAux) == 24);

static guint16
cordic_magnitude_orientation (gint32  vertical,
                              gint32 *horizontal)
{
  const gint32 original_horizontal = *horizontal;
  const gint32 original_vertical = vertical;
  gint32 x = original_horizontal > 0 ? original_horizontal :
             -original_horizontal;
  gint32 y = original_vertical > 0 ? original_vertical : -original_vertical;
  guint16 angle = 0;
  guint stop = 12;

  if (y == 0)
    {
      *horizontal = x;
      return original_horizontal > 0 ? 0 : 0x3244;
    }
  if (x == 0)
    {
      *horizontal = y;
      return original_vertical > 0 ? 0x1922 : 0xe6de;
    }

  for (guint iteration = 0; iteration <= 12; iteration++)
    {
      const gint32 y_shift = y >> iteration;
      const gint32 x_shift = x >> iteration;

      if (y > 0)
        {
          x += y_shift;
          y -= x_shift;
          angle = (guint16) (angle + cordic_angles[iteration]);
        }
      else
        {
          x -= y_shift;
          y += x_shift;
          angle = (guint16) (angle - cordic_angles[iteration]);
        }

      if (y == 0)
        {
          stop = iteration;
          break;
        }
    }

  if (original_horizontal > 0)
    {
      if (original_vertical < 0)
        angle = (guint16) - angle;
    }
  else if (original_vertical > 0)
    {
      angle = (guint16) (0x3244 - angle);
    }
  else
    {
      angle = (guint16) (angle + 0xcdbc);
    }

  *horizontal = (gint32) (((gint64) cordic_gains[stop] * x + 0x8000) >> 16);
  return angle;
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

static void
filter_q16 (const guint16        input[GOODIX_CHICAGO_FEATURE_PIXELS],
            guint16              output[GOODIX_CHICAGO_FEATURE_PIXELS],
            const ChicagoKernel *kernel)
{
  g_autofree guint16 *horizontal =
    g_new (guint16, GOODIX_CHICAGO_FEATURE_PIXELS);
  const gint radius = kernel->length / 2;

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          guint64 sum = 0;

          for (gint delta = -radius; delta <= radius; delta++)
            {
              const guint source_x =
                reflect_101 ((gint) x + delta,
                             GOODIX_CHICAGO_FEATURE_WIDTH);

              sum += (guint32) input[y * GOODIX_CHICAGO_FEATURE_WIDTH +
                                     source_x] *
                     kernel->coefficient[delta + radius];
            }
          horizontal[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
            MIN (sum >> 16, G_MAXUINT16);
        }
    }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          guint64 sum = 0;

          for (gint delta = -radius; delta <= radius; delta++)
            {
              const guint source_y =
                reflect_101 ((gint) y + delta,
                             GOODIX_CHICAGO_FEATURE_HEIGHT);

              sum += (guint32) horizontal[source_y *
                                          GOODIX_CHICAGO_FEATURE_WIDTH + x] *
                     kernel->coefficient[delta + radius];
            }
          output[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
            MIN (sum >> 16, G_MAXUINT16);
        }
    }
}

static guint32
integral_rectangle_sum (const guint32 integral[GOODIX_CHICAGO_FEATURE_PIXELS],
                        guint         x,
                        guint         y,
                        guint         radius)
{
  const guint left = MAX (1u, x > radius ? x - radius : 0u);
  const guint right = MIN (GOODIX_CHICAGO_FEATURE_WIDTH - 1, x + radius);
  const guint top = MAX (1u, y > radius ? y - radius : 0u);
  const guint bottom = MIN (GOODIX_CHICAGO_FEATURE_HEIGHT - 1, y + radius);

  return integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + right] -
         integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1] -
         integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + right] +
         integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1];
}

void
goodix_chicago_feature_build_orientation_map (
  const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint8       orientation[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  g_autofree guint32 *cross = NULL;
  g_autofree guint32 *difference = NULL;
  g_autofree guint32 *cross_integral = NULL;
  g_autofree guint32 *difference_integral = NULL;

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (orientation != NULL);

  cross = g_new0 (guint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  difference = g_new0 (guint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  cross_integral = g_new0 (guint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  difference_integral = g_new0 (guint32, GOODIX_CHICAGO_FEATURE_PIXELS);

  for (guint y = 1; y + 1 < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 1; x + 1 < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;
          const gint32 horizontal =
            2 * ((gint32) enhanced[pixel + 1] - enhanced[pixel - 1]) -
            enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH - 1] -
            enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH - 1] +
            enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH + 1] +
            enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH + 1];
          const gint32 vertical =
            2 * ((gint32) enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH] -
                 enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH]) -
            enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH + 1] -
            enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH - 1] +
            enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH - 1] +
            enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH + 1];

          cross[pixel] = (guint32) (2 * horizontal * vertical);
          difference[pixel] = (guint32) (horizontal * horizontal -
                                         vertical * vertical);
        }
    }

  for (guint y = 1; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 1; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;

          cross_integral[pixel] = cross[pixel] +
                                  cross_integral[pixel - 1] +
                                  cross_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH] -
                                  cross_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH - 1];
          difference_integral[pixel] = difference[pixel] +
                                       difference_integral[pixel - 1] +
                                       difference_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH] -
                                       difference_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH - 1];
        }
    }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          gint32 horizontal = (gint32) integral_rectangle_sum (
            cross_integral, x, y, 6);
          const gint32 vertical = (gint32) integral_rectangle_sum (
            difference_integral, x, y, 6);
          gint32 angle = (gint16) cordic_magnitude_orientation (vertical,
                                                                &horizontal);
          gint32 degrees;
          gint32 adjusted;

          if (angle < 0)
            angle += 0x6488;
          degrees = angle * 0x1ca6 >> 20;
          adjusted = degrees - 0x87;
          if (adjusted <= 0)
            adjusted = degrees + 0x2d;
          orientation[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
            (guint8) (0xb4 - adjusted);
        }
    }
}

void
goodix_chicago_feature_build_prepared_image (
  const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint8       prepared[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  guint8 orientation[GOODIX_CHICAGO_FEATURE_PIXELS];

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (prepared != NULL);

  goodix_chicago_feature_build_orientation_map (enhanced, orientation);
  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;
          const guint value = orientation[pixel];
          const guint bucket = value >= 8 && value < 173 ?
                               (value - 8) / 15 + 1 : 0;
          guint weighted_sum = 0;
          guint weight_sum = 0;

          for (guint sample = 0; sample < G_N_ELEMENTS (direction_weights);
               sample++)
            {
              const gint source_x =
                (gint) x + direction_offsets[bucket][sample].x;
              const gint source_y =
                (gint) y + direction_offsets[bucket][sample].y;

              if (source_x < 0 ||
                  source_x >= (gint) GOODIX_CHICAGO_FEATURE_WIDTH ||
                  source_y < 0 ||
                  source_y >= (gint) GOODIX_CHICAGO_FEATURE_HEIGHT)
                continue;
              weighted_sum +=
                enhanced[source_y * GOODIX_CHICAGO_FEATURE_WIDTH + source_x] *
                direction_weights[sample];
              weight_sum += direction_weights[sample];
            }
          prepared[pixel] = weight_sum ? weighted_sum / weight_sum : 0xff;
        }
    }
}

void
goodix_chicago_feature_build_source (
  const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint8       source[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  const ChicagoKernel prefilter = {
    3, { 0x5555, 0x5555, 0x5555 },
  };
  g_autofree guint16 *lifted = NULL;
  g_autofree guint16 *filtered = NULL;

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (source != NULL);

  lifted = g_new (guint16, GOODIX_CHICAGO_FEATURE_PIXELS);
  filtered = g_new (guint16, GOODIX_CHICAGO_FEATURE_PIXELS);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    lifted[pixel] = (guint16) enhanced[pixel] << 8;

  filter_q16 (lifted, filtered, &prefilter);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    source[pixel] = filtered[pixel] >> 8;
}

void
goodix_chicago_feature_build_scale_space (
  const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint16      scales[GOODIX_CHICAGO_FEATURE_SCALES]
  [GOODIX_CHICAGO_FEATURE_PIXELS])
{
  guint8 source[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint16 lifted[GOODIX_CHICAGO_FEATURE_PIXELS];

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (scales != NULL);

  goodix_chicago_feature_build_source (enhanced, source);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    lifted[pixel] = (guint16) source[pixel] << 8;

  filter_q16 (lifted, scales[0], &feature_kernels[0]);
  filter_q16 (lifted, scales[1], &feature_kernels[1]);
  for (guint scale = 2; scale < GOODIX_CHICAGO_FEATURE_SCALES; scale++)
    filter_q16 (scales[scale - 1], scales[scale], &feature_kernels[scale]);
}

guint
goodix_chicago_feature_collect_extrema (
  const guint16          scales[GOODIX_CHICAGO_FEATURE_SCALES]
  [GOODIX_CHICAGO_FEATURE_PIXELS],
  GoodixChicagoExtremum *extrema,
  guint                  capacity)
{
  guint count = 0;

  g_return_val_if_fail (scales != NULL, 0);
  g_return_val_if_fail (extrema != NULL || capacity == 0, 0);

  for (gint scale = 6; scale >= 1; scale--)
    {
      for (guint y = 6; y + 6 < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
        {
          for (guint x = 6; x + 6 < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
            {
              const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;
              const gint32 response =
                (gint16) (scales[scale + 1][pixel] - scales[scale][pixel]);
              gboolean extremum = ABS (response) > 0x148 && response != 0;

              for (gint adjacent_scale = scale - 1;
                   extremum && adjacent_scale <= scale + 1;
                   adjacent_scale++)
                {
                  for (gint delta_y = -1; extremum && delta_y <= 1;
                       delta_y++)
                    {
                      for (gint delta_x = -1; delta_x <= 1; delta_x++)
                        {
                          const guint neighbor =
                            (y + delta_y) * GOODIX_CHICAGO_FEATURE_WIDTH +
                            x + delta_x;
                          const gint32 neighbor_response =
                            (gint32) scales[adjacent_scale + 1][neighbor] -
                            scales[adjacent_scale][neighbor];

                          if ((response > 0 && neighbor_response > response) ||
                              (response < 0 && neighbor_response < response))
                            {
                              extremum = FALSE;
                              break;
                            }
                        }
                    }
                }

              if (!extremum)
                continue;
              if (count < capacity)
                {
                  extrema[count] = (GoodixChicagoExtremum){
                    .x = x,
                    .y = y,
                    .scale = scale,
                    .response = response,
                  };
                }
              count++;
            }
        }
    }

  return count;
}

static gboolean
build_refinement_derivatives (
  const guint16 scales[GOODIX_CHICAGO_FEATURE_SCALES]
  [GOODIX_CHICAGO_FEATURE_PIXELS],
  gint32        x,
  gint32        y,
  gint32        scale,
  gint16        hessian[6],
  gint16        gradient[3],
  gint16       *center)
{
  const gint32 width = GOODIX_CHICAGO_FEATURE_WIDTH;
  const gint32 pixel = y * width + x;
  const guint16 *previous = scales[scale - 1];
  const guint16 *current = scales[scale];
  const guint16 *next = scales[scale + 1];
  const guint16 *next2 = scales[scale + 2];
  gint32 value;

  *center = (gint16) (2 * ((gint32) next[pixel] - current[pixel]));

  value = 2 * (((gint32) current[pixel - 1] - next[pixel - 1]) -
               current[pixel + 1] + next[pixel + 1]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  gradient[0] = (gint16) value;

  value = 2 * (((gint32) current[pixel - width] - next[pixel - width]) -
               current[pixel + width] + next[pixel + width]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  gradient[1] = (gint16) value;

  value = 2 * (((gint32) previous[pixel] - current[pixel]) - next[pixel] +
               next2[pixel]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  gradient[2] = (gint16) value;

  value = 4 * (((gint32) next[pixel - 1] - current[pixel - 1]) -
               current[pixel + 1] - *center + next[pixel + 1]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  hessian[0] = (gint16) value;

  value = 4 * (((gint32) next[pixel - width] - current[pixel - width]) -
               *center - current[pixel + width] + next[pixel + width]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  hessian[1] = (gint16) value;

  value = 4 * (((gint32) current[pixel] - previous[pixel]) - next[pixel] -
               *center + next2[pixel]);
  if (ABS (value) >= 0x8000)
    return FALSE;
  hessian[2] = (gint16) value;

  value = ((gint32) current[pixel + width - 1] - next[pixel + width - 1]) -
          next[pixel - width + 1] - current[pixel + width + 1] -
          current[pixel - width - 1] + current[pixel - width + 1] +
          next[pixel + width + 1] + next[pixel - width - 1];
  if (ABS (value) >= 0x2000)
    return FALSE;
  hessian[3] = (gint16) value;

  value = ((gint32) previous[pixel + width] - previous[pixel - width]) -
          next2[pixel - width] + next2[pixel + width] - next[pixel + width] -
          current[pixel + width] + current[pixel - width] + next[pixel - width];
  if (ABS (value) >= 0x2000)
    return FALSE;
  hessian[4] = (gint16) value;

  value = ((gint32) previous[pixel + 1] - previous[pixel - 1]) -
          next2[pixel - 1] + next2[pixel + 1] - current[pixel + 1] -
          next[pixel + 1] + next[pixel - 1] + current[pixel - 1];
  if (ABS (value) >= 0x2000)
    return FALSE;
  hessian[5] = (gint16) value;
  return TRUE;
}

static gint
signed_bit_length (gint64 value)
{
  guint64 magnitude = value < 0 ? (guint64) - value : (guint64) value;
  gint length = 1;

  while (magnitude != 0)
    {
      length++;
      magnitude >>= 1;
    }
  return length;
}

static void
solve_refinement_offset (const gint16 hessian[6],
                         const gint16 gradient[3],
                         gint32       offset[3])
{
  const gint64 h00 = hessian[0];
  const gint64 h11 = hessian[1];
  const gint64 h22 = hessian[2];
  const gint64 h01 = hessian[3];
  const gint64 h12 = hessian[4];
  const gint64 h02 = hessian[5];
  const gint64 cofactor0 = h11 * h22 - h12 * h12;
  const gint64 cofactor1 = h02 * h12 - h01 * h22;
  const gint64 cofactor2 = h01 * h12 - h02 * h11;
  const gint64 determinant =
    h00 * cofactor0 + h01 * cofactor1 + h02 * cofactor2;
  gint64 numerator[3];
  const gint determinant_bits = signed_bit_length (determinant);

  if (determinant == 0)
    {
      offset[0] = 0;
      offset[1] = 0;
      offset[2] = 0;
      return;
    }

  numerator[0] = -((gint64) gradient[0] * cofactor0 +
                   (gint64) gradient[1] * cofactor1 +
                   (gint64) gradient[2] * cofactor2);
  numerator[1] = -((h00 * h22 - h02 * h02) * gradient[1] +
                   (h01 * h02 - h00 * h12) * gradient[2] +
                   (gint64) gradient[0] * cofactor1);
  numerator[2] = -((h00 * h11 - h01 * h01) * gradient[2] +
                   (h01 * h02 - h00 * h12) * gradient[1] +
                   (gint64) gradient[0] * cofactor2);

  for (guint axis = 0; axis < 3; axis++)
    {
      gint numerator_bits = signed_bit_length (numerator[axis]);
      gint64 scaled_numerator = numerator[axis];
      gint64 scaled_determinant = determinant;

      if (numerator_bits - determinant_bits >= 9)
        {
          offset[axis] = 0x00ffffff;
          continue;
        }
      numerator_bits = MAX (numerator_bits, determinant_bits);
      if (numerator_bits > 32)
        {
          scaled_numerator >>= numerator_bits - 32;
          scaled_determinant >>= numerator_bits - 32;
        }
      offset[axis] = (gint32) ((scaled_numerator * 0x1000) /
                               scaled_determinant);
    }
}

static gint32
round_q12_move (gint32 value)
{
  if (value < 0)
    {
      const guint32 magnitude = (guint32) - value;

      return -(gint32) ((magnitude >> 12) +
                        ((magnitude & 0x800) != 0));
    }
  return (value >> 12) + (((guint32) value & 0x800) != 0);
}

static guint32
chicago_exp2_q16 (gint32 value)
{
  const guint32 magnitude = value < 0 ? (guint32) - value : (guint32) value;
  guint32 fraction = magnitude & 0xffff;
  guint64 result = UINT64_C (0x10000);

  if ((magnitude >> 16) > 0)
    result <<= magnitude >> 16;
  for (guint bit = 0; bit < G_N_ELEMENTS (chicago_exp2_thresholds); bit++)
    {
      if (fraction < chicago_exp2_thresholds[bit])
        continue;
      fraction -= chicago_exp2_thresholds[bit];
      result += result >> (bit + 1);
    }

  if (value > 0)
    return (guint32) result;
  return (guint32) (UINT64_C (0x100000000) / result);
}

gboolean
goodix_chicago_feature_refine_extremum (
  const guint16           scales[GOODIX_CHICAGO_FEATURE_SCALES]
  [GOODIX_CHICAGO_FEATURE_PIXELS],
  GoodixChicagoCandidate *candidate,
  guint32                *curvature)
{
  gint16 hessian[6];
  gint16 gradient[3];
  gint16 center;
  gint32 offset[3];
  gint32 x;
  gint32 y;
  gint32 scale;

  g_return_val_if_fail (scales != NULL, FALSE);
  g_return_val_if_fail (candidate != NULL, FALSE);
  g_return_val_if_fail (curvature != NULL, FALSE);

  x = candidate->x;
  y = candidate->y;
  scale = candidate->scale;
  for (guint iteration = 0; iteration < 5; iteration++)
    {
      gint64 contrast;
      gint32 trace;
      gint32 determinant;

      if (!build_refinement_derivatives (scales, x, y, scale, hessian,
                                         gradient, &center))
        return FALSE;
      solve_refinement_offset (hessian, gradient, offset);
      if (ABS (offset[0]) < 0x800 && ABS (offset[1]) < 0x800 &&
          ABS (offset[2]) < 0x800)
        {
          contrast = (gint64) center * 0x2000 +
                     (gint64) gradient[0] * offset[0] +
                     (gint64) gradient[1] * offset[1] +
                     (gint64) gradient[2] * offset[2];
          if (contrast <= 0)
            contrast = -contrast;
          if (contrast < 0x1478000)
            return FALSE;
          candidate->strength = (gint32) (contrast >> 12);

          trace = (gint32) hessian[0] + hessian[1];
          determinant = (gint32) hessian[0] * hessian[1] -
                        (gint32) hessian[3] * hessian[3];
          if (determinant <= 0)
            return FALSE;

          candidate->refined_x = (gint16) ((x * 0x1000 + offset[0]) >> 4);
          candidate->refined_y = (gint16) ((y * 0x1000 + offset[1]) >> 4);
          *curvature = (guint32) (((gint64) trace * trace * 0x400) /
                                  determinant);
          candidate->scale_value = (gint32) chicago_exp2_q16 (
            ((scale * 0x1000 + offset[2]) * 0x10) / 4);
          return TRUE;
        }

      x += round_q12_move (offset[0]);
      y += round_q12_move (offset[1]);
      scale += round_q12_move (offset[2]);
      candidate->x = x;
      candidate->y = y;
      candidate->scale = scale;
      if (scale < 1 || scale > 6 || x < 1 ||
          x >= (gint32) GOODIX_CHICAGO_FEATURE_WIDTH - 1 || y < 1 ||
          y >= (gint32) GOODIX_CHICAGO_FEATURE_HEIGHT - 1)
        return FALSE;
    }

  return FALSE;
}

static void
build_orientation_weights (guint32 *weights,
                           guint    size,
                           guint32  coefficient)
{
  for (guint y = 0; y < size; y++)
    {
      for (guint x = y; x < size; x++)
        {
          const guint32 radius_squared = x * x + y * y;
          const guint32 power = radius_squared * coefficient;
          guint32 weight = 0;

          if (power <= 0x6ee75)
            {
              const guint32 reduced = power >> 8;
              const guint32 reduced_squared = reduced * reduced;
              guint32 approximation = reduced_squared >> 8;

              approximation =
                ((approximation * approximation >> 8) *
                 ((power / 5 + 0x10000) >> 8)) / 0xc00 + 0x200 +
                (((power / 6 + 0x8000) >> 8) * reduced_squared >> 15) +
                (power >> 7);
              weight = ((approximation >> 1) + 0x40000) / approximation;
            }
          weights[y * size + x] = weight;
          weights[x * size + y] = weight;
        }
    }
}

static guint32
build_orientation_histogram (
  gint32        center_x,
  gint32        center_y,
  gint32        scale_value,
  const guint32 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
  const gint16  orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint32       histogram[36],
  gint32       *selected_bin)
{
  gint32 radius_q16 = (gint32) ((guint32) scale_value * 18u);
  gint32 sigma_q16 = (gint32) ((guint32) scale_value * 6u);
  guint32 raw_histogram[36] = { 0, };
  guint32 extended[40];
  guint32 weights[33 * 33] = { 0, };
  gint32 radius;
  guint size;
  guint32 coefficient;
  guint32 peak;
  gint32 peak_bin = 0;

  radius_q16 >>= 2;
  sigma_q16 >>= 2;
  radius = radius_q16 < 0 ?
           -(((-radius_q16) >> 16) +
             ((((guint32) - radius_q16) & 0x8000) != 0)) :
           (radius_q16 >> 16) + (((guint32) radius_q16 & 0x8000) != 0);
  radius = MIN (radius, 32);
  size = radius + 1;
  coefficient = (guint32) (G_GINT64_CONSTANT (0x800000000000) /
                           ((gint64) sigma_q16 * sigma_q16));
  build_orientation_weights (weights, size, coefficient);

  const gint32 min_y = MAX (-radius, 1 - center_y);
  const gint32 max_y = MIN (radius,
                            (gint32) GOODIX_CHICAGO_FEATURE_HEIGHT -
                            center_y - 2);
  const gint32 min_x = MAX (-radius, 1 - center_x);
  const gint32 max_x = MIN (radius,
                            (gint32) GOODIX_CHICAGO_FEATURE_WIDTH -
                            center_x - 2);

  for (gint32 delta_y = min_y; delta_y <= max_y; delta_y++)
    {
      for (gint32 delta_x = min_x; delta_x <= max_x; delta_x++)
        {
          const guint pixel =
            (center_y + delta_y) * GOODIX_CHICAGO_FEATURE_WIDTH +
            center_x + delta_x;
          gint32 bin = ((gint32) orientation[pixel] * -36 + 0x743d4) /
                       0x6488;
          const guint32 weighted =
            magnitude[pixel] *
            weights[ABS (delta_y) * size + ABS (delta_x)] >> 8;

          if (bin >= 36)
            bin = 0;
          raw_histogram[bin] += weighted;
          raw_histogram[(bin + 18) % 36] += weighted;
        }
    }

  extended[0] = raw_histogram[34];
  extended[1] = raw_histogram[35];
  memcpy (&extended[2], raw_histogram, sizeof (raw_histogram));
  extended[38] = raw_histogram[0];
  extended[39] = raw_histogram[1];
  for (guint bin = 0; bin < 36; bin++)
    histogram[bin] = (extended[bin] + 4 * extended[bin + 1] +
                      6 * extended[bin + 2] + 4 * extended[bin + 3] +
                      extended[bin + 4]) >> 4;

  peak = histogram[0];
  for (gint32 bin = 1; bin < 36; bin++)
    {
      if (histogram[bin] > peak)
        {
          peak = histogram[bin];
          peak_bin = bin;
        }
    }
  *selected_bin = peak_bin + 18;
  return peak;
}

void
goodix_chicago_feature_materialize_candidate (
  const guint8                  feature_source[GOODIX_CHICAGO_FEATURE_PIXELS],
  const guint32                 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
  const gint16                  orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
  const GoodixChicagoCandidate *candidate,
  guint                         index,
  GoodixChicagoFeatureRecord   *record,
  GoodixChicagoFeatureRank     *rank,
  GoodixChicagoFeatureAux      *aux)
{
  guint32 histogram[36];
  gint32 selected_bin;
  guint32 peak;
  gint32 previous_bin;
  gint32 next_bin;
  gint64 previous;
  gint64 current;
  gint64 next;
  gint64 denominator;
  gint16 interpolated;
  gint16 feature_orientation;
  guint pixel;
  gint32 neighborhood;
  gint32 absolute_strength;

  g_return_if_fail (feature_source != NULL);
  g_return_if_fail (magnitude != NULL);
  g_return_if_fail (orientation != NULL);
  g_return_if_fail (candidate != NULL);
  g_return_if_fail (record != NULL);
  g_return_if_fail (rank != NULL);
  g_return_if_fail (aux != NULL);

  peak = build_orientation_histogram (
    candidate->x, candidate->y, candidate->scale_value, magnitude,
    orientation, histogram, &selected_bin);
  previous_bin = selected_bin == 0 ? 35 : selected_bin - 1;
  next_bin = (selected_bin + 1) % 36;
  previous = histogram[previous_bin];
  current = histogram[selected_bin];
  next = histogram[next_bin];
  denominator = 2 * previous + 2 * next - 4 * current;
  interpolated = (gint16)
                 (((previous - next) * 0x200 - (denominator >> 1)) / denominator +
                  selected_bin * 0x200);
  pixel = candidate->y * GOODIX_CHICAGO_FEATURE_WIDTH + candidate->x;
  neighborhood = feature_source[pixel] + feature_source[pixel - 1] +
                 feature_source[pixel + 1] +
                 feature_source[pixel - GOODIX_CHICAGO_FEATURE_WIDTH] +
                 feature_source[pixel + GOODIX_CHICAGO_FEATURE_WIDTH];
  absolute_strength = candidate->strength;

  if (interpolated < 0)
    interpolated += 0x4800;
  else if (interpolated >= 0x4800)
    interpolated -= 0x4800;
  feature_orientation =
    (gint16) ((((gint32) interpolated * 0x6488) / 36) >> 9) - 0x3244;
  if (feature_orientation < 0)
    feature_orientation += 0x3244;

  if (absolute_strength <= 0)
    absolute_strength = -absolute_strength;
  memset (record, 0, sizeof (*record));
  record->foreground = neighborhood > 0x27f;
  record->refined_x = candidate->refined_x;
  record->refined_y = candidate->refined_y;
  record->orientation = feature_orientation;
  record->strength = -absolute_strength;

  rank->strength = -absolute_strength;
  rank->index = index;
  *aux = (GoodixChicagoFeatureAux){
    .x = candidate->x,
    .y = candidate->y,
    .scale_value = candidate->scale_value,
    .peak = peak,
    .selected_peak = histogram[selected_bin],
    .secondary_orientation = histogram[selected_bin] != peak,
  };
}

static void
descriptor_sine_cosine (gint16  angle,
                        gint32 *sine,
                        gint32 *cosine)
{
  gint32 reduced = angle;
  gint16 sin_value = 0;
  gint16 cos_value = 0x4000;
  gint16 accumulated = 0;

  if (reduced > 0x1922)
    reduced = 0x3244 - reduced;
  if (reduced == 0)
    {
      *sine = 0;
      *cosine = angle > 0x1922 ? -0x4000 : 0x4000;
      return;
    }
  if (reduced == 0x1922)
    {
      *sine = 0x4000;
      *cosine = 0;
      return;
    }

  for (guint iteration = 0; iteration < 13; iteration++)
    {
      gint16 cos_shift = cos_value >> iteration;
      gint16 sin_shift = sin_value >> iteration;
      gint16 delta;

      if (reduced - accumulated < 0)
        {
          cos_shift = (gint16) - cos_shift;
          delta = (gint16) - cordic_angles[iteration];
        }
      else
        {
          sin_shift = (gint16) - sin_shift;
          delta = cordic_angles[iteration];
        }
      accumulated = (gint16) (accumulated + delta);
      sin_value = (gint16) (sin_value + cos_shift);
      cos_value = (gint16) (cos_value + sin_shift);
    }

  *sine = ((gint32) sin_value * 0x9b75 + 0x8000) >> 16;
  *cosine = ((gint32) cos_value * 0x9b75 + 0x8000) >> 16;
  if (angle > 0x1922)
    *cosine = -*cosine;
}

static void
descriptor_accumulate (guint32 accumulator[6][6][8],
                       gint32  x,
                       gint32  y,
                       gint32  angle,
                       gint32  weight)
{
  const gint32 x_bin = x >> 9;
  const gint32 y_bin = y >> 9;
  const gint32 x_fraction = x - x_bin * 0x200;
  const gint32 y_fraction = y - y_bin * 0x200;
  const gint32 orientation_bin = (angle >> 12) & 7;
  const gint32 next_orientation = (orientation_bin + 1) & 7;
  const gint32 orientation_fraction = angle - (angle >> 12) * 0x1000;
  const guint32 scaled_weight = (guint32) (weight >> 9);
  const guint32 upper_y =
    (guint32) (y_fraction * (gint32) scaled_weight) >> 9;
  const guint32 lower_y = scaled_weight - upper_y;
  const guint32 upper_right =
    (guint32) (x_fraction * (gint32) upper_y) >> 9;
  const guint32 lower_right =
    (guint32) (x_fraction * (gint32) lower_y) >> 9;
  const guint32 cell_weight[2][2] = {
    { lower_y - lower_right, lower_right },
    { upper_y - upper_right, upper_right },
  };

  for (guint cell_y = 0; cell_y < 2; cell_y++)
    {
      for (guint cell_x = 0; cell_x < 2; cell_x++)
        {
          const guint32 value = cell_weight[cell_y][cell_x];
          const guint32 orientation_part = value * orientation_fraction;
          guint32 *cell = accumulator[y_bin + 1 + cell_y]
                          [x_bin + 1 + cell_x];

          cell[orientation_bin] +=
            (value - (orientation_part >> 12)) >> 5;
          cell[next_orientation] += orientation_part >> 17;
        }
    }
}

static void
build_descriptor_samples (
  gint32        center_x,
  gint32        center_y,
  gint16        feature_orientation,
  const guint32 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
  const gint16  orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
  gint32        samples[128])
{
  const gint32 descriptor_scale = 0x1822e;
  const gint32 scale3 = descriptor_scale * 3;
  const gint64 radius_q16 = ((gint64) scale3 * 0x38916) >> 16;
  const gint32 inverse_scale =
    (gint32) (G_GINT64_CONSTANT (0x1000000000) / scale3);
  guint32 weights[33 * 33] = { 0, };
  guint32 accumulator[6][6][8] = { 0, };
  gint32 sine;
  gint32 cosine;
  gint32 radius = (gint32) ((radius_q16 >> 16) +
                            ((((guint64) radius_q16) & 0x8000) != 0));
  guint size;
  guint32 coefficient;

  radius = MIN (radius, 32);
  size = radius + 1;
  coefficient = (guint32) (G_GINT64_CONSTANT (0x200000000000) /
                           ((gint64) scale3 * scale3));
  build_orientation_weights (weights, size, coefficient);
  descriptor_sine_cosine (feature_orientation, &sine, &cosine);
  cosine = (gint32) (((gint64) cosine * inverse_scale) >> 25);
  sine = (gint32) (((gint64) sine * inverse_scale) >> 25);

  const gint32 min_y = MAX (-radius, 1 - center_y);
  const gint32 max_y = MIN (radius,
                            (gint32) GOODIX_CHICAGO_FEATURE_HEIGHT -
                            center_y - 2);
  const gint32 min_x = MAX (-radius, 1 - center_x);
  const gint32 max_x = MIN (radius,
                            (gint32) GOODIX_CHICAGO_FEATURE_WIDTH -
                            center_x - 2);

  for (gint32 delta_y = min_y; delta_y <= max_y; delta_y++)
    {
      for (gint32 delta_x = min_x; delta_x <= max_x; delta_x++)
        {
          const gint32 rotated_x = delta_x * cosine - delta_y * sine;
          const gint32 rotated_y = delta_x * sine + delta_y * cosine;
          const guint pixel =
            (center_y + delta_y) * GOODIX_CHICAGO_FEATURE_WIDTH +
            center_x + delta_x;
          gint32 relative_orientation;
          gint32 angle;
          gint32 weighted_magnitude;

          if (ABS (rotated_x) >= 0x500 || ABS (rotated_y) >= 0x500)
            continue;
          relative_orientation =
            0x3244 - orientation[pixel] - feature_orientation;
          relative_orientation %= 0x6488;
          if (relative_orientation < 0)
            relative_orientation += 0x6488;
          angle = (gint32) (((guint32) relative_orientation * 0x145f3u) >>
                            16);
          weighted_magnitude = (gint32)
                               (magnitude[pixel] *
                                weights[ABS (delta_y) * size + ABS (delta_x)]);
          descriptor_accumulate (accumulator, rotated_x + 0x300,
                                 rotated_y + 0x300, angle,
                                 weighted_magnitude);
        }
    }

  for (guint y = 0; y < 4; y++)
    for (guint x = 0; x < 4; x++)
      for (guint bin = 0; bin < 8; bin++)
        samples[(y * 4 + x) * 8 + bin] = accumulator[y + 1][x + 1][bin];
}

static guint64
integer_square_root (guint64 value)
{
  guint64 result = 0;
  guint64 bit = G_GUINT64_CONSTANT (1) << 62;

  while (bit > value)
    bit >>= 2;
  while (bit != 0)
    {
      if (value >= result + bit)
        {
          value -= result + bit;
          result = (result >> 1) + bit;
        }
      else
        {
          result >>= 1;
        }
      bit >>= 2;
    }
  return result;
}

static void
normalize_descriptor_samples (const gint32 samples[],
                              guint        count,
                              gint16       normalized[])
{
  guint64 energy = 0;

  for (guint index = 0; index < count; index++)
    energy += (guint64) (guint32) samples[index] *
              (guint32) samples[index];

  const guint32 threshold =
    (guint32) ((integer_square_root (energy) * 0x3333) >> 16);
  const gint16 clipped = (gint16) integer_square_root (threshold);

  for (guint index = 0; index < count; index++)
    normalized[index] = (gint16)
                        (samples[index] < (gint32) threshold ?
                         integer_square_root ((guint32) samples[index]) : clipped);
}

static gint
hadamard_sign (guint row,
               guint column)
{
  return __builtin_parity (row & column) ? -1 : 1;
}

static gint16
descriptor_median (const gint16 values[],
                   guint        count)
{
  gint16 sorted[128];

  memcpy (sorted, values, count * sizeof (*values));
  for (guint index = 1; index < count; index++)
    {
      const gint16 value = sorted[index];
      guint position = index;

      while (position > 0 && value < sorted[position - 1])
        {
          sorted[position] = sorted[position - 1];
          position--;
        }
      sorted[position] = value;
    }
  return sorted[(count - 1) / 2];
}

static void
encode_descriptor_first_pass (const gint16 normalized[128],
                              guint8      *record_bytes)
{
  static const gint coefficient[4][4] = {
    { 1,  1,  1,  1 },
    { 1, -1,  1, -1 },
    { 1,  1, -1, -1 },
    { 1, -1, -1,  1 },
  };
  guint32 transform[4] = { 0, };
  guint32 median_bits = 0;

  for (guint bit = 0; bit < 32; bit++)
    {
      gint16 partial[4];

      for (guint block = 0; block < 4; block++)
        {
          gint32 sum = 0;

          for (guint sample = 0; sample < 32; sample++)
            sum += normalized[block * 32 + sample] *
                   hadamard_sign (bit, sample);
          partial[block] = (gint16) sum;
        }
      for (guint output = 0; output < 4; output++)
        {
          gint32 sum = 0;

          for (guint block = 0; block < 4; block++)
            sum += coefficient[output][block] * partial[block];
          if (sum > 0)
            transform[output] |= 1u << bit;
        }
    }

  const gint16 median = descriptor_median (normalized, 128);
  for (guint bit = 0; bit < 32; bit++)
    if (median < normalized[bit * 4 + 1])
      median_bits |= 1u << bit;
  memset (record_bytes + 0x10, 0, 0x18);
  memcpy (record_bytes + 0x10, transform, sizeof (transform));
  memcpy (record_bytes + 0x20, &median_bits, sizeof (median_bits));
}

static void
encode_descriptor_second_pass (const gint16 normalized[32],
                               guint8      *record_bytes)
{
  guint32 transform = 0;
  guint32 median_bits = 0;
  const gint16 median = descriptor_median (normalized, 32);

  for (guint bit = 0; bit < 32; bit++)
    {
      gint32 sum = 0;

      for (guint sample = 0; sample < 32; sample++)
        sum += normalized[sample] * hadamard_sign (bit, sample);
      if (sum > 0)
        transform |= 1u << bit;
      if (median < normalized[bit])
        median_bits |= 1u << bit;
    }
  memset (record_bytes + 0x28, 0, 0x10);
  memcpy (record_bytes + 0x28, &transform, sizeof (transform));
  memcpy (record_bytes + 0x2c, &median_bits, sizeof (median_bits));
}

static gboolean
descriptor_is_distinctive (const gint32 samples[128],
                           gboolean     edge)
{
  guint32 average[16];
  guint32 average_sum = 0;
  guint low_variance = 0;
  guint high_variance = 0;

  for (guint block = 0; block < 16; block++)
    {
      guint32 sum = 0;

      for (guint sample = 0; sample < 8; sample++)
        sum += samples[block * 8 + sample];
      average[block] = sum >> 3;
      average_sum += average[block];
    }

  const guint32 global_average = average_sum >> 4;
  for (guint block = 0; block < 16; block++)
    {
      guint32 variance_sum = 0;

      if (edge && 3 * average[block] < global_average)
        continue;
      for (guint sample = 0; sample < 8; sample++)
        {
          const gint64 delta =
            (gint64) samples[block * 8 + sample] - average[block];

          variance_sum += (guint32) ((delta * delta) >> 16);
        }
      const guint32 variance = variance_sum / 7;

      if (variance < global_average / 25)
        low_variance++;
      if (variance > global_average / 8)
        high_variance++;
    }
  return (low_variance > 3 && high_variance < 8) || low_variance > 5;
}

void
goodix_chicago_feature_build_descriptor (
  const guint32                  magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
  const gint16                   orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
  const GoodixChicagoFeatureAux *aux,
  GoodixChicagoFeatureRecord    *record)
{
  gint32 samples[128];
  gint32 central_samples[32];
  gint16 normalized[128];
  gint16 central_normalized[32];
  guint8 *record_bytes;
  gboolean edge;

  g_return_if_fail (magnitude != NULL);
  g_return_if_fail (orientation != NULL);
  g_return_if_fail (aux != NULL);
  g_return_if_fail (record != NULL);

  record_bytes = (guint8 *) record;
  edge = !((guint16) record->refined_x > 0x0a00 &&
           (guint16) record->refined_x <
           (guint16) ((GOODIX_CHICAGO_FEATURE_WIDTH + 0xf6) * 0x100) &&
           (guint16) record->refined_y > 0x0a00 &&
           (guint16) record->refined_y <
           (guint16) ((GOODIX_CHICAGO_FEATURE_HEIGHT + 0xf6) * 0x100));

  build_descriptor_samples (aux->x, aux->y, record->orientation, magnitude,
                            orientation, samples);
  normalize_descriptor_samples (samples, 128, normalized);
  encode_descriptor_first_pass (normalized, record_bytes);
  record_bytes[0x38] = descriptor_is_distinctive (samples, edge) ? 2 : 0;

  memcpy (&central_samples[0], &samples[40], 8 * sizeof (gint32));
  memcpy (&central_samples[8], &samples[48], 8 * sizeof (gint32));
  memcpy (&central_samples[16], &samples[72], 8 * sizeof (gint32));
  memcpy (&central_samples[24], &samples[80], 8 * sizeof (gint32));
  normalize_descriptor_samples (central_samples, 32, central_normalized);
  encode_descriptor_second_pass (central_normalized, record_bytes);
}

void
goodix_chicago_feature_finalize_record (
  GoodixChicagoFeatureRecord *record)
{
  gint16 rounded_orientation;
  gint32 boundary;

  g_return_if_fail (record != NULL);

  record->refined_x = (gint16) (((guint16) record->refined_x + 8) & 0xfff0);
  record->refined_y = (gint16) (((guint16) record->refined_y + 8) & 0xfff0);
  if (record->orientation < 0)
    rounded_orientation = (gint16)
                          - ((0x80 - (gint32) record->orientation) & 0xff00);
  else
    rounded_orientation = (gint16)
                          (((gint32) record->orientation + 0x80) & 0xff00);
  record->orientation = rounded_orientation;

  if ((guint16) record->refined_y < 0x1400)
    boundary = 1;
  else if ((guint16) record->refined_y >=
           (guint16) (GOODIX_CHICAGO_FEATURE_HEIGHT * 0x100 - 0x1400))
    boundary = 2;
  else
    boundary = 0;
  memcpy (record->descriptor, &boundary, sizeof (boundary));
}

void
goodix_chicago_feature_build_mask (
  const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint8       mask[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  static const guint32 gaussian[5] = {
    0x0df3, 0x3e84, 0x6712, 0x3e84, 0x0df3,
  };
  guint16 horizontal[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint16 smoothed[GOODIX_CHICAGO_FEATURE_PIXELS];
  gint16 gradient_x[GOODIX_CHICAGO_FEATURE_PIXELS] = { 0, };
  gint16 gradient_y[GOODIX_CHICAGO_FEATURE_PIXELS] = { 0, };
  guint16 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS];

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (mask != NULL);

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dx = -2; dx <= 2; dx++)
          sum += ((guint32) enhanced[y * GOODIX_CHICAGO_FEATURE_WIDTH +
                                     reflect_101 ((gint) x + dx,
                                                  GOODIX_CHICAGO_FEATURE_WIDTH)] << 8) *
                 gaussian[dx + 2];
        horizontal[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] = sum >> 16;
      }
  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        guint64 sum = 0;

        for (gint dy = -2; dy <= 2; dy++)
          sum += (guint32) horizontal[
            reflect_101 ((gint) y + dy,
                         GOODIX_CHICAGO_FEATURE_HEIGHT) *
            GOODIX_CHICAGO_FEATURE_WIDTH + x] * gaussian[dy + 2];
        smoothed[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] = sum >> 16;
      }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      const guint previous_y = reflect_101 ((gint) y - 1,
                                            GOODIX_CHICAGO_FEATURE_HEIGHT);
      const guint next_y = reflect_101 ((gint) y + 1,
                                        GOODIX_CHICAGO_FEATURE_HEIGHT);

      for (guint x = 1; x + 1 < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        gradient_x[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
          ((smoothed[previous_y * GOODIX_CHICAGO_FEATURE_WIDTH + x + 1] >> 8) -
           (smoothed[previous_y * GOODIX_CHICAGO_FEATURE_WIDTH + x - 1] >> 8)) +
          2 * ((smoothed[y * GOODIX_CHICAGO_FEATURE_WIDTH + x + 1] >> 8) -
               (smoothed[y * GOODIX_CHICAGO_FEATURE_WIDTH + x - 1] >> 8)) +
          ((smoothed[next_y * GOODIX_CHICAGO_FEATURE_WIDTH + x + 1] >> 8) -
           (smoothed[next_y * GOODIX_CHICAGO_FEATURE_WIDTH + x - 1] >> 8));
    }
  for (guint y = 1; y + 1 < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        const guint previous_x = reflect_101 ((gint) x - 1,
                                              GOODIX_CHICAGO_FEATURE_WIDTH);
        const guint next_x = reflect_101 ((gint) x + 1,
                                          GOODIX_CHICAGO_FEATURE_WIDTH);

        gradient_y[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
          ((smoothed[(y + 1) * GOODIX_CHICAGO_FEATURE_WIDTH + previous_x] >> 8) -
           (smoothed[(y - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + previous_x] >> 8)) +
          2 * ((smoothed[(y + 1) * GOODIX_CHICAGO_FEATURE_WIDTH + x] >> 8) -
               (smoothed[(y - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + x] >> 8)) +
          ((smoothed[(y + 1) * GOODIX_CHICAGO_FEATURE_WIDTH + next_x] >> 8) -
           (smoothed[(y - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + next_x] >> 8));
      }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    magnitude[pixel] = ABS ((gint) gradient_x[pixel]) / 2 +
                       ABS ((gint) gradient_y[pixel]) / 2;
  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        guint32 sum = 0;

        for (gint dy = -7; dy <= 7; dy++)
          for (gint dx = -7; dx <= 7; dx++)
            sum += magnitude[
              reflect_101 ((gint) y + dy,
                           GOODIX_CHICAGO_FEATURE_HEIGHT) *
              GOODIX_CHICAGO_FEATURE_WIDTH +
              reflect_101 ((gint) x + dx,
                           GOODIX_CHICAGO_FEATURE_WIDTH)];
        mask[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] =
          (((guint64) sum * (0x10000u / 225u)) >> 16) > 110 ? 1 : 0;
      }
}

static gint64
annotation_rectangle_sum (const gint32 *integral,
                          guint         left,
                          guint         top,
                          guint         right,
                          guint         bottom)
{
  gint64 sum = integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + right];

  if (left != 0)
    sum -= integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1];
  if (top != 0)
    sum -= integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + right];
  if (left != 0 && top != 0)
    sum += integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1];
  return sum;
}

void
goodix_chicago_feature_build_annotations (
  const guint8                      enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  const guint8                      mask[GOODIX_CHICAGO_FEATURE_PIXELS],
  const GoodixChicagoFeatureRecord *records,
  guint                             count,
  guint8                           *annotations)
{
  g_autofree gint32 *gradient_x = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  g_autofree gint32 *gradient_y = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  g_autofree gint32 *valid_integral = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  g_autofree gint32 *xx_integral = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  g_autofree gint32 *yy_integral = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);
  g_autofree gint32 *xy_integral = g_new0 (gint32, GOODIX_CHICAGO_FEATURE_PIXELS);

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (mask != NULL);
  g_return_if_fail (records != NULL || count == 0);
  g_return_if_fail (annotations != NULL || count == 0);

  for (guint y = 1; y + 1 < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 1; x + 1 < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        gboolean supported = TRUE;

        for (gint dy = -1; dy <= 1 && supported; dy++)
          for (gint dx = -1; dx <= 1; dx++)
            if (mask[(y + dy) * GOODIX_CHICAGO_FEATURE_WIDTH + x + dx] == 0)
              {
                supported = FALSE;
                break;
              }
        if (supported)
          {
            const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;

            gradient_x[pixel] = enhanced[pixel + 1] - enhanced[pixel - 1];
            gradient_y[pixel] =
              enhanced[pixel + GOODIX_CHICAGO_FEATURE_WIDTH] -
              enhanced[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
          }
      }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      gint32 valid_row = 0;
      gint32 xx_row = 0;
      gint32 yy_row = 0;
      gint32 xy_row = 0;

      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;
          const gint32 gx = gradient_x[pixel];
          const gint32 gy = gradient_y[pixel];
          const gboolean valid = mask[pixel] != 0 && gx * gx + gy * gy >= 25;

          valid_row += valid ? mask[pixel] : 0;
          xx_row += valid ? gx * gx : 0;
          yy_row += valid ? gy * gy : 0;
          xy_row += valid ? gx * gy : 0;
          valid_integral[pixel] = valid_row;
          xx_integral[pixel] = xx_row;
          yy_integral[pixel] = yy_row;
          xy_integral[pixel] = xy_row;
          if (y != 0)
            {
              valid_integral[pixel] += valid_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
              xx_integral[pixel] += xx_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
              yy_integral[pixel] += yy_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
              xy_integral[pixel] += xy_integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
            }
        }
    }

  for (guint index = 0; index < count; index++)
    {
      const guint8 *bytes = (const guint8 *) &records[index];
      const guint left = MAX (1, (gint) bytes[3] - 16);
      const guint top = MAX (1, (gint) bytes[5] - 16);
      const guint right = MIN ((gint) GOODIX_CHICAGO_FEATURE_WIDTH - 2,
                               (gint) bytes[3] + 16);
      const guint bottom = MIN ((gint) GOODIX_CHICAGO_FEATURE_HEIGHT - 2,
                                (gint) bytes[5] + 16);
      const guint area = (right - left + 1) * (bottom - top + 1);
      const gint64 valid = annotation_rectangle_sum (valid_integral,
                                                     left, top, right, bottom);
      gint64 fixed = 0;

      if (valid >= area / 2)
        {
          const gint64 half = valid >> 1;
          const gint64 xx =
            (annotation_rectangle_sum (xx_integral, left, top, right, bottom) + half) / valid;
          const gint64 yy =
            (annotation_rectangle_sum (yy_integral, left, top, right, bottom) + half) / valid;
          const gint64 xy =
            (annotation_rectangle_sum (xy_integral, left, top, right, bottom) + half) / valid;
          const gint64 mean = (xx + yy) / 2;
          const gint64 coherence =
            ((xx * yy - xy * xy) * 0x10000) / (mean * mean + 1);

          fixed = MAX ((gint64) 0, 0x10000 - MAX ((gint64) 0, coherence));
        }
      annotations[index] = (fixed * 100) >> 16;
    }
}

gboolean
goodix_chicago_feature_classify_status_full (
  GoodixChicagoFeatureRecord    *records,
  guint                          count,
  guint                          template_type,
  GoodixChicagoFeatureConsensus *consensus)
{
  gint8 votes[GOODIX_CHICAGO_FEATURE_PIXELS] = { 0, };
  gint32 integral[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint8 classes[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint positive_records = 0;
  guint negative_pixels = 0;
  guint positive_pixels = 0;
  guint neutral_pixels = 0;
  guint inactive_count = 0;

  g_return_val_if_fail (records != NULL || count == 0, FALSE);
  if (consensus)
    memset (consensus, 0, sizeof (*consensus));

  for (guint index = 0; index < count; index++)
    {
      guint8 *bytes = (guint8 *) &records[index];

      if ((gint8) bytes[0x38] > 0)
        {
          bytes[0x38] = 2;
          positive_records++;
        }
    }
  if (positive_records == 0)
    return FALSE;

  for (guint index = 0; index < count; index++)
    {
      const guint8 *bytes = (const guint8 *) &records[index];
      const gint delta = (gint8) bytes[0x38] > 0 ? 1 : -1;
      const gint center_x = bytes[3];
      const gint center_y = bytes[5];

      for (gint y = MAX (0, center_y - 15);
           y < MIN ((gint) GOODIX_CHICAGO_FEATURE_HEIGHT, center_y + 15);
           y++)
        for (gint x = MAX (0, center_x - 15);
             x < MIN ((gint) GOODIX_CHICAGO_FEATURE_WIDTH, center_x + 15);
             x++)
          votes[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] += delta;
    }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      gint32 row_sum = 0;

      for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;

          row_sum += votes[pixel];
          integral[pixel] = row_sum;
          if (y != 0)
            integral[pixel] +=
              integral[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
        }
    }

  for (guint y = 0; y < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
      {
        const guint left = x > 15 ? x - 15 : 0;
        const guint right = MIN (x + 15,
                                 GOODIX_CHICAGO_FEATURE_WIDTH - 1);
        const guint top = y > 15 ? y - 15 : 0;
        const guint bottom = MIN (y + 15,
                                  GOODIX_CHICAGO_FEATURE_HEIGHT - 1);
        const guint area = (right - left + 1) * (bottom - top + 1);
        gint32 sum = integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + right];
        gint average;

        if (left != 0)
          sum -= integral[bottom * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1];
        if (top != 0)
          sum -= integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + right];
        if (left != 0 && top != 0)
          sum += integral[(top - 1) * GOODIX_CHICAGO_FEATURE_WIDTH + left - 1];
        average = (sum + (gint32) (area >> 1)) / (gint32) area;
        if (average < -3)
          {
            classes[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] = 0;
            negative_pixels++;
          }
        else if (average < 2)
          {
            classes[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] = 0x80;
            neutral_pixels++;
          }
        else
          {
            classes[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] = 0xff;
            positive_pixels++;
          }
      }

  for (guint index = 0; index < count; index++)
    {
      guint8 *bytes = (guint8 *) &records[index];
      const guint x = bytes[3];
      const guint y = bytes[5];
      const guint8 classification =
        classes[y * GOODIX_CHICAGO_FEATURE_WIDTH + x];

      if (classification == 0xff && (gint8) bytes[0x38] == 0)
        bytes[0x38] = 3;
      else if (classification == 0 && (gint8) bytes[0x38] > 0)
        bytes[0x38] = 0xff;
      if ((gint8) bytes[0x38] <= 0)
        inactive_count++;
    }

  if (consensus)
    {
      const guint pixels = GOODIX_CHICAGO_FEATURE_PIXELS;
      const guint class_threshold =
        template_type == 7 || template_type == 23 ? 5 : 10;

      consensus->negative_percent = negative_pixels * 100u / pixels;
      consensus->positive_percent = positive_pixels * 100u / pixels;
      consensus->neutral_percent = neutral_pixels * 100u / pixels;
      consensus->inactive_count = inactive_count;
      if (consensus->positive_percent >= class_threshold)
        consensus->density_class =
          consensus->positive_percent >= 65 ? 2 : 1;
    }

  return TRUE;
}

guint
goodix_chicago_feature_filter_mask (
  const guint8                mask[GOODIX_CHICAGO_FEATURE_PIXELS],
  GoodixChicagoFeatureRecord *records,
  guint                       count)
{
  guint index = 0;

  g_return_val_if_fail (mask != NULL, count);
  g_return_val_if_fail (records != NULL || count == 0, count);

  while (index < count)
    {
      const guint x = ((guint16) records[index].refined_x + 0x80u) >> 8;
      const guint y = ((guint16) records[index].refined_y + 0x80u) >> 8;

      if (x < GOODIX_CHICAGO_FEATURE_WIDTH &&
          y < GOODIX_CHICAGO_FEATURE_HEIGHT &&
          mask[y * GOODIX_CHICAGO_FEATURE_WIDTH + x] == 0)
        {
          count--;
          if (index != count)
            records[index] = records[count];
          memset (&records[count], 0, sizeof (records[count]));
          continue;
        }
      index++;
    }

  return count;
}

static guint32
reverse_record_bits (guint32 value)
{
  guint32 reversed = 0;

  for (guint bit = 0; bit < 32; bit++)
    reversed |= ((value >> bit) & 1u) << (31 - bit);
  return reversed;
}

void
goodix_chicago_feature_pack_record (
  GoodixChicagoFeatureRecord *record,
  gboolean                    reverse_bits)
{
  static const guint8 swap_pair[8] = { 0, 1, 1, 0, 1, 0, 0, 1 };
  guint8 packed[24] = { 0, };
  guint8 *bytes;
  guint32 word;

  g_return_if_fail (record != NULL);

  bytes = (guint8 *) record;
  for (guint pair = 0; pair < 8; pair++)
    {
      const guint8 first = bytes[0x10 + pair * 2];
      const guint8 second = bytes[0x11 + pair * 2];
      guint8 low_first = (second & 0xf0u) | (first & 0x0fu);
      guint8 low_second = (first & 0xf0u) | (second & 0x0fu);

      if (swap_pair[pair])
        {
          const guint8 temporary = low_first;

          low_first = low_second;
          low_second = temporary;
        }
      packed[pair] = low_first;
      packed[8 + pair] = low_second;
    }

  memcpy (&word, bytes + 0x20, sizeof (word));
  if (reverse_bits)
    word = reverse_record_bits (word);
  memcpy (packed + 0x10, &word, sizeof (word));
  memcpy (bytes + 0x10, packed, sizeof (packed));

  if (reverse_bits)
    {
      const guint8 tail[8] = {
        bytes[0x28], (guint8) ~bytes[0x29], (guint8) ~bytes[0x2a], bytes[0x2b],
        bytes[0x2f], bytes[0x2e], bytes[0x2d], bytes[0x2c],
      };

      memcpy (bytes + 0x30, tail, sizeof (tail));
    }
}

guint
goodix_chicago_feature_partition_records (
  GoodixChicagoFeatureRecord *records,
  guint8                     *annotations,
  guint                       count)
{
  guint low = 0;
  guint high = count;

  g_return_val_if_fail (records != NULL || count == 0, 0);

  while (low < high)
    {
      while (low < high && (((guint8 *) &records[low])[0] & 3u) == 0)
        low++;
      while (low < high &&
             (((guint8 *) &records[high - 1])[0] & 3u) == 1)
        high--;
      if (low >= high)
        break;

      {
        const GoodixChicagoFeatureRecord temporary = records[low];

        records[low] = records[high - 1];
        records[high - 1] = temporary;
      }
      if (annotations)
        {
          const guint8 temporary = annotations[low];

          annotations[low] = annotations[high - 1];
          annotations[high - 1] = temporary;
        }
    }

  return low;
}

static guint
neighbor_distance_weight (guint squared_distance)
{
  static const guint8 maximum_distance[39] = {
    20, 30, 37, 44, 50, 55, 60, 65, 70, 74,
    78, 83, 87, 91, 95, 99, 103, 108, 112, 116,
    120, 125, 129, 134, 138, 143, 148, 154, 159, 165,
    171, 178, 185, 193, 202, 213, 226, 243, 255,
  };

  for (guint index = 0; index < G_N_ELEMENTS (maximum_distance); index++)
    if (squared_distance <= maximum_distance[index])
      return 39 - index;
  return 0;
}

void
goodix_chicago_feature_score_neighbors (
  GoodixChicagoFeatureRecord *records,
  const guint8               *annotations,
  guint                       count)
{
  g_return_if_fail (records != NULL || count == 0);
  g_return_if_fail (annotations != NULL || count == 0);

  for (guint index = 0; index < count; index++)
    {
      const guint8 *center = (const guint8 *) &records[index];
      guint score = 0;

      for (guint neighbor_index = 0; neighbor_index < count; neighbor_index++)
        {
          const guint8 *neighbor;
          gint delta_x;
          gint delta_y;
          guint squared_distance;
          guint weight;

          if (neighbor_index == index)
            continue;
          neighbor = (const guint8 *) &records[neighbor_index];
          delta_x = (gint) neighbor[3] - center[3];
          delta_y = (gint) center[5] - neighbor[5];
          squared_distance = delta_x * delta_x + delta_y * delta_y;
          if (squared_distance == 0 || squared_distance >= 0x100)
            continue;

          weight = neighbor_distance_weight (squared_distance);
          if ((gint8) annotations[neighbor_index] > 50)
            weight = (weight * 12) >> 3;
          score += weight;
        }

      ((guint8 *) &records[index])[0x39] = MIN (score >> 3, 100u);
    }
}

void
goodix_chicago_feature_build_gradient_input (
  const guint8 feature_image[GOODIX_CHICAGO_FEATURE_PIXELS],
  gint32       input[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  guint16 lifted[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint16 filtered0[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint16 filtered1[GOODIX_CHICAGO_FEATURE_PIXELS];

  g_return_if_fail (feature_image != NULL);
  g_return_if_fail (input != NULL);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    lifted[pixel] = (guint16) feature_image[pixel] << 8;

  filter_q16 (lifted, filtered0, &gradient_input_kernels[0]);
  filter_q16 (filtered0, filtered1, &gradient_input_kernels[1]);
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    input[pixel] = 10 * (gint32) filtered0[pixel] -
                   9 * (gint32) filtered1[pixel];
}

void
goodix_chicago_feature_build_gradients (
  const gint32 input[GOODIX_CHICAGO_FEATURE_PIXELS],
  guint32      magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
  gint16       orientation[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  g_return_if_fail (input != NULL);
  g_return_if_fail (magnitude != NULL);
  g_return_if_fail (orientation != NULL);

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_FEATURE_PIXELS; pixel++)
    {
      magnitude[pixel] = 0;
      orientation[pixel] = 0;
    }

  for (guint y = 1; y + 1 < GOODIX_CHICAGO_FEATURE_HEIGHT; y++)
    {
      for (guint x = 1; x + 1 < GOODIX_CHICAGO_FEATURE_WIDTH; x++)
        {
          const guint pixel = y * GOODIX_CHICAGO_FEATURE_WIDTH + x;
          gint32 horizontal = input[pixel + 1] - input[pixel - 1];
          gint32 vertical =
            input[pixel + GOODIX_CHICAGO_FEATURE_WIDTH] -
            input[pixel - GOODIX_CHICAGO_FEATURE_WIDTH];
          guint16 angle;

          horizontal = CLAMP (horizontal, -0x80000, 0x7ffff) * 0x1000;
          vertical = CLAMP (vertical, -0x80000, 0x7ffff) * 0x1000;
          angle = cordic_magnitude_orientation (vertical, &horizontal);
          orientation[pixel] = (gint16) angle;
          magnitude[pixel] = MIN ((guint32) (horizontal >> 12), 0x3ffffu);
        }
    }
}

/* ------------------------------------------------------------------------
 * Feature extraction for template type 24: the steps around the candidate
 * pipeline above.
 * ------------------------------------------------------------------------ */

#define CHICAGO_W GOODIX_CHICAGO_FEATURE_WIDTH
#define CHICAGO_H GOODIX_CHICAGO_FEATURE_HEIGHT
#define CHICAGO_N GOODIX_CHICAGO_FEATURE_PIXELS

void
goodix_chicago_feature_state_init (GoodixChicagoFeatureState *state)
{
  g_return_if_fail (state != NULL);
  memset (state, 0, sizeof (*state));
}

/* The preprocessor marks its output in the least significant
 * bits of the first row.  Returns TRUE when the per-pixel region
 * plane (LSB of every pixel below row 0) is present; *packed receives the code
 * even when it is not. */
gboolean
goodix_chicago_feature_decode_mark (const guint8 image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                    guint32     *packed,
                                    guint8       region[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  const guint signature = CHICAGO_W - 8;
  guint value;

  for (guint x = 0; x < signature; x += 2)
    if (image[x] & 1)
      return FALSE;
  for (guint x = 1; x < signature; x += 2)
    if (!(image[x] & 1))
      return FALSE;

  value = (image[CHICAGO_W - 8] & 1) + 2 * (image[CHICAGO_W - 7] & 1);
  *packed = value == 1 ? 1 : value == 2 ? 3 : value == 3 ? 2 : 0;
  *packed += ((image[CHICAGO_W - 5] & 1) + 2 * (image[CHICAGO_W - 4] & 1) +
              4 * (image[CHICAGO_W - 3] & 1)) << 8;
  if (!(image[CHICAGO_W - 6] & 1))
    return FALSE;
  memset (region, 1, CHICAGO_W);
  for (guint pixel = CHICAGO_W; pixel < CHICAGO_N; pixel++)
    region[pixel] = image[pixel] & 1;
  return TRUE;
}

/* Jump tables. */
static gint
resolution_class (gint value)
{
  static const gint8 map[9] = { 2, 2, 3, 0, 1, 2, 4, 5, 5 };

  return value >= 1 && value <= 9 ? map[value - 1] : 0;
}

/* 4-connected flood-fill labelling with a 0x992-entry FIFO,
 * margins 0, at most 70 + 1 labels; sizes[label]++ per pixel. */
static gint
label_components (const guint8 *image,
                  gint32       *labels,
                  gint32        sizes[100],
                  gint          max_labels)
{
  static const gint dx[4] = { -1, 0, 1, 0 };
  static const gint dy[4] = { 0, -1, 0, 1 };

  enum { QUEUE = 0x992 };
  gint32 (*queue)[2] = g_malloc_n (QUEUE, sizeof (*queue));
  gint count = 0, head = 0, tail = 0;
  gint label = 1;
  gboolean stop = FALSE;

  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    labels[pixel] = -1;
  for (gint y = 0; y < (gint) CHICAGO_H && !stop; y++)
    for (gint x = 0; x < (gint) CHICAGO_W && !stop; x++)
      {
        const gint pixel = y * CHICAGO_W + x;
        gint iterations = 0;

        if (labels[pixel] != -1)
          continue;
        if (image[pixel] == 0)
          {
            labels[pixel] = 0;
            continue;
          }
        if (count < QUEUE)
          {
            queue[tail][0] = x;
            queue[tail][1] = y;
            tail = tail + 1 == QUEUE ? 0 : tail + 1;
            count++;
          }
        labels[pixel] = label;
        sizes[label]++;
        while (count > 0 && iterations <= 0x4c90)
          {
            gint px, py;

            iterations++;
            px = queue[head][0];
            py = queue[head][1];
            count--;
            head = head + 1 == QUEUE ? 0 : head + 1;
            for (guint k = 0; k < 4; k++)
              {
                const gint nx = px + dx[k], ny = py + dy[k];
                gint neighbor;

                if (ny < 0 || ny >= (gint) CHICAGO_H || nx < 0 || nx >= (gint) CHICAGO_W)
                  continue;
                neighbor = ny * CHICAGO_W + nx;
                if (labels[neighbor] != -1)
                  continue;
                if (image[neighbor] == 0)
                  {
                    labels[neighbor] = 0;
                    continue;
                  }
                labels[neighbor] = label;
                sizes[label]++;
                if (count < QUEUE)
                  {
                    queue[tail][0] = nx;
                    queue[tail][1] = ny;
                    tail = tail + 1 == QUEUE ? 0 : tail + 1;
                    count++;
                  }
              }
          }
        label++;
        if (label > max_labels + 1)
          stop = TRUE;
      }
  g_free (queue);
  return label - 1;
}

/* The k largest labels with more than `threshold` pixels. */
static void
select_components (gint32 sizes[100],
                   gint   label_count,
                   gint32 selected[],
                   gint   k,
                   gint   threshold)
{
  gint32 saved[10] = { 0, };

  if (k > 10 || k <= 0)
    return;
  for (gint j = 0; j < k; j++)
    {
      gint best = threshold;

      for (gint label = 0; label <= label_count; label++)
        if (sizes[label] > best)
          {
            best = sizes[label];
            selected[j] = label;
          }
      if (selected[j] > 0)
        {
          saved[j] = sizes[selected[j]];
          sizes[selected[j]] = 0;
        }
    }
  for (gint j = 0; j < k; j++)
    if (selected[j] > 0)
      sizes[selected[j]] = saved[j];
}

/* Keep the 3 largest components (> 350 px) of `image` in place;
 * with three frames of history, `out` receives `value` where the component
 * survived in all of them (again the 3 largest > 350 px). */
static void
select_stable_components (GoodixChicagoFeatureState *state,
                          guint8                    *image,
                          guint8                     value,
                          guint8                    *out)
{
  g_autofree gint32 *labels = g_new (gint32, CHICAGO_N);
  g_autofree guint8 *votes = g_new0 (guint8, CHICAGO_N);
  gint32 sizes[100] = { 0, };
  gint32 selected[8] = { -2, -2, -2, -2, -2, -2, -2, -2 };
  const gint threshold = CHICAGO_N < 6000 ? 350 : 500;
  gint count;

  count = label_components (image, labels, sizes, 70);
  select_components (sizes, count, selected, 3, threshold);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    {
      image[pixel] = 0;
      for (guint j = 0; j < 3; j++)
        if (labels[pixel] == selected[j])
          image[pixel] = 0xff;
    }
  if (state->history_count < 3)
    return;

  for (guint slot = 0; slot < 3; slot++)
    for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
      if (image[pixel] > 0 && state->history[slot][pixel] == value)
        votes[pixel]++;
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    votes[pixel] = votes[pixel] > 2 ? 0xff : 0;
  memset (sizes, 0, sizeof (sizes));
  count = label_components (votes, labels, sizes, 0x1e);
  selected[0] = selected[1] = selected[2] = -2;
  select_components (sizes, count, selected, 3, 350);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    if (out[pixel] == 0)
      out[pixel] = labels[pixel] == selected[0] || labels[pixel] == selected[1] ||
                   labels[pixel] == selected[2] ? value : 0;
}

/* Iterative 3x3 gap filling (256-entry neighbourhood table). */
static void
bridge_gaps (guint8 *image)
{
  static const guint8 table[256] = {
    0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1,
    0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1,
    0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0,
  };
  const gint w = CHICAGO_W, h = CHICAGO_H;
  gboolean changed;

#define CHICAGO_BRIDGE_INDEX(p) \
  (image[(p) + w - 1] + 2 * image[(p) + w] + 4 * image[(p) + w + 1] + 8 * image[(p) - 1] + \
   16 * image[(p) + 1] + 32 * image[(p) - w - 1] + 64 * image[(p) - w] + 128 * image[(p) - w + 1])
  do
    {
      changed = FALSE;
      for (gint y = 1; y < h - 1; y++)
        for (gint x = 1; x < w - 1; x++)
          {
            const gint p = y * w + x;

            if (image[p] != 0 || (image[p - 1] != 1 && image[p + 1] != 1))
              continue;
            if (table[CHICAGO_BRIDGE_INDEX (p)] == 1)
              {
                image[p] = 1;
                changed = TRUE;
                x++;
              }
          }
      for (gint x = 1; x < w - 1; x++)
        for (gint y = 1; y < h - 1; y++)
          {
            const gint p = y * w + x;

            if (image[p] != 0 || (image[p - w] != 1 && image[p + w] != 1))
              continue;
            if (table[CHICAGO_BRIDGE_INDEX (p)] == 1)
              {
                image[p] = 1;
                changed = TRUE;
                y++;
              }
          }
    }
  while (changed);
#undef CHICAGO_BRIDGE_INDEX
}

/* Type 24: the live auxiliary record [0..5] and the final packed
 * resolution, using the cross-frame state. */
static void
build_live_auxiliary (GoodixChicagoFeatureState *state,
                      const guint8              *prepared,
                      const guint8              *mask,
                      const guint8               context[6],
                      guint                      coverage,
                      gint                       class1,
                      gint                       class2,
                      guint32                   *packed,
                      guint8                     auxiliary[6])
{
  g_autofree guint8 *work = g_new (guint8, CHICAGO_N);
  g_autofree guint8 *classes = g_new0 (guint8, CHICAGO_N);
  g_autofree guint8 *stable = g_new0 (guint8, CHICAGO_N);
  g_autofree guint8 *bright = g_new (guint8, CHICAGO_N);
  const gint mode = context[1];
  const gint state0 = context[0], state2 = context[2], signal = context[4];
  gint count1 = 0, count2 = 0, dark = 0;
  gint peak = state0, level = class2;
  gboolean spread = FALSE;

  if (mode != 2)
    state->last_coverage = coverage;
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    work[pixel] = prepared[pixel] < 0x80 ? 0xff : 0;
  select_stable_components (state, work, 1, stable);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    if (work[pixel] > 0)
      classes[pixel] = 1;
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    work[pixel] = prepared[pixel] >= 0x80 && mask[pixel] > 0 ? 0xff : 0;
  select_stable_components (state, work, 2, stable);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    if (work[pixel] > 0)
      classes[pixel] = 2;
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    bright[pixel] = prepared[pixel] >= 0x80;
  bridge_gaps (bright);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    {
      if (stable[pixel] == 1)
        count1++;
      else if (stable[pixel] == 2)
        count2++;
      if (bright[pixel] == 0 && prepared[pixel] < 30)
        dark++;
    }

  if (count1 > 300 || count2 > 300 || count1 + count2 > 400)
    {
      spread = TRUE;
      peak = MAX (state0 + 1, 3);
      level = MAX (level, 4);
      if (dark * 100 <= signal << 8)
        peak = 6;
      else if (dark * 100 <= signal * 300)
        peak = 5;
    }
  if (dark * 100 <= signal << 8)
    peak = MAX (peak, 2);

  if (mode != 2)
    {
      state->last_level = level;
      if (state->stable_count < 5 && (level > 1 || peak > 1 || state2 > 1))
        state->stable_count++;
      if (state->stable_count > 0 && level < 2 && peak < 2 && state2 < 2)
        state->stable_count--;
    }
  else
    {
      level = MAX (level, state->last_level);
      state->last_level = level;
    }
  if (state->stable_count >= 3)
    level = MAX (level, 2);
  *packed = ((guint32) level << 8) + (guint32) class1;

  if (coverage >= 75 && (mode != 2 || state->last_coverage < 75))
    {
      if (state->history_count >= 3)
        {
          memmove (state->history[0], state->history[1], sizeof (state->history[0]));
          memmove (state->history[1], state->history[2], sizeof (state->history[0]));
          memcpy (state->history[2], classes, CHICAGO_N);
        }
      else
        {
          memcpy (state->history[state->history_count], classes, CHICAGO_N);
        }
      state->history_count = MIN (state->history_count + 1, 3);
    }

  auxiliary[0] = (guint8) peak;
  auxiliary[1] = (guint8) mode;
  auxiliary[2] = (guint8) state2;
  auxiliary[3] = (guint8) (class2 + 4);
  auxiliary[4] = 0;
  auxiliary[5] = (guint8) spread;
}

/* Status classification; `weak` = cbuf[0] lets records with a
 * low coherence annotation vote as positive.  Returns FALSE (leaving
 * `consensus` untouched) when nothing votes. */
static gboolean
classify_status (GoodixChicagoFeatureRecord    *records,
                 guint                          count,
                 const guint8                  *annotations,
                 gint                           weak,
                 GoodixChicagoFeatureConsensus *consensus)
{
  guint positive = 0;

  for (guint index = 0; index < count; index++)
    {
      guint8 *bytes = (guint8 *) &records[index];
      const gint8 annotation = (gint8) annotations[index];

      if ((gint8) bytes[0x38] > 0 || (weak > 2 && annotation < 30) ||
          (weak > 1 && annotation < 20))
        {
          bytes[0x38] = 2;
          positive++;
        }
    }
  if (positive == 0)
    return FALSE;
  /* The positive marks are already in place, so the pass sees at
   * least one positive record and only redoes the (idempotent) marking. */
  return goodix_chicago_feature_classify_status_full (records, count, 24, consensus);
}

/* Region filter, mode 2: when the region plane has at least 50
* zero pixels, drop records with more than one zero pixel in their clipped
* 5x5 neighbourhood (tail-swap removal, as the mask filter). */
static guint
filter_region (const guint8               *region,
               GoodixChicagoFeatureRecord *records,
               guint                       count)
{
  const gint w = CHICAGO_W, h = CHICAGO_H, radius = 2;
  g_autofree guint16 *integral = g_new0 (guint16, (w + 1) * (h + 1));
  g_autofree guint16 *zeros = g_new0 (guint16, CHICAGO_N);
  guint zero_count = 0;
  guint index = 0;

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        const guint16 indicator = region[y * w + x] == 0;

        zero_count += indicator;
        integral[(y + 1) * (w + 1) + x + 1] =
          integral[y * (w + 1) + x + 1] - integral[y * (w + 1) + x] +
          integral[(y + 1) * (w + 1) + x] + indicator;
      }
  if (zero_count == 0 || zero_count < 50)
    return count;
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        const gint left = x - radius > 0 ? x - radius : 0;
        const gint right = MIN (x + radius + 1, w);
        const gint top = y - radius > 0 ? y - radius : 0;
        const gint bottom = MIN (y + radius + 1, h);

        zeros[y * w + x] = (guint16) (integral[top * (w + 1) + left] -
                                      integral[top * (w + 1) + right] -
                                      integral[bottom * (w + 1) + left] +
                                      integral[bottom * (w + 1) + right]);
      }
  while (index < count)
    {
      const gint x = ((guint16) records[index].refined_x + 0x80) >> 8;
      const gint y = ((guint16) records[index].refined_y + 0x80) >> 8;

      if (x < w && y < h && zeros[y * w + x] > 1)
        {
          count--;
          if (index != count)
            records[index] = records[count];
          memset (&records[count], 0, sizeof (records[count]));
          continue;
        }
      index++;
    }
  return count;
}

/* Mode 1 (identify) also stores the bit-reversed word at +0x24
 * and the "shifted" tail at +0x30 used by the matcher's 180-degree variant. */
void
goodix_chicago_feature_pack_record_mode (GoodixChicagoFeatureRecord *record,
                                         gboolean                    identify)
{
  guint8 *bytes = (guint8 *) record;
  guint32 word;

  memcpy (&word, bytes + 0x20, sizeof (word));
  goodix_chicago_feature_pack_record (record, FALSE);
  if (!identify)
    return;
  {
    guint32 reversed = 0;
    const guint8 tail[8] = {
      bytes[0x28], (guint8) ~bytes[0x29], (guint8) ~bytes[0x2a], bytes[0x2b],
      bytes[0x2f], bytes[0x2e], bytes[0x2d], bytes[0x2c],
    };

    for (guint bit = 0; bit < 32; bit++)
      reversed |= ((word >> bit) & 1u) << (31 - bit);
    memcpy (bytes + 0x24, &reversed, sizeof (reversed));
    memcpy (bytes + 0x30, tail, sizeof (tail));
  }
}

/* Retry: when the first pass (kernel selectors 0/1) collects at
 * most 60 candidates, the gradient input is rebuilt with selectors 100/101
 * and the candidates are collected again; the scale space
 * of type 24 (selectors 0x12c..0x134) does not depend on the pass. */
static const ChicagoKernel gradient_input_kernels_retry[2] = {
  { 9, { 0x0036, 0x02f5, 0x1393, 0x3cda, 0x58d0, 0x3cda, 0x1393, 0x02f5,
         0x0036 } },
  { 7, { 0x005a, 0x08d4, 0x3ce1, 0x73e2, 0x3ce1, 0x08d4, 0x005a } },
};

static void
build_gradient_input_retry (const guint8 feature_image[GOODIX_CHICAGO_FEATURE_PIXELS],
                            gint32       input[GOODIX_CHICAGO_FEATURE_PIXELS])
{
  g_autofree guint16 *lifted = g_new (guint16, CHICAGO_N);
  g_autofree guint16 *filtered0 = g_new (guint16, CHICAGO_N);
  g_autofree guint16 *filtered1 = g_new (guint16, CHICAGO_N);

  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    lifted[pixel] = (guint16) feature_image[pixel] << 8;
  filter_q16 (lifted, filtered0, &gradient_input_kernels_retry[0]);
  filter_q16 (filtered0, filtered1, &gradient_input_kernels_retry[1]);
  for (guint pixel = 0; pixel < CHICAGO_N; pixel++)
    input[pixel] = 10 * (gint32) filtered0[pixel] - 9 * (gint32) filtered1[pixel];
}

/* For type 24: as goodix_chicago_feature_collect_candidates(),
 * except that (1) a rejected extremum keeps the strength the refinement left
 * in the candidate (it is updated before the determinant test), and (2) the
 * rejected extrema are only used when fewer than 60 records were found and
 * both quality and coverage exceed 80. */
static guint
collect_candidates (const guint8                feature_source[GOODIX_CHICAGO_FEATURE_PIXELS],
                    const guint16               scales[GOODIX_CHICAGO_FEATURE_SCALES]
                    [GOODIX_CHICAGO_FEATURE_PIXELS],
                    const guint32               magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                    const gint16                orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
                    guint                       quality,
                    guint                       coverage,
                    GoodixChicagoFeatureRecord *records,
                    GoodixChicagoFeatureRank   *ranks,
                    GoodixChicagoFeatureAux    *aux,
                    guint                       capacity)
{
  enum { FALLBACK_CAPACITY = 600 };
  g_autofree GoodixChicagoExtremum *extrema = NULL;
  g_autofree GoodixChicagoCandidate *fallback = g_new0 (GoodixChicagoCandidate, FALLBACK_CAPACITY);
  guint8 visited[GOODIX_CHICAGO_FEATURE_PIXELS] = { 0, };
  guint extrema_count;
  guint fallback_count = 0;
  guint count = 0;

  extrema_count = goodix_chicago_feature_collect_extrema (scales, NULL, 0);
  extrema = g_new (GoodixChicagoExtremum, MAX (extrema_count, 1));
  goodix_chicago_feature_collect_extrema (scales, extrema, extrema_count);

  for (guint index = 0; index < extrema_count; index++)
    {
      const GoodixChicagoExtremum *extremum = &extrema[index];
      GoodixChicagoCandidate candidate = {
        .x = extremum->x,
        .y = extremum->y,
        .scale = extremum->scale,
        .strength = extremum->response,
      };
      guint32 curvature = 0;

      if (goodix_chicago_feature_refine_extremum (scales, &candidate, &curvature))
        {
          const guint pixel = candidate.y * CHICAGO_W + candidate.x;

          if (visited[pixel] != 0)
            continue;
          if (count < capacity)
            {
              goodix_chicago_feature_materialize_candidate (
                feature_source, magnitude, orientation, &candidate, count,
                &records[count], &ranks[count], &aux[count]);
              count++;
            }
          visited[pixel] = 1;
        }
      else if (fallback_count < FALLBACK_CAPACITY)
        {
          const guint32 scale_value = chicago_exp2_q16 ((extremum->scale << 16) / 6);

          fallback[fallback_count++] = (GoodixChicagoCandidate){
            .x = extremum->x,
            .y = extremum->y,
            .scale = extremum->scale,
            .strength = candidate.strength,
            .refined_x = (gint16) (extremum->x << 8),
            .refined_y = (gint16) (extremum->y << 8),
            .scale_value = (gint32) (((guint64) scale_value * 0x13333) >> 16),
          };
        }
    }

  if (count < 60 && quality > 80 && coverage > 80)
    {
      for (guint index = 0; index < fallback_count && count < 120; index++)
        {
          const GoodixChicagoCandidate *candidate = &fallback[index];
          const guint pixel = candidate->y * CHICAGO_W + candidate->x;

          if (visited[pixel] != 0)
            continue;
          if (count < capacity)
            {
              goodix_chicago_feature_materialize_candidate (
                feature_source, magnitude, orientation, candidate, count,
                &records[count], &ranks[count], &aux[count]);
              count++;
            }
          visited[pixel] = 1;
        }
    }
  return count;
}


static void
score_neighbors (GoodixChicagoFeatureRecord *records,
                 guint                       count,
                 guint                       density_class,
                 const guint8               *annotations,
                 const guint8               *secondary,
                 guint8                     *secondary_score)
{
  if (density_class == 0)
    {
      for (guint index = 0; index < count; index++)
        ((guint8 *) &records[index])[0x39] = 0;
      return;
    }
  goodix_chicago_feature_score_neighbors (records, annotations, count);
  for (guint index = 0; index < count; index++)
    {
      const guint8 *center = (const guint8 *) &records[index];
      gint sum = 0, samples = 1;

      for (guint other = 0; other < count; other++)
        {
          const guint8 *neighbor = (const guint8 *) &records[other];
          const gint dx = (gint) neighbor[3] - center[3];
          const gint dy = (gint) center[5] - neighbor[5];
          const gint squared = dx * dx + dy * dy;

          if (other == index || squared == 0 || squared > 0xff)
            continue;
          sum += (gint8) secondary[other];
          samples += 100;
        }
      secondary_score[index] = (guint8) (sum * 100 / samples);
    }
}

guint
goodix_chicago_feature_get_feature (GoodixChicagoFeatureState     *state,
                                    const guint8                   image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                    const guint8                   context[6],
                                    guint                          quality,
                                    guint                          coverage,
                                    gboolean                       identify,
                                    GoodixChicagoFeatureRecord    *records,
                                    guint                          capacity,
                                    guint                         *active_count,
                                    GoodixChicagoFeatureConsensus *consensus,
                                    guint32                       *packed_resolution,
                                    guint8                         live_auxiliary[6])
{
  enum { COLLECTION_CAPACITY = 600 };
  g_autofree guint16 (*scales)[GOODIX_CHICAGO_FEATURE_PIXELS] = NULL;
  g_autofree guint8 *prepared = g_new (guint8, CHICAGO_N);
  g_autofree guint8 *feature_source = g_new (guint8, CHICAGO_N);
  g_autofree guint8 *mask = g_new (guint8, CHICAGO_N);
  g_autofree guint8 *region = g_new (guint8, CHICAGO_N);
  g_autofree gint32 *gradient_input = g_new (gint32, CHICAGO_N);
  g_autofree guint32 *magnitude = g_new (guint32, CHICAGO_N);
  g_autofree gint16 *orientation = g_new (gint16, CHICAGO_N);
  g_autofree GoodixChicagoFeatureRecord *collected = g_new0 (GoodixChicagoFeatureRecord, COLLECTION_CAPACITY);
  g_autofree GoodixChicagoFeatureRank *ranks = g_new0 (GoodixChicagoFeatureRank, COLLECTION_CAPACITY);
  g_autofree GoodixChicagoFeatureAux *collected_aux = g_new0 (GoodixChicagoFeatureAux, COLLECTION_CAPACITY);
  g_autofree GoodixChicagoFeatureAux *selected_aux = g_new0 (GoodixChicagoFeatureAux, MAX (capacity, 1));
  guint8 annotations[GOODIX_CHICAGO_FEATURE_RECORD_LIMIT] = { 0, };
  guint8 secondary[GOODIX_CHICAGO_FEATURE_RECORD_LIMIT] = { 0, };
  guint8 secondary_score[GOODIX_CHICAGO_FEATURE_RECORD_LIMIT] = { 0, };
  GoodixChicagoFeatureConsensus fresh;
  guint32 packed = 0;
  gboolean has_region;
  gint class1, class2;
  guint count, active;

  g_return_val_if_fail (state != NULL && image != NULL && context != NULL, 0);
  g_return_val_if_fail (capacity <= GOODIX_CHICAGO_FEATURE_RECORD_LIMIT, 0);

  has_region = goodix_chicago_feature_decode_mark (image, &packed, region);
  class1 = resolution_class ((gint) (packed & 3));
  class2 = (packed >> 8) & 7;
  class2 = resolution_class (class2 > 0 ? class2 + 4 : 0);
  if (context[3] > 0)
    class2 = resolution_class (context[3]);

  goodix_chicago_feature_build_mask (image, mask);
  goodix_chicago_feature_build_prepared_image (image, prepared);
  goodix_chicago_feature_build_source (image, feature_source);
  build_live_auxiliary (state, prepared, mask, context, coverage, class1, class2,
                        &packed, live_auxiliary);

  /* The extraction front half. */
  scales = g_malloc_n (GOODIX_CHICAGO_FEATURE_SCALES, sizeof (*scales));
  goodix_chicago_feature_build_scale_space (image, scales);
  goodix_chicago_feature_build_gradient_input (prepared, gradient_input);
  goodix_chicago_feature_build_gradients (gradient_input, magnitude, orientation);
  count = collect_candidates (
    feature_source, (const guint16 (*)[GOODIX_CHICAGO_FEATURE_PIXELS]) scales,
    magnitude, orientation, quality, coverage, collected, ranks, collected_aux,
    COLLECTION_CAPACITY);
  if (count <= 60)
    {
      build_gradient_input_retry (prepared, gradient_input);
      goodix_chicago_feature_build_gradients (gradient_input, magnitude, orientation);
      memset (collected, 0, COLLECTION_CAPACITY * sizeof (*collected));
      memset (ranks, 0, COLLECTION_CAPACITY * sizeof (*ranks));
      memset (collected_aux, 0, COLLECTION_CAPACITY * sizeof (*collected_aux));
      count = collect_candidates (
        feature_source, (const guint16 (*)[GOODIX_CHICAGO_FEATURE_PIXELS]) scales,
        magnitude, orientation, quality, coverage, collected, ranks, collected_aux,
        COLLECTION_CAPACITY);
    }
  if (count > capacity)
    {
      for (guint index = 1; index < count; index++)
        {
          const GoodixChicagoFeatureRank rank = ranks[index];
          guint position = index;

          while (position > 0 &&
                 (ranks[position - 1].strength > rank.strength ||
                  (ranks[position - 1].strength == rank.strength &&
                   ranks[position - 1].index > rank.index)))
            {
              ranks[position] = ranks[position - 1];
              position--;
            }
          ranks[position] = rank;
        }
      count = capacity;
      for (guint index = 0; index < count; index++)
        {
          records[index] = collected[ranks[index].index];
          selected_aux[index] = collected_aux[ranks[index].index];
        }
    }
  else
    {
      for (guint index = 0; index < count; index++)
        {
          records[index] = collected[index];
          selected_aux[index] = collected_aux[index];
        }
    }
  for (guint index = 0; index < count; index++)
    {
      goodix_chicago_feature_build_descriptor (magnitude, orientation,
                                               &selected_aux[index], &records[index]);
      goodix_chicago_feature_finalize_record (&records[index]);
    }

  /* Annotations, status and neighbour scores */
  goodix_chicago_feature_build_annotations (image, mask, records, count, annotations);
  /* identify reuses one feature object, enrolment allocates a zeroed
   * one per call: without votes the identify object keeps the previous values. */
  if (!identify)
    memset (&fresh, 0, sizeof (fresh));
  classify_status (records, count, annotations, context[0],
                   identify ? &state->consensus : &fresh);
  count = goodix_chicago_feature_filter_mask (mask, records, count);
  if (has_region)
    count = filter_region (region, records, count);
  for (guint index = 0; index < count; index++)
    goodix_chicago_feature_pack_record_mode (&records[index], identify);
  active = goodix_chicago_feature_partition_records (records, annotations, count);

  {
    const GoodixChicagoFeatureConsensus *current = identify ? &state->consensus : &fresh;

    if (current->density_class == 1)
      {
        goodix_chicago_feature_build_annotations (prepared, mask, records, count, secondary);
        score_neighbors (records, count, 1, annotations, secondary, secondary_score);
        for (guint index = 0; index < count; index++)
          {
            guint8 *bytes = (guint8 *) &records[index];

            if (bytes[0x38] != 0 && (gint8) annotations[index] > 30 &&
                (gint8) secondary_score[index] > 65 && (gint8) secondary[index] > 65 &&
                (gint8) bytes[0x39] > 35)
              bytes[0x38] = 0;
          }
      }
    else
      {
        score_neighbors (records, count, current->density_class, annotations,
                         secondary, secondary_score);
      }
    if (consensus)
      *consensus = *current;
  }

  if (active_count)
    *active_count = active;
  if (packed_resolution)
    *packed_resolution = packed;
  return count;
}
