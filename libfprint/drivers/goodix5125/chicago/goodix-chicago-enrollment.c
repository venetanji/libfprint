// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: enrolment merge, group handling and template
 * study; unused code removed. */

/* Chicago enrollment state. */

#include <gio/gio.h>
#include <string.h>

#include "goodix-chicago-enrollment.h"

typedef struct
{
  guint                       record_count;
  guint                       active_count;
  guint                       quality;
  guint                       coverage;
  GoodixChicagoFeatureRecord *records;
  gboolean                    has_metric_data;
  GoodixChicagoMetricData     metric_data;
  guint                       group_state;
  guint                       relation_base;
  guint                       study_state;
  guint                       study_flags;
  guint                       lineage_index;
  guint                       replacement_count;
  guint                       study_value_a;
  guint                       study_value_b;
} GoodixChicagoSubtemplate;

struct _GoodixChicagoEnrollment
{
  guint      required_samples;
  guint      capacity;
  guint      record_limit;
  GPtrArray *subtemplates;
  GArray    *relations;
  guint      transform_count;
  guint      group_anchor;
  gboolean   has_group_anchor;
  guint32    match_order[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  guint32    matcher_active_index;
  guint32    matcher_value_a;
  guint32    matcher_value_b;
  guint32    matcher_value_c;
  guint32    matcher_value_d;
};

G_STATIC_ASSERT (sizeof (GoodixChicagoRelation) == 0x1c);

gboolean
goodix_chicago_enrollment_drop_last (
  GoodixChicagoEnrollment *self,
  GError                 **error)
{
  GoodixChicagoSubtemplate *last;
  guint old_count;
  guint relation_base;

  g_return_val_if_fail (self != NULL, FALSE);
  if (self->subtemplates->len == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: cannot drop from an empty Chicago enrollment");
      return FALSE;
    }

  old_count = self->subtemplates->len;
  last = g_ptr_array_index (self->subtemplates,
                            self->subtemplates->len - 1);
  relation_base = last->relation_base;
  g_array_set_size (self->relations, relation_base);
  g_ptr_array_remove_index (self->subtemplates,
                            self->subtemplates->len - 1);
  /* Dropping the last frame leaves the match order as it is (the slot
   * of the dropped frame keeps its stale value in the packed template);
   * dropping the group anchor clears the group flag and every group state. */
  self->transform_count = self->relations->len;
  if (old_count - 1 == self->group_anchor)
    {
      self->has_group_anchor = FALSE;
      for (guint index = 0; index < self->subtemplates->len; index++)
        ((GoodixChicagoSubtemplate *) g_ptr_array_index (self->subtemplates, index))->group_state = 0;
    }
  return TRUE;
}

typedef struct
{
  guint old_index;
  guint new_index;
  guint best_distance;
  guint second_distance;
} CorrespondenceCandidate;

static guint
descriptor_distance (const GoodixChicagoFeatureRecord *a,
                     const GoodixChicagoFeatureRecord *b)
{
  guint distance = 0;

  /* Starts at record byte 0x10, four bytes into descriptor[]. */
  for (guint offset = 4; offset < 28; offset += sizeof (guint32))
    {
      guint32 a_word;
      guint32 b_word;

      memcpy (&a_word, a->descriptor + offset, sizeof (a_word));
      memcpy (&b_word, b->descriptor + offset, sizeof (b_word));
      distance += __builtin_popcount (a_word ^ b_word);
    }
  return distance;
}

guint
goodix_chicago_enrollment_find_correspondences (
  const GoodixChicagoFeatureRecord *old_records,
  guint                             old_count,
  const GoodixChicagoFeatureRecord *new_records,
  guint                             new_count,
  GoodixChicagoCorrespondence       pairs[GOODIX_CHICAGO_CORRESPONDENCE_LIMIT])
{
  CorrespondenceCandidate selected[GOODIX_CHICAGO_CORRESPONDENCE_LIMIT];
  guint selected_count = 0;

  g_return_val_if_fail (old_count == 0 || old_records != NULL, 0);
  g_return_val_if_fail (new_count == 0 || new_records != NULL, 0);
  g_return_val_if_fail (pairs != NULL, 0);

  for (guint old_index = 0; old_index < old_count; old_index++)
    {
      guint best_distance = 192;
      guint second_distance = 192;
      gint best_index = -1;
      guint scan;
      gboolean replaced = FALSE;

      for (guint new_index = 0; new_index < new_count; new_index++)
        {
          const guint distance = descriptor_distance (&old_records[old_index],
                                                      &new_records[new_index]);

          if (distance < best_distance)
            {
              second_distance = best_distance;
              best_distance = distance;
              best_index = new_index;
            }
          else if (distance < second_distance)
            {
              second_distance = distance;
            }
        }

      if (best_index < 0 || best_distance * 40u >= second_distance * 38u)
        continue;

      for (scan = 0; scan < selected_count; scan++)
        {
          const GoodixChicagoFeatureRecord *existing =
            &new_records[selected[scan].new_index];
          const gint dx = (gint) (guint16) new_records[best_index].refined_x -
                          (gint) (guint16) existing->refined_x;
          const gint dy = (gint) (guint16) new_records[best_index].refined_y -
                          (gint) (guint16) existing->refined_y;

          if (dx * dx + dy * dy >= 0x10000)
            continue;
          if (selected[scan].best_distance <= best_distance)
            {
              if (!replaced)
                goto next_old_record;
              continue;
            }

          if (!replaced)
            {
              selected[scan] = (CorrespondenceCandidate){
                old_index, (guint) best_index, best_distance, second_distance,
              };
              replaced = TRUE;
            }
          else
            {
              if (scan != selected_count - 1)
                selected[scan] = selected[selected_count - 1];
              selected_count--;
            }
        }

      if (replaced)
        continue;
      if (selected_count < GOODIX_CHICAGO_CORRESPONDENCE_LIMIT)
        {
          selected[selected_count++] = (CorrespondenceCandidate){
            old_index, (guint) best_index, best_distance, second_distance,
          };
        }
      else
        {
          guint worst_index = 0;

          for (guint index = 1; index < selected_count; index++)
            if (selected[index].best_distance >
                selected[worst_index].best_distance)
              worst_index = index;
          if (best_distance < selected[worst_index].best_distance)
            selected[worst_index] = (CorrespondenceCandidate){
              old_index, (guint) best_index, best_distance, second_distance,
            };
        }

next_old_record:
      ;
    }

  for (guint index = 0; index < selected_count; index++)
    {
      pairs[index].old_index = selected[index].old_index;
      pairs[index].new_index = selected[index].new_index;
    }
  return selected_count;
}

static void
affine_from_three_points (const GoodixChicagoPoint source[3],
                          const GoodixChicagoPoint target[3],
                          gint32                   transform[6])
{
  const gint64 dx21 = (gint64) source[1].x - source[0].x;
  const gint64 dy21 = (gint64) source[1].y - source[0].y;
  const gint64 dx31 = (gint64) source[2].x - source[0].x;
  const gint64 dy31 = (gint64) source[2].y - source[0].y;
  const gint64 y_denominator = dx21 * dy31 - dx31 * dy21;
  const gint64 tx1 = (gint64) target[0].x << 10;
  const gint64 tx2 = (gint64) target[1].x << 10;
  const gint64 tx3 = (gint64) target[2].x << 10;
  const gint64 ty1 = (gint64) target[0].y << 10;
  const gint64 ty2 = (gint64) target[1].y << 10;
  const gint64 ty3 = (gint64) target[2].y << 10;
  gint32 a_q10;
  gint32 b_q10;
  gint32 c_q10;
  gint32 d_q10;

  if (y_denominator == 0)
    {
      c_q10 = G_MAXINT32;
      d_q10 = G_MAXINT32;
    }
  else
    {
      c_q10 = (gint32) (((ty2 - ty1) * dy31 -
                         (ty3 - ty1) * dy21) / y_denominator);
      d_q10 = (gint32) (((ty2 - ty1) * dx31 -
                         (ty3 - ty1) * dx21) / -y_denominator);
    }

  {
    const gint64 x12 = (gint64) source[0].x - source[1].x;
    const gint64 y12 = (gint64) source[0].y - source[1].y;
    const gint64 x23 = (gint64) source[1].x - source[2].x;
    const gint64 y23 = (gint64) source[1].y - source[2].y;
    const gint64 x_denominator = x12 * y23 - x23 * y12;

    if (x_denominator == 0)
      {
        a_q10 = G_MAXINT32;
        b_q10 = G_MAXINT32;
      }
    else
      {
        a_q10 = (gint32) (((tx1 - tx2) * y23 -
                           (tx2 - tx3) * y12) / x_denominator);
        b_q10 = (gint32) (((tx1 - tx2) * x23 -
                           (tx2 - tx3) * x12) / -x_denominator);
      }
  }

  transform[0] = a_q10 >> 2;
  transform[1] = b_q10 >> 2;
  transform[2] = (gint32) ((tx1 - (gint64) b_q10 * source[0].y -
                            (gint64) a_q10 * source[0].x) >> 10);
  transform[3] = c_q10 >> 2;
  transform[4] = d_q10 >> 2;
  transform[5] = (gint32) ((ty3 - (gint64) d_q10 * source[2].y -
                            (gint64) c_q10 * source[2].x) >> 10);
}

static gboolean
transform_scale_is_valid (const gint32 transform[6])
{
  const gint64 a = transform[0];
  const gint64 b = transform[1];
  const gint64 c = transform[3];
  const gint64 d = transform[4];
  const gint64 m00 = a * a + b * b;
  const gint64 m01 = a * c + b * d;
  const gint64 m10 = a * c + b * d;
  const gint64 m11 = c * c + d * d;
  const gint64 trace = m00 + m11;
  /* 64-bit wraparound (unsigned: defined in C) */
  const gint64 discriminant = (gint64) ((guint64) trace * (guint64) trace +
                                        4u * ((guint64) m10 * (guint64) m01 -
                                              (guint64) m11 * (guint64) m00));
  const gint64 upper_delta = ((gint64) 0xa3 << 9) - trace;
  const gint64 lower_delta = ((gint64) 0x191 << 9) - trace;

  return discriminant >= 0 && trace >= 0 && upper_delta <= 0 &&
         lower_delta >= 0 &&
         upper_delta * upper_delta > discriminant &&
         lower_delta * lower_delta > discriminant;
}

void
goodix_chicago_enrollment_estimate_transform (
  const GoodixChicagoPoint     *source_points,
  const GoodixChicagoPoint     *target_points,
  guint                         point_count,
  GoodixChicagoTransformResult *result)
{
  guint best_inliers = 0;

  g_return_if_fail (point_count == 0 || source_points != NULL);
  g_return_if_fail (point_count == 0 || target_points != NULL);
  g_return_if_fail (point_count <= GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT);
  g_return_if_fail (result != NULL);
  memset (result, 0, sizeof (*result));
  result->error = 0x190000;

  for (guint first = 0; first + 2 < point_count; first++)
    for (guint second = first + 1; second + 1 < point_count; second++)
      for (guint third = second + 1; third < point_count; third++)
        {
          const GoodixChicagoPoint source[3] = {
            source_points[first], source_points[second], source_points[third],
          };
          const GoodixChicagoPoint target[3] = {
            target_points[first], target_points[second], target_points[third],
          };
          gint32 transform[6];
          guint8 inliers[GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT] = { 0, };
          guint inlier_count = 0;
          gint32 residual_sum = 0;
          gint32 average_error;

          affine_from_three_points (source, target, transform);
          for (guint index = 0; index < point_count; index++)
            {
              const gint64 predicted_x =
                (((gint64) transform[0] * source_points[index].x +
                  (gint64) transform[1] * source_points[index].y) >> 8) +
                transform[2];
              const gint64 predicted_y =
                (((gint64) transform[3] * source_points[index].x +
                  (gint64) transform[4] * source_points[index].y) >> 8) +
                transform[5];
              const gint64 dx = predicted_x - target_points[index].x;
              const gint64 dy = predicted_y - target_points[index].y;
              gint64 squared;

              /* far-off predictions (degenerate transforms) would overflow
               * the square; they are no inliers anyway */
              if (ABS (dx) > 0x280 || ABS (dy) > 0x280)
                continue;
              squared = dx * dx + dy * dy;
              if (squared < 0x64000)
                {
                  inliers[index] = 1;
                  inlier_count++;
                  residual_sum += (gint32) squared;
                }
            }
          average_error = inlier_count == 0 ? 0x190000 :
                          ((gint32) (inlier_count >> 1) + residual_sum) /
                          (gint32) inlier_count;

          if ((inlier_count > best_inliers ||
               (inlier_count == best_inliers &&
                average_error < result->error)) &&
              transform_scale_is_valid (transform))
            {
              memcpy (result->values, transform, sizeof (transform));
              memcpy (result->inliers, inliers, sizeof (inliers));
              result->error = average_error;
              result->inlier_count = inlier_count;
              best_inliers = inlier_count;
            }
          if (best_inliers > 20)
            return;
        }
}

gboolean
goodix_chicago_enrollment_evidence_is_accepted (guint inlier_count,
                                                gint  metric_a,
                                                gint  metric_b)
{
  return inlier_count >= 11 ||
         (inlier_count >= 6 && metric_a >= 216) ||
         (inlier_count >= 7 && metric_a >= 209 && metric_b >= 65);
}

static void
unpack_metric_bits (const guint8 packed[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
                    guint8       unpacked[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                                          GOODIX_CHICAGO_METRIC_MAP_HEIGHT])
{
  for (guint index = 0;
       index < GOODIX_CHICAGO_METRIC_MAP_WIDTH *
       GOODIX_CHICAGO_METRIC_MAP_HEIGHT;
       index++)
    unpacked[index] = (packed[index >> 3] >> (index & 7)) & 1;
}

static gint32
transform_metric_coordinate (gint32       x,
                             gint32       y,
                             const gint32 transform[6],
                             guint        axis)
{
  const guint offset = axis == 0 ? 0 : 3;

  return (transform[offset] * x + transform[offset + 1] * y +
          transform[offset + 2] + 0x80) >> 8;
}

static void
expand_config1_mask (
  const guint8 packed[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES],
  guint8       expanded[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                        GOODIX_CHICAGO_METRIC_MAP_HEIGHT])
{
  const guint output_width = GOODIX_CHICAGO_METRIC_MAP_WIDTH;
  const guint output_height = GOODIX_CHICAGO_METRIC_MAP_HEIGHT;
  const guint coarse_width = output_width / 2;

  for (guint y = 0; y < output_height; y++)
    for (guint x = 0; x < output_width; x++)
      {
        const guint coarse_index = (y >> 1) * coarse_width + (x >> 1);

        expanded[y * output_width + x] =
          (packed[coarse_index >> 3] >> (coarse_index & 7)) & 1;
      }
}

static gint32
config1_half_translation (gint32 value)
{
  value++;
  value -= value >> 31;
  return value >> 1;
}

static gboolean
warp_config1_plane (const guint8 *source,
                    guint         source_width,
                    guint         source_height,
                    const gint32  transform[6],
                    guint         output_width,
                    guint         output_height,
                    guint8       *output)
{
  /* 32-bit wraparound (unsigned: defined in C) for degenerate transforms */
  const gint32 determinant =
    (gint32) ((guint32) transform[0] * (guint32) transform[4] -
              (guint32) transform[1] * (guint32) transform[3]);
  gint32 inverse_a;
  gint32 inverse_b;
  gint32 inverse_c;
  gint32 inverse_d;
  gint32 inverse_x;
  gint32 inverse_y;
  gint32 row_x = 0;
  gint32 row_y = 0;

  if (determinant == 0)
    return FALSE;
  inverse_a = (gint32) (((gint64) transform[4] * 0x40000) / determinant);
  inverse_c = (gint32) (((gint64) transform[3] * -0x40000) /
                        determinant);
  inverse_b = (gint32) (((gint64) transform[1] * -0x40000) /
                        determinant);
  inverse_d = (gint32) (((gint64) transform[0] * 0x40000) / determinant);
  inverse_x = (gint32) ((((gint64) transform[5] * transform[1] -
                          (gint64) transform[4] * transform[2]) * 0x400) /
                        determinant);
  inverse_y = (gint32) ((((gint64) transform[3] * transform[2] -
                          (gint64) transform[5] * transform[0]) * 0x400) /
                        determinant);

  for (guint y = 0; y < output_height; y++)
    {
      gint32 fixed_x = row_x;
      gint32 fixed_y = row_y;

      for (guint x = 0; x < output_width; x++)
        {
          const gint32 source_x = (fixed_x + inverse_x) >> 10;
          const gint32 source_y = (fixed_y + inverse_y) >> 10;
          guint8 value = 0xff;

          if (source_x + 1 >= 0 && source_x < (gint32) source_width &&
              source_y + 1 >= 0 && source_y < (gint32) source_height)
            {
              const gboolean left_out = source_x < 0;
              const gboolean top_out = source_y < 0;
              const gboolean right_out = source_x + 1 >=
                                         (gint32) source_width;
              const gboolean bottom_out = source_y + 1 >=
                                          (gint32) source_height;
              const gssize base = (gssize) source_y * source_width + source_x;

              if (!left_out && !top_out && !right_out && !bottom_out)
                {
                  const gint32 fraction_x =
                    fixed_x + inverse_x - source_x * 0x400;
                  const gint32 fraction_y =
                    fixed_y + inverse_y - source_y * 0x400;
                  const guint top = source[base] * (0x400 - fraction_x) +
                                    source[base + 1] * fraction_x;
                  const guint bottom = source[base + source_width] *
                                       (0x400 - fraction_x) +
                                       source[base + source_width + 1] *
                                       fraction_x;

                  value = (bottom * fraction_y +
                           top * (0x400 - fraction_y) + 0x80000) >> 20;
                }
              else
                {
                  guint sum = 0;
                  guint samples = 0;

                  if (!left_out && !top_out)
                    sum += source[base], samples++;
                  if (!right_out && !top_out)
                    sum += source[base + 1], samples++;
                  if (!left_out && !bottom_out)
                    sum += source[base + source_width], samples++;
                  if (!right_out && !bottom_out)
                    sum += source[base + source_width + 1], samples++;
                  if (samples)
                    value = sum / samples;
                }
            }
          output[y * output_width + x] = value;
          fixed_x += inverse_a;
          fixed_y += inverse_c;
        }
      row_x += inverse_b;
      row_y += inverse_d;
    }
  return TRUE;
}

static gboolean
calculate_config1_counts (
  const guint8 new_packed[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
  const guint8 old_packed[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
  const guint8 new_mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES],
  const guint8 old_mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES],
  const gint32 transform[6],
  guint        counts[4],
  guint       *total)
{
  guint8 new_plane[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                   GOODIX_CHICAGO_METRIC_MAP_HEIGHT];
  guint8 old_plane[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                   GOODIX_CHICAGO_METRIC_MAP_HEIGHT];
  guint8 new_expanded_mask[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                           GOODIX_CHICAGO_METRIC_MAP_HEIGHT];
  guint8 old_expanded_mask[GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                           GOODIX_CHICAGO_METRIC_MAP_HEIGHT];
  gint32 adjusted[6];
  gint32 corner_x[4];
  gint32 corner_y[4];
  gint32 minimum_x;
  gint32 minimum_y;
  gint32 maximum_x;
  gint32 maximum_y;
  guint output_width;
  guint output_height;
  g_autofree guint8 *warped_plane = NULL;
  g_autofree guint8 *warped_mask = NULL;

  memset (counts, 0, 4 * sizeof (*counts));
  *total = 0;
  unpack_metric_bits (new_packed, new_plane);
  unpack_metric_bits (old_packed, old_plane);
  expand_config1_mask (new_mask, new_expanded_mask);
  expand_config1_mask (old_mask, old_expanded_mask);
  memcpy (adjusted, transform, sizeof (adjusted));
  adjusted[2] = config1_half_translation (adjusted[2]);
  adjusted[5] = config1_half_translation (adjusted[5]);

  corner_x[0] = transform_metric_coordinate (0, 0, adjusted, 0);
  corner_y[0] = transform_metric_coordinate (0, 0, adjusted, 1);
  corner_x[1] = transform_metric_coordinate (
    GOODIX_CHICAGO_METRIC_MAP_WIDTH - 1, 0, adjusted, 0);
  corner_y[1] = transform_metric_coordinate (
    GOODIX_CHICAGO_METRIC_MAP_WIDTH - 1, 0, adjusted, 1);
  corner_x[2] = transform_metric_coordinate (
    GOODIX_CHICAGO_METRIC_MAP_WIDTH - 1,
    GOODIX_CHICAGO_METRIC_MAP_HEIGHT - 1, adjusted, 0);
  corner_y[2] = transform_metric_coordinate (
    GOODIX_CHICAGO_METRIC_MAP_WIDTH - 1,
    GOODIX_CHICAGO_METRIC_MAP_HEIGHT - 1, adjusted, 1);
  corner_x[3] = transform_metric_coordinate (
    0, GOODIX_CHICAGO_METRIC_MAP_HEIGHT - 1, adjusted, 0);
  corner_y[3] = transform_metric_coordinate (
    0, GOODIX_CHICAGO_METRIC_MAP_HEIGHT - 1, adjusted, 1);
  minimum_x = maximum_x = corner_x[0];
  minimum_y = maximum_y = corner_y[0];
  for (guint index = 1; index < 4; index++)
    {
      minimum_x = MIN (minimum_x, corner_x[index]);
      minimum_y = MIN (minimum_y, corner_y[index]);
      maximum_x = MAX (maximum_x, corner_x[index]);
      maximum_y = MAX (maximum_y, corner_y[index]);
    }
  minimum_x = MAX (minimum_x, 0);
  minimum_y = MAX (minimum_y, 0);
  maximum_x = MIN (maximum_x,
                   (gint32) GOODIX_CHICAGO_METRIC_MAP_WIDTH - 1);
  maximum_y = MIN (maximum_y,
                   (gint32) GOODIX_CHICAGO_METRIC_MAP_HEIGHT - 1);
  if (maximum_x < minimum_x || maximum_y < minimum_y)
    return FALSE;

  output_width = maximum_x - minimum_x + 1;
  output_height = maximum_y - minimum_y + 1;
  adjusted[2] -= minimum_x * 0x100;
  adjusted[5] -= minimum_y * 0x100;
  warped_plane = g_malloc (output_width * output_height);
  warped_mask = g_malloc (output_width * output_height);
  if (!warp_config1_plane (new_plane, GOODIX_CHICAGO_METRIC_MAP_WIDTH,
                           GOODIX_CHICAGO_METRIC_MAP_HEIGHT, adjusted,
                           output_width, output_height, warped_plane) ||
      !warp_config1_plane (new_expanded_mask,
                           GOODIX_CHICAGO_METRIC_MAP_WIDTH,
                           GOODIX_CHICAGO_METRIC_MAP_HEIGHT, adjusted,
                           output_width, output_height, warped_mask))
    return FALSE;

  for (guint y = 0; y < output_height && minimum_y + (gint) y <
       GOODIX_CHICAGO_METRIC_MAP_HEIGHT; y++)
    for (guint x = 0; x < output_width && minimum_x + (gint) x <
         GOODIX_CHICAGO_METRIC_MAP_WIDTH; x++)
      {
        const guint old_index =
          (minimum_y + y) * GOODIX_CHICAGO_METRIC_MAP_WIDTH +
          minimum_x + x;
        const guint warped_index = y * output_width + x;
        const guint8 old_value = old_plane[old_index];
        const guint8 new_value = warped_plane[warped_index];

        if (old_value < 2 && new_value < 2 &&
            old_expanded_mask[old_index] != 0 &&
            warped_mask[warped_index] != 0)
          {
            counts[old_value + new_value * 2]++;
            (*total)++;
          }
      }
  return TRUE;
}

/* The config-1 selector score takes two more inputs from the metric
 * context: the inlier count of the transform gates the "activity > 17" branch — (occupancy > 40 && inliers >= 8) || inliers >= 9 —
 * and the occupancy is computed only while still -1, i.e. by the first
 * map pair, and reused for the second.  inliers = 8 and a fresh occupancy give the
 * previous behaviour. */
static gint
config1_selector_score_ex (const guint counts[4],
                           guint       total,
                           gint        inliers,
                           gint       *occupancy_io)
{
  const guint pixels = GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                       GOODIX_CHICAGO_METRIC_MAP_HEIGHT;
  gint occupancy;
  gint score;
  gint activity;

  if (*occupancy_io < 0)
    *occupancy_io = (counts[3] * 0x100) / (counts[1] + counts[2] + counts[3] + 1);
  occupancy = *occupancy_io;

  if (total == 0)
    return 128;
  if (total > pixels / 2)
    score = (counts[0] * 0x100 + total / 2) / total + 3 + 38;
  else
    score = (counts[0] * 0x100 + total / 2) / (total + 1) +
            (total * 19) / (pixels / 2) + 2 + 19;
  activity = (counts[3] * 0x100 + total / 2) / total + 3;
  if (activity > 23 ||
      (activity > 18 && occupancy > 62) ||
      (activity > 19 && occupancy > 50) ||
      (activity > 17 && ((occupancy > 40 && inliers >= 8) || inliers >= 9)) ||
      (score > 230 && activity > 16))
    return score;
  return 128;
}

void
goodix_chicago_enrollment_calculate_live_auxiliary_counts_type24 (
  const guint8 auxiliary[6],
  const gint32 transform[6],
  gint        *count_zero,
  gint        *count_mixed,
  gint        *count_one)
{
  guint8 packed[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES] = { 0, };
  guint8 mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES];
  guint counts[4];
  guint total;

  g_return_if_fail (auxiliary != NULL);
  g_return_if_fail (transform != NULL);
  g_return_if_fail (count_zero != NULL);
  g_return_if_fail (count_mixed != NULL);
  g_return_if_fail (count_one != NULL);
  *count_zero = 0;
  *count_mixed = 0;
  *count_one = 0;

  /* Every nonzero byte is canonicalized to one first.  Type 24 never
   * takes the type-7/type-23 map-copy block, so all
   * pixels following this six-byte prefix remain zero. */
  for (guint index = 0; index < 6; index++)
    if (auxiliary[index] != 0)
      packed[index >> 3] |= (guint8) (1u << (index & 7));
  memset (mask, 0xff, sizeof (mask));
  if (!calculate_config1_counts (packed, packed, mask, mask, transform,
                                 counts, &total))
    return;
  *count_zero = counts[0];
  *count_mixed = counts[1] + counts[2];
  *count_one = counts[3];
}

gint goodix_chicago_enrollment_calculate_config1_metrics_ex (const GoodixChicagoMetricData *new_metric_data,
                                                             const GoodixChicagoMetricData *old_metric_data,
                                                             const gint32                   transform[6],
                                                             gint                           inliers,
                                                             gint                          *occupancy_io,
                                                             gint                          *metric_a,
                                                             gint                          *metric_b);

gint
goodix_chicago_enrollment_calculate_config1_metrics (
  const GoodixChicagoMetricData *new_metric_data,
  const GoodixChicagoMetricData *old_metric_data,
  const gint32                   transform[6],
  gint                          *metric_a,
  gint                          *metric_b)
{
  gint occupancy = -1;

  return goodix_chicago_enrollment_calculate_config1_metrics_ex (
    new_metric_data, old_metric_data, transform, 8, &occupancy, metric_a, metric_b);
}

/* One map pair. */
gint
goodix_chicago_enrollment_calculate_config1_metrics_ex (
  const GoodixChicagoMetricData *new_metric_data,
  const GoodixChicagoMetricData *old_metric_data,
  const gint32                   transform[6],
  gint                           inliers,
  gint                          *occupancy_io,
  gint                          *metric_a,
  gint                          *metric_b)
{
  guint counts[4];
  guint total;
  const guint pixels = GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                       GOODIX_CHICAGO_METRIC_MAP_HEIGHT;

  gint selector_score;

  g_return_val_if_fail (new_metric_data != NULL, 0);
  g_return_val_if_fail (old_metric_data != NULL, 0);
  g_return_val_if_fail (transform != NULL, 0);
  g_return_val_if_fail (metric_a != NULL, 0);
  g_return_val_if_fail (metric_b != NULL, 0);
  *metric_a = 0;
  *metric_b = 0;

  if (!calculate_config1_counts (
        new_metric_data->primary, old_metric_data->primary,
        new_metric_data->coarse_mask, old_metric_data->coarse_mask,
        transform, counts, &total))
    return 0;
  selector_score = config1_selector_score_ex (counts, total, inliers, occupancy_io);
  *metric_b = (total * 0x100 + pixels / 2) / pixels;
  {
    const guint without_both_one = counts[0] + counts[1] + counts[2];
    const guint both_one = (counts[3] * 0x100 + total / 2) / (total + 1);

    *metric_a = (counts[0] * 0x100 + without_both_one / 2) /
                (without_both_one + 1);
    if (both_one < 15)
      *metric_a -= ((15 - both_one) >> 1) + 3;
  }
  return selector_score;
}

/* Relation metrics: the matcher's selector/agreement metric (not a separate
 * relation metric) for every pair with > 4 inliers: metric_a is the agreement,
 * taken from the second map pair when it is higher and the first selector is
 * > 220. */
static void
group_relation_metrics (const GoodixChicagoMetricData *new_metric_data,
                        const GoodixChicagoMetricData *old_metric_data,
                        const gint32                   transform[6],
                        gint                          *agreement,
                        gint                          *coverage)
{
  GoodixChicagoMetricData p2 = *new_metric_data;
  GoodixChicagoMetricData g2 = *old_metric_data;
  gint a = 0, c = 0, a2 = 0, c2 = 0, selector, occupancy = -1;

  /* Inlier count argument 0 (gates the selector) */
  selector = goodix_chicago_enrollment_calculate_config1_metrics_ex (
    new_metric_data, old_metric_data, transform, 0, &occupancy, &a, &c);
  memcpy (p2.primary, new_metric_data->secondary, sizeof (p2.primary));
  memcpy (g2.primary, old_metric_data->secondary, sizeof (g2.primary));
  goodix_chicago_enrollment_calculate_config1_metrics_ex (&p2, &g2, transform, 0, &occupancy,
                                                          &a2, &c2);
  if (a2 > a && selector > 220)
    a = a2;
  *agreement = a;
  *coverage = c;
}

/* Return value: the second map pair's
 * selector when it is higher and its agreement is > 195. */
static gint
group_selector (const GoodixChicagoMetricData *new_metric_data,
                const GoodixChicagoMetricData *old_metric_data,
                const gint32                   transform[6])
{
  GoodixChicagoMetricData p2 = *new_metric_data;
  GoodixChicagoMetricData g2 = *old_metric_data;
  gint a = 0, c = 0, a2 = 0, c2 = 0, selector, s2, occupancy = -1;

  /* Inlier count argument 0, as above */
  selector = goodix_chicago_enrollment_calculate_config1_metrics_ex (
    new_metric_data, old_metric_data, transform, 0, &occupancy, &a, &c);
  memcpy (p2.primary, new_metric_data->secondary, sizeof (p2.primary));
  memcpy (g2.primary, old_metric_data->secondary, sizeof (g2.primary));
  s2 = goodix_chicago_enrollment_calculate_config1_metrics_ex (&p2, &g2, transform, 0,
                                                               &occupancy, &a2, &c2);
  if (s2 > selector && a2 > 195)
    selector = s2;
  return selector;
}

gboolean
goodix_chicago_enrollment_calculate_study_metrics (
  const GoodixChicagoMetricData *new_metric_data,
  const GoodixChicagoMetricData *old_metric_data,
  const gint32                   transform[6],
  gint                          *metric_18,
  gint                          *metric_1c,
  gint                          *metric_20)
{
  guint counts[4];
  guint total;
  guint denominator;

  g_return_val_if_fail (new_metric_data != NULL, FALSE);
  g_return_val_if_fail (old_metric_data != NULL, FALSE);
  g_return_val_if_fail (transform != NULL, FALSE);
  g_return_val_if_fail (metric_18 != NULL, FALSE);
  g_return_val_if_fail (metric_1c != NULL, FALSE);
  g_return_val_if_fail (metric_20 != NULL, FALSE);
  *metric_18 = 0;
  *metric_1c = 0;
  *metric_20 = 0;

  if (!calculate_config1_counts (
        new_metric_data->validity, old_metric_data->validity,
        new_metric_data->coarse_mask, old_metric_data->coarse_mask,
        transform, counts, &total))
    return FALSE;

  if (total > 0)
    *metric_18 = ((counts[0] + counts[3]) * 0x100 + total / 2) / total;
  denominator = counts[0] + counts[1] + counts[2];
  if (denominator > 0)
    *metric_1c = (counts[0] * 0x100 + denominator / 2) / denominator;
  denominator = counts[1] + counts[2] + counts[3];
  if (denominator > 0)
    *metric_20 = (counts[3] * 0x100 + denominator / 2) / denominator;
  return TRUE;
}

static void
metric_data_build_position_map (GoodixChicagoMetricData *metric_data)
{
  memset (metric_data->position_map, 0, sizeof (metric_data->position_map));
  /* Mode 1 reinterprets the 320 coarse bits as 20 x 16,
   * duplicates each bit to a 2 x 2 block, and repacks the resulting
   * 40 x 32 plane LSB-first. */
  for (guint y = 0; y < 32; y++)
    for (guint x = 0; x < 40; x++)
      {
        const guint coarse_bit = (y >> 1) * 20 + (x >> 1);
        const guint output_bit = y * 40 + x;

        if ((metric_data->coarse_mask[coarse_bit >> 3] >>
             (coarse_bit & 7)) & 1u)
          metric_data->position_map[output_bit >> 3] |=
            1u << (output_bit & 7);
      }
}

void
goodix_chicago_enrollment_build_metric_data (
  const guint8             enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
  GoodixChicagoMetricData *metric_data)
{
  guint8 source[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint8 prepared[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint8 mask[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint histogram[256] = { 0, };
  guint quantile_samples = 0;
  guint target;
  guint cumulative = 0;
  guint threshold = 0;

  g_return_if_fail (enhanced != NULL);
  g_return_if_fail (metric_data != NULL);
  memset (metric_data, 0, sizeof (*metric_data));
  goodix_chicago_feature_build_source (enhanced, source);
  goodix_chicago_feature_build_prepared_image (enhanced, prepared);
  goodix_chicago_feature_build_mask (enhanced, mask);

  for (guint y = 0; y < GOODIX_CHICAGO_METRIC_MAP_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_METRIC_MAP_WIDTH; x++)
      {
        const guint index = (y * 2) * GOODIX_CHICAGO_FEATURE_WIDTH + x * 2;

        if (mask[index] != 0)
          {
            histogram[prepared[index]]++;
            quantile_samples++;
          }
      }
  target = (quantile_samples * 205u + 128u) >> 8;
  cumulative = histogram[0];
  for (guint value = 1; value < 256; value++)
    {
      const guint previous = cumulative;

      cumulative += histogram[value];
      if (cumulative >= target)
        {
          threshold = target - previous < cumulative - target ?
                      value - 1 : value;
          break;
        }
    }

  /* Mode 1 samples the 80 x 64 runtime image at even pixels,
   * producing the packed 40 x 32 metric plane. */
  for (guint y = 0; y < GOODIX_CHICAGO_METRIC_MAP_HEIGHT; y++)
    for (guint x = 0; x < GOODIX_CHICAGO_METRIC_MAP_WIDTH; x++)
      {
        const guint source_index =
          (y * 2) * GOODIX_CHICAGO_FEATURE_WIDTH + x * 2;
        const guint bit = y * GOODIX_CHICAGO_METRIC_MAP_WIDTH + x;

        if (source[source_index] > 200)
          metric_data->primary[bit >> 3] |= 1u << (bit & 7);
        if (prepared[source_index] > threshold)
          metric_data->secondary[bit >> 3] |= 1u << (bit & 7);
        if (source[source_index] > 55)
          metric_data->validity[bit >> 3] |= 1u << (bit & 7);
      }

  for (guint block_y = 0;
       block_y < GOODIX_CHICAGO_FEATURE_HEIGHT / 4; block_y++)
    for (guint block_x = 0;
         block_x < GOODIX_CHICAGO_FEATURE_WIDTH / 4; block_x++)
      {
        guint populated = 0;
        const guint bit =
          block_y * (GOODIX_CHICAGO_FEATURE_WIDTH / 4) + block_x;

        for (guint y = 0; y < 4; y++)
          for (guint x = 0; x < 4; x++)
            populated += mask[(block_y * 4 + y) *
                              GOODIX_CHICAGO_FEATURE_WIDTH +
                              block_x * 4 + x];
        if (populated >= 8)
          metric_data->coarse_mask[bit >> 3] |= 1u << (bit & 7);
      }

  metric_data_build_position_map (metric_data);
}

void
goodix_chicago_enrollment_build_relation (
  const GoodixChicagoFeatureRecord *old_records,
  guint                             old_count,
  const GoodixChicagoMetricData    *old_metric_data,
  const GoodixChicagoFeatureRecord *new_records,
  guint                             new_count,
  const GoodixChicagoMetricData    *new_metric_data,
  GoodixChicagoRelation            *relation,
  gint                             *metric_a,
  gint                             *metric_b,
  gboolean                         *evidence_accepted)
{
  GoodixChicagoCorrespondence pairs[GOODIX_CHICAGO_CORRESPONDENCE_LIMIT];
  GoodixChicagoPoint source[GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT];
  GoodixChicagoPoint target[GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT];
  GoodixChicagoTransformResult transform;
  guint pair_count;
  gint native_metric_a = 0;
  gint native_metric_b = 0;

  g_return_if_fail (old_count == 0 || old_records != NULL);
  g_return_if_fail (new_count == 0 || new_records != NULL);
  g_return_if_fail (old_metric_data != NULL);
  g_return_if_fail (new_metric_data != NULL);
  g_return_if_fail (relation != NULL);
  memset (relation, 0, sizeof (*relation));

  pair_count = goodix_chicago_enrollment_find_correspondences (
    old_records, old_count, new_records, new_count, pairs);
  for (guint index = 0; index < pair_count; index++)
    {
      source[index].x =
        (guint16) new_records[pairs[index].new_index].refined_x;
      source[index].y =
        (guint16) new_records[pairs[index].new_index].refined_y;
      target[index].x =
        (guint16) old_records[pairs[index].old_index].refined_x;
      target[index].y =
        (guint16) old_records[pairs[index].old_index].refined_y;
    }
  goodix_chicago_enrollment_estimate_transform (source, target, pair_count,
                                                &transform);
  relation->inlier_count = transform.inlier_count;
  memcpy (relation->transform, transform.values, sizeof (relation->transform));
  if (transform.inlier_count > 4)
    group_relation_metrics (new_metric_data, old_metric_data, relation->transform,
                            &native_metric_a, &native_metric_b);
  if (metric_a)
    *metric_a = native_metric_a;
  if (metric_b)
    *metric_b = native_metric_b;
  if (evidence_accepted)
    *evidence_accepted =
      goodix_chicago_enrollment_evidence_is_accepted (
        transform.inlier_count, native_metric_a, native_metric_b);
}

static void
subtemplate_free (gpointer data)
{
  GoodixChicagoSubtemplate *subtemplate = data;

  if (!subtemplate)
    return;
  g_free (subtemplate->records);
  g_free (subtemplate);
}

GoodixChicagoEnrollment *
goodix_chicago_enrollment_new (void)
{
  GoodixChicagoEnrollment *self =
    g_new0 (GoodixChicagoEnrollment, 1);

  self->required_samples = GOODIX_CHICAGO_ENROLLMENT_REQUIRED_SAMPLES;
  self->capacity = GOODIX_CHICAGO_ENROLLMENT_CAPACITY;
  self->record_limit = GOODIX_CHICAGO_SUBTEMPLATE_RECORD_LIMIT;
  self->subtemplates = g_ptr_array_new_with_free_func (subtemplate_free);
  self->relations = g_array_new (FALSE, FALSE,
                                 sizeof (GoodixChicagoRelation));
  for (guint index = 0; index < G_N_ELEMENTS (self->match_order); index++)
    self->match_order[index] = G_MAXUINT32;
  self->matcher_active_index = G_MAXUINT32;
  return self;
}

void
goodix_chicago_enrollment_free (GoodixChicagoEnrollment *self)
{
  if (!self)
    return;
  g_ptr_array_unref (self->subtemplates);
  g_array_unref (self->relations);
  g_free (self);
}

gboolean
goodix_chicago_enrollment_insert_first (
  GoodixChicagoEnrollment          *self,
  const GoodixChicagoFeatureRecord *records,
  guint                             record_count,
  guint                             active_count,
  guint                             quality,
  guint                             coverage,
  const GoodixChicagoMetricData    *metric_data,
  GoodixChicagoEnrollmentResult    *result,
  GError                          **error)
{
  GoodixChicagoSubtemplate *subtemplate;
  const GoodixChicagoRelation sentinel = {
    -1, { 0x100, 0, 0, 0, 0x100, 0 },
  };

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  memset (result, 0, sizeof (*result));

  if (self->subtemplates->len != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                           "gdix51c0: Chicago first subtemplate is already present");
      return FALSE;
    }
  if ((record_count != 0 && records == NULL) ||
      record_count > self->record_limit || active_count > record_count)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "gdix51c0: invalid Chicago subtemplate counts %u/%u",
                   active_count, record_count);
      return FALSE;
    }

  subtemplate = g_new0 (GoodixChicagoSubtemplate, 1);
  subtemplate->record_count = record_count;
  subtemplate->active_count = active_count;
  subtemplate->quality = quality;
  subtemplate->coverage = coverage;
  subtemplate->lineage_index = self->subtemplates->len;
  if (metric_data)
    {
      subtemplate->has_metric_data = TRUE;
      subtemplate->metric_data = *metric_data;
    }
  subtemplate->records = g_memdup2 (records,
                                    record_count * sizeof (*records));
  g_ptr_array_add (self->subtemplates, subtemplate);
  self->match_order[0] = 0;
  g_array_append_val (self->relations, sentinel);
  self->transform_count++;

  result->packed_position_detail = GOODIX_CHICAGO_FIRST_POSITION_DETAIL;
  result->position_x =
    100 - (result->packed_position_detail >> 24);
  result->position_y =
    100 - (result->packed_position_detail & 0xff);
  result->progress = MIN (100u,
                          self->subtemplates->len * 100u /
                          self->required_samples);
  return TRUE;
}

static guint
affine_rectangle_overlap (const gint32 transform[6],
                          guint        width,
                          guint        height,
                          gboolean     half_translation)
{
  const gint32 translation_x = half_translation ?
                               transform[2] >> 1 : transform[2];
  const gint32 translation_y = half_translation ?
                               transform[5] >> 1 : transform[5];
  const gint64 maximum_x = ((gint64) width - 1) << 8;
  const gint64 maximum_y = ((gint64) height - 1) << 8;
  guint overlap = 0;

  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        const gint64 mapped_x = (gint64) transform[0] * x +
                                (gint64) transform[1] * y + translation_x;
        const gint64 mapped_y = (gint64) transform[3] * x +
                                (gint64) transform[4] * y + translation_y;

        overlap += mapped_x >= 0 && mapped_x <= maximum_x &&
                   mapped_y >= 0 && mapped_y <= maximum_y;
      }
  return overlap;
}

static guint
enrollment_integer_square_root (guint32 value)
{
  guint32 result = 0;
  guint32 bit = 1u << 30;

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
normalize_group_transform (const gint32 input[6],
                           gint32       output[6])
{
  const gint32 cosine = (input[0] + input[4]) >> 1;
  const gint32 sine = (input[3] - input[1]) >> 1;
  const guint32 magnitude = enrollment_integer_square_root (
    cosine * cosine + sine * sine);

  memcpy (output, input, 6 * sizeof (*output));
  if (magnitude == 0)
    {
      output[0] = 0x100;
      output[1] = 0;
      output[3] = 0;
      output[4] = 0x100;
      return;
    }
  output[0] = ((gint64) cosine * 0x100 + magnitude / 2) / magnitude;
  output[1] = (magnitude / 2 - (gint64) sine * 0x100) / magnitude;
  output[3] = -output[1];
  output[4] = output[0];
}

static void
identity_group_transform (gint32 transform[6])
{
  memcpy (transform, (const gint32[6]){ 0x100, 0, 0, 0, 0x100, 0 },
          6 * sizeof (*transform));
}

static void
invert_group_transform (const gint32 input[6],
                        gint32       output[6])
{
  const gint32 determinant = input[0] * input[4] - input[1] * input[3];

  if (determinant == 0)
    {
      memcpy (output, input, 6 * sizeof (*output));
      return;
    }
  output[0] = (gint32) (((gint64) input[4] * 0x10000) / determinant);
  output[1] = (gint32) (((gint64) - input[1] * 0x10000) / determinant);
  output[2] = (gint32) (((((gint64) input[5] * input[1]) -
                          ((gint64) input[4] * input[2])) * 0x100) /
                        determinant);
  output[3] = (gint32) (((gint64) - input[3] * 0x10000) / determinant);
  output[4] = (gint32) (((gint64) input[0] * 0x10000) / determinant);
  output[5] = (gint32) (((((gint64) input[3] * input[2]) -
                          ((gint64) input[5] * input[0])) * 0x100) /
                        determinant);
}

static void
compose_group_transform (const gint32 first[6],
                         const gint32 second[6],
                         gint32       output[6])
{
  gint32 composed[6];

  composed[0] = (gint32) ((((gint64) first[0] * second[0]) +
                           ((gint64) first[1] * second[3])) >> 8);
  composed[1] = (gint32) ((((gint64) first[0] * second[1]) +
                           ((gint64) first[1] * second[4])) >> 8);
  composed[2] = (gint32) ((((gint64) first[0] * second[2]) +
                           ((gint64) first[1] * second[5])) >> 8) + first[2];
  composed[3] = (gint32) ((((gint64) first[3] * second[0]) +
                           ((gint64) first[4] * second[3])) >> 8);
  composed[4] = (gint32) ((((gint64) first[3] * second[1]) +
                           ((gint64) first[4] * second[4])) >> 8);
  composed[5] = (gint32) ((((gint64) first[3] * second[2]) +
                           ((gint64) first[4] * second[5])) >> 8) + first[5];
  normalize_group_transform (composed, output);
}

typedef struct
{
  GoodixChicagoPoint points[5];
  GoodixChicagoPoint edges[4];
} GoodixChicagoCapacityPolygon;

static void
capacity_transform_point (const GoodixChicagoPoint *point,
                          const gint32              transform[6],
                          GoodixChicagoPoint       *mapped)
{
  mapped->x = ((gint64) transform[0] * point->x +
               (gint64) transform[1] * point->y + transform[2] + 0x80) >> 8;
  mapped->y = ((gint64) transform[3] * point->x +
               (gint64) transform[4] * point->y + transform[5] + 0x80) >> 8;
}

/* Literal geometric boundary for the 40 x 32 study map.  The
 * return value only decides whether the transformed rectangle can intersect
 * the target; the five points are retained even when it cannot. */
static gboolean
capacity_build_polygon (const gint32                  transform[6],
                        gint32                        width,
                        gint32                        height,
                        GoodixChicagoCapacityPolygon *polygon)
{
  const GoodixChicagoPoint corners[4] = {
    { 0, 0 }, { width, 0 }, { width, height }, { 0, height },
  };
  gboolean inside_margin = FALSE;
  gboolean inside_expanded = FALSE;
  GoodixChicagoPoint point;

  for (guint index = 0; index < 4; index++)
    {
      capacity_transform_point (&corners[index], transform,
                                &polygon->points[index]);
      inside_margin |=
        polygon->points[index].x > 5 &&
        polygon->points[index].x < width - 5 &&
        polygon->points[index].y > 5 &&
        polygon->points[index].y < height - 5;
      inside_expanded |=
        polygon->points[index].x > -5 &&
        polygon->points[index].x < width + 5 &&
        polygon->points[index].y > -5 &&
        polygon->points[index].y < height + 5;
    }
  polygon->points[4] = polygon->points[0];
  for (guint index = 0; index < 4; index++)
    {
      polygon->edges[index].x =
        polygon->points[index + 1].x - polygon->points[index].x;
      polygon->edges[index].y =
        polygon->points[index + 1].y - polygon->points[index].y;
    }
  if (inside_margin)
    return TRUE;

  point = (GoodixChicagoPoint){ width >> 1, height >> 1 };
  capacity_transform_point (&point, transform, &point);
  if (point.x >= 0 && point.x < width && point.y >= 0 && point.y < height)
    return TRUE;
  if (inside_expanded)
    {
      for (gint32 x = 0; x < width; x += 16)
        {
          point = (GoodixChicagoPoint){ x, height >> 1 };
          capacity_transform_point (&point, transform, &point);
          if (point.x >= 0 && point.x < width &&
              point.y >= 0 && point.y < height)
            return TRUE;
        }
      for (gint32 y = 0; y < height; y += 16)
        {
          point = (GoodixChicagoPoint){ width >> 1, y };
          capacity_transform_point (&point, transform, &point);
          if (point.x >= 0 && point.x < width &&
              point.y >= 0 && point.y < height)
            return TRUE;
        }
    }
  return FALSE;
}

/* Sign test. */
static gboolean
capacity_polygon_contains (const GoodixChicagoCapacityPolygon *polygon,
                           gint32                              x,
                           gint32                              y)
{
  gint64 cross[4];

  for (guint index = 0; index < 4; index++)
    cross[index] =
      (gint64) (x - polygon->points[index].x) * polygon->edges[index].y -
      (gint64) (y - polygon->points[index].y) * polygon->edges[index].x;
  return ((cross[0] ^ cross[2]) >= 0) && ((cross[1] ^ cross[3]) >= 0);
}

/* Samples the expanded position map at 1,4,7,... in both axes.
 * Empty map cells and cells covered by at least one other template both count
 * toward the redundancy percentage. */
static gint32
capacity_redundancy_score (
  const GoodixChicagoMetricData      *metric_data,
  const GoodixChicagoCapacityPolygon *polygons,
  guint                               polygon_count)
{
  const gint32 width = GOODIX_CHICAGO_FEATURE_WIDTH;
  const gint32 height = GOODIX_CHICAGO_FEATURE_HEIGHT;
  gint32 covered = 0;
  gint32 total = 0;

  for (gint32 y = 1; y < height; y += 3)
    for (gint32 x = 1; x < width; x += 3)
      {
        const guint bit = (y >> 2) * (width >> 2) + (x >> 2);
        gboolean admitted =
          ((metric_data->coarse_mask[bit >> 3] >> (bit & 7)) & 1u) == 0;

        total++;
        for (guint index = 0; index < polygon_count && !admitted; index++)
          admitted = capacity_polygon_contains (&polygons[index], x, y);
        covered += admitted;
      }
  return total > 0 ? covered * 100 / total : 100;
}

static GoodixChicagoRelation *
relation_at (const GoodixChicagoEnrollment *self,
             guint                          relation_index)
{
  if (relation_index >= self->relations->len)
    return NULL;
  return &g_array_index (self->relations, GoodixChicagoRelation,
                         relation_index);
}

static const GoodixChicagoRelation *
relation_between_subtemplates (const GoodixChicagoEnrollment *self,
                               guint                          newer,
                               guint                          older)
{
  const GoodixChicagoSubtemplate *subtemplate;

  g_return_val_if_fail (newer > older, NULL);
  subtemplate = g_ptr_array_index (self->subtemplates, newer);
  return relation_at (self, subtemplate->relation_base + older);
}

/* Mode zero returns the transform from @second to @first. */
static gint32
capacity_relation_load (const GoodixChicagoEnrollment *self,
                        const GoodixChicagoRelation   *probe_relations,
                        guint                          first,
                        guint                          second,
                        gint32                         transform[6])
{
  const guint count = self->subtemplates->len;
  const GoodixChicagoRelation *relation;

  if (first == second)
    {
      identity_group_transform (transform);
      return 0;
    }
  if (first == count)
    {
      if (probe_relations == NULL)
        {
          identity_group_transform (transform);
          return -1;
        }
      relation = &probe_relations[second];
      invert_group_transform (relation->transform, transform);
      return relation->inlier_count;
    }
  if (second == count)
    {
      if (probe_relations == NULL)
        {
          identity_group_transform (transform);
          return -1;
        }
      relation = &probe_relations[first];
      memcpy (transform, relation->transform, sizeof (relation->transform));
      return relation->inlier_count;
    }
  if (first < second)
    {
      relation = relation_between_subtemplates (self, second, first);
      if (relation == NULL)
        {
          identity_group_transform (transform);
          return -1;
        }
      memcpy (transform, relation->transform, sizeof (relation->transform));
      return relation->inlier_count;
    }

  relation = relation_between_subtemplates (self, first, second);
  if (relation == NULL)
    {
      identity_group_transform (transform);
      return -1;
    }
  invert_group_transform (relation->transform, transform);
  return relation->inlier_count;
}

/* Mode one stores a transform from @second to @first. */
static void
capacity_relation_store (GoodixChicagoEnrollment *self,
                         GoodixChicagoRelation   *probe_relations,
                         guint                    first,
                         guint                    second,
                         const gint32             transform[6],
                         gint32                   strength)
{
  const guint count = self->subtemplates->len;
  GoodixChicagoRelation *relation;

  if (first == second)
    return;
  if (first == count)
    {
      if (probe_relations == NULL)
        return;
      relation = &probe_relations[second];
      relation->inlier_count = strength;
      invert_group_transform (transform, relation->transform);
      return;
    }
  if (second == count)
    {
      if (probe_relations == NULL)
        return;
      relation = &probe_relations[first];
      relation->inlier_count = strength;
      memcpy (relation->transform, transform, sizeof (relation->transform));
      return;
    }
  if (first < second)
    {
      relation = (GoodixChicagoRelation *)
                 relation_between_subtemplates (self, second, first);
      relation->inlier_count = strength;
      memcpy (relation->transform, transform, sizeof (relation->transform));
      return;
    }

  relation = (GoodixChicagoRelation *)
             relation_between_subtemplates (self, first, second);
  relation->inlier_count = strength;
  invert_group_transform (transform, relation->transform);
}

static gboolean
propagate_capacity_relations (
  GoodixChicagoEnrollment *self,
  GoodixChicagoRelation   *probe_relations,
  guint                    relation_count,
  gboolean                 visited[GOODIX_CHICAGO_ENROLLMENT_CAPACITY])
{
  const guint count = self != NULL ? self->subtemplates->len : 0;
  const guint probe_node = count;
  guint stack[GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1];
  guint stack_count = 1;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (probe_relations != NULL, FALSE);
  /* Mode 1 (study append) runs this below capacity as well. */
  if (count > self->capacity ||
      count > GOODIX_CHICAGO_ENROLLMENT_CAPACITY ||
      relation_count != count)
    return FALSE;

  memset (visited, 0,
          GOODIX_CHICAGO_ENROLLMENT_CAPACITY * sizeof (*visited));
  stack[0] = probe_node;
  while (stack_count > 0)
    {
      const guint source = stack[--stack_count];
      gint32 source_to_probe[6];

      capacity_relation_load (self, probe_relations, probe_node, source,
                              source_to_probe);
      for (gint candidate_signed = (gint) count - 1;
           candidate_signed >= 0; candidate_signed--)
        {
          const guint candidate = (guint) candidate_signed;
          gint32 candidate_to_source[6];
          gint32 candidate_to_probe[6];
          gint32 edge_strength;
          gint32 existing_strength;

          if (visited[candidate])
            continue;
          edge_strength = capacity_relation_load (
            self, probe_relations, source, candidate,
            candidate_to_source);
          /* Literal unsigned range test: only 1..42 forms a
           * traversable relation edge. */
          if ((guint32) (edge_strength - 1) > 41)
            continue;

          visited[candidate] = TRUE;
          stack[stack_count++] = candidate;
          existing_strength = capacity_relation_load (
            self, probe_relations, probe_node, candidate,
            candidate_to_probe);
          if (source == probe_node || existing_strength >= 2)
            continue;

          compose_group_transform (source_to_probe,
                                   candidate_to_source,
                                   candidate_to_probe);
          capacity_relation_store (self, probe_relations, probe_node,
                                   candidate, candidate_to_probe, 2);
        }
    }

  return TRUE;
}

static gboolean
refine_unresolved_capacity_relations (
  GoodixChicagoEnrollment             *self,
  const GoodixChicagoSubtemplateView  *probe,
  GoodixChicagoRelation               *probe_relations,
  const gboolean                       visited[GOODIX_CHICAGO_ENROLLMENT_CAPACITY],
  GoodixChicagoCapacityRelationBuilder build_unresolved_relation)
{
  for (guint index = 0; index < self->subtemplates->len; index++)
    {
      GoodixChicagoRelation *relation = &probe_relations[index];
      GoodixChicagoSubtemplateView gallery;

      if (visited[index] || relation->inlier_count == -2)
        continue;
      if (!build_unresolved_relation ||
          !goodix_chicago_enrollment_get_subtemplate (
            self, index, &gallery))
        return FALSE;
      build_unresolved_relation (&gallery, probe, relation);
    }
  return TRUE;
}

gboolean
goodix_chicago_enrollment_synthesize_capacity_relations (
  GoodixChicagoEnrollment *self,
  GoodixChicagoRelation   *probe_relations,
  guint                    relation_count)
{
  const guint count = self != NULL ? self->subtemplates->len : 0;
  const guint node_count = count + 1;
  gint32 strengths[GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1]
  [GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1] = { { 0, }, };

  g_return_val_if_fail (self != NULL, FALSE);
  /* The gallery-only closure (probe_relations NULL) also runs below capacity:
   * after every study append and after unpack. */
  if ((probe_relations != NULL && count != self->capacity) ||
      count > GOODIX_CHICAGO_ENROLLMENT_CAPACITY ||
      relation_count != count)
    return FALSE;

  for (guint root = 0; root < count; root++)
    {
      gint32 visited[GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1];
      guint stack[GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1];
      guint stack_count = 1;

      for (guint index = 0; index < node_count; index++)
        visited[index] = -1;
      visited[root] = root;
      stack[0] = root;

      while (stack_count > 0)
        {
          const guint source = stack[--stack_count];
          gint32 source_to_root[6];
          const gint32 root_strength = capacity_relation_load (
            self, probe_relations, root, source, source_to_root);

          strengths[root][source] = root_strength;
          strengths[source][root] = root_strength;

          /* Visits candidates from the probe down to slot zero.
           * The LIFO work list consequently processes lower slots first. */
          for (gint candidate_signed = (gint) count;
               candidate_signed >= 0; candidate_signed--)
            {
              const guint candidate = (guint) candidate_signed;
              gint32 candidate_to_source[6];
              gint32 candidate_to_root[6];
              gint32 edge_strength;
              gint32 path_strength;
              gint32 existing_strength;

              if (visited[candidate] >= 0)
                continue;
              edge_strength = capacity_relation_load (
                self, probe_relations, source, candidate,
                candidate_to_source);
              if (edge_strength < 1)
                continue;

              visited[candidate] = root;
              strengths[source][candidate] = edge_strength;
              strengths[candidate][source] = edge_strength;
              path_strength = MIN (root_strength, edge_strength);
              stack[stack_count++] = candidate;

              existing_strength = capacity_relation_load (
                self, probe_relations, root, candidate,
                candidate_to_root);
              if (root == source || existing_strength > 2 ||
                  (existing_strength == 2 &&
                   strengths[root][candidate] >= path_strength))
                continue;

              compose_group_transform (source_to_root,
                                       candidate_to_source,
                                       candidate_to_root);
              capacity_relation_store (self, probe_relations, root,
                                       candidate, candidate_to_root, 2);
              strengths[root][candidate] = path_strength;

              if (candidate < count)
                {
                  GoodixChicagoSubtemplate *root_subtemplate =
                    g_ptr_array_index (self->subtemplates, root);
                  GoodixChicagoSubtemplate *candidate_subtemplate =
                    g_ptr_array_index (self->subtemplates, candidate);

                  if (root_subtemplate->group_state == 1 ||
                      candidate_subtemplate->group_state == 1)
                    root_subtemplate->group_state =
                      candidate_subtemplate->group_state = 1;
                }
            }
        }
    }
  return TRUE;
}

gboolean
goodix_chicago_enrollment_calculate_capacity_scores (
  const GoodixChicagoEnrollment      *self,
  const GoodixChicagoSubtemplateView *probe,
  const GoodixChicagoRelation        *relations,
  guint                               relation_count,
  gint32                             *probe_score,
  gint32                              gallery_scores[GOODIX_CHICAGO_ENROLLMENT_CAPACITY],
  gint32                              gallery_qualities[GOODIX_CHICAGO_ENROLLMENT_CAPACITY])
{
  GoodixChicagoCapacityPolygon polygons[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { 0, };
  const guint count = self ? self->subtemplates->len : 0;
  guint polygon_count = 0;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (relations != NULL, FALSE);
  g_return_val_if_fail (probe_score != NULL, FALSE);
  g_return_val_if_fail (gallery_scores != NULL, FALSE);
  g_return_val_if_fail (gallery_qualities != NULL, FALSE);
  if (count != self->capacity || relation_count != count ||
      !probe->has_metric_data)
    return FALSE;

  for (guint index = 0; index < count; index++)
    if (relations[index].inlier_count >= 0)
      {
        gint32 inverse[6];

        invert_group_transform (relations[index].transform, inverse);
        if (capacity_build_polygon (inverse,
                                    GOODIX_CHICAGO_FEATURE_WIDTH,
                                    GOODIX_CHICAGO_FEATURE_HEIGHT,
                                    &polygons[polygon_count]))
          polygon_count++;
      }
  *probe_score = capacity_redundancy_score (
    probe->metric_data, polygons, polygon_count);

  for (guint target = 0; target < count; target++)
    {
      GoodixChicagoSubtemplate *target_subtemplate =
        g_ptr_array_index (self->subtemplates, target);

      polygon_count = 0;
      gallery_qualities[target] = target_subtemplate->quality;
      if (!target_subtemplate->has_metric_data)
        {
          gallery_scores[target] = 100;
          continue;
        }
      for (guint source = 0; source < count; source++)
        {
          const GoodixChicagoRelation *relation;
          gint32 inverse[6];
          const gint32 *transform;

          if (source == target)
            {
              relation = &relations[target];
              transform = relation->transform;
            }
          else if (source < target)
            {
              relation = relation_between_subtemplates (self, target, source);
              if (!relation || relation->inlier_count < 0)
                continue;
              invert_group_transform (relation->transform, inverse);
              transform = inverse;
            }
          else
            {
              relation = relation_between_subtemplates (self, source, target);
              transform = relation ? relation->transform : NULL;
            }
          if (!relation || relation->inlier_count < 0 || !transform)
            continue;
          if (capacity_build_polygon (transform,
                                      GOODIX_CHICAGO_FEATURE_WIDTH,
                                      GOODIX_CHICAGO_FEATURE_HEIGHT,
                                      &polygons[polygon_count]))
            polygon_count++;
        }
      gallery_scores[target] = capacity_redundancy_score (
        &target_subtemplate->metric_data, polygons, polygon_count);
    }
  return TRUE;
}

static gboolean
select_capacity_replacement (
  GoodixChicagoEnrollment             *self,
  const GoodixChicagoSubtemplateView  *probe,
  guint                                selected_index,
  GoodixChicagoRelation               *probe_relations,
  guint                                relation_count,
  guint                               *replacement_index,
  gboolean                             reconstruct_gallery,
  GoodixChicagoCapacityRelationBuilder build_unresolved_relation)
{
  gint32 probe_score;
  gint32 scores[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  gint32 qualities[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  gboolean visited[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  const guint count = self != NULL ? self->subtemplates->len : 0;
  gint32 best_score;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (probe_relations != NULL, FALSE);
  g_return_val_if_fail (replacement_index != NULL, FALSE);
  *replacement_index = G_MAXUINT;
  if (count != self->capacity || selected_index >= count ||
      relation_count != count)
    return FALSE;

  /* Unpack has already run the gallery-only closure by
   * the time starts.  Native packed templates retain only the
   * spanning forest, so reconstruct that transient state here. */
  if ((reconstruct_gallery &&
       !goodix_chicago_enrollment_synthesize_capacity_relations (
         self, NULL, count)) ||
      !propagate_capacity_relations (
        self, probe_relations, count, visited))
    return FALSE;
  if (!refine_unresolved_capacity_relations (
        self, probe, probe_relations, visited, build_unresolved_relation))
    return FALSE;
  if (!goodix_chicago_enrollment_synthesize_capacity_relations (
        self, probe_relations, count) ||
      !goodix_chicago_enrollment_calculate_capacity_scores (
        self, probe, probe_relations, count, &probe_score, scores, qualities))
    return FALSE;

  best_score = probe_score;
  for (guint index = 0; index < count; index++)
    {
      const GoodixChicagoSubtemplate *candidate =
        g_ptr_array_index (self->subtemplates, index);

      if ((guint64) candidate->record_count * 50 >
          (guint64) probe->record_count * 100 ||
          scores[index] <= best_score)
        continue;
      best_score = scores[index];
      *replacement_index = index;
    }
  if (*replacement_index != G_MAXUINT)
    return TRUE;

  {
    const GoodixChicagoSubtemplate *selected =
      g_ptr_array_index (self->subtemplates, selected_index);
    const guint quality_ratio = selected->quality < 60 ? 90 : 80;

    if ((scores[selected_index] > 95 && probe->quality > 50) ||
        (probe->coverage > 80 &&
         probe_score * 9 < scores[selected_index] * 10 &&
         (guint64) probe->quality * 100 >
         (guint64) selected->quality * quality_ratio))
      *replacement_index = selected_index;
  }
  return TRUE;
}

gboolean
goodix_chicago_enrollment_select_capacity_replacement (
  GoodixChicagoEnrollment             *self,
  const GoodixChicagoSubtemplateView  *probe,
  guint                                selected_index,
  GoodixChicagoRelation               *probe_relations,
  guint                                relation_count,
  GoodixChicagoCapacityRelationBuilder build_unresolved_relation,
  guint                               *replacement_index)
{
  return select_capacity_replacement (
    self, probe, selected_index, probe_relations, relation_count,
    replacement_index, TRUE, build_unresolved_relation);
}

static void
group_transform_between (const GoodixChicagoEnrollment *self,
                         guint                          source,
                         guint                          target,
                         gint32                         output[6])
{
  const GoodixChicagoRelation *relation;

  if (source == target)
    {
      identity_group_transform (output);
      return;
    }
  if (source > target)
    {
      relation = relation_between_subtemplates (self, source, target);
      if (relation)
        memcpy (output, relation->transform, sizeof (relation->transform));
      else
        identity_group_transform (output);
      return;
    }
  relation = relation_between_subtemplates (self, target, source);
  if (relation)
    invert_group_transform (relation->transform, output);
  else
    identity_group_transform (output);
}

static void
synthesize_first_group_relations (GoodixChicagoEnrollment *self,
                                  guint                    current_index)
{
  const GoodixChicagoSubtemplate *current =
    g_ptr_array_index (self->subtemplates, current_index);

  for (guint newer = 1; newer < current_index; newer++)
    for (guint older = 0; older < newer; older++)
      {
        GoodixChicagoRelation *existing;
        const GoodixChicagoRelation *current_to_older;
        const GoodixChicagoRelation *current_to_newer;
        gint32 newer_to_current[6];

        existing = &g_array_index (
          self->relations, GoodixChicagoRelation,
          ((GoodixChicagoSubtemplate *)
           g_ptr_array_index (self->subtemplates, newer))->relation_base +
          older);
        if (existing->inlier_count >= 0)
          continue;
        current_to_older = &g_array_index (
          self->relations, GoodixChicagoRelation,
          current->relation_base + older);
        current_to_newer = &g_array_index (
          self->relations, GoodixChicagoRelation,
          current->relation_base + newer);
        if (current_to_older->inlier_count < 0 ||
            current_to_newer->inlier_count < 0)
          continue;
        invert_group_transform (current_to_newer->transform,
                                newer_to_current);
        compose_group_transform (current_to_older->transform,
                                 newer_to_current,
                                 existing->transform);
        existing->inlier_count = 0;
      }
}

static guint
position_map_uncovered_by_group (
  const GoodixChicagoEnrollment *self,
  const guint8                   position_map[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
  guint                          current_index,
  const gint32                   current_to_anchor[6])
{
  const gint64 maximum_x = (40 - 1) << 8;
  const gint64 maximum_y = (32 - 1) << 8;
  guint uncovered = 0;

  for (guint y = 0; y < 32; y++)
    for (guint x = 0; x < 40; x++)
      {
        const guint bit = y * 40 + x;
        gboolean covered = FALSE;

        if (((position_map[bit >> 3] >> (bit & 7)) & 1u) == 0)
          continue;
        for (guint index = 0;
             index < self->subtemplates->len && !covered;
             index++)
          {
            const GoodixChicagoSubtemplate *subtemplate =
              g_ptr_array_index (self->subtemplates, index);
            gint32 anchor_to_item[6];
            gint32 transform[6];

            if (index == current_index ||
                subtemplate->group_state != 1)
              continue;
            group_transform_between (self, self->group_anchor, index,
                                     anchor_to_item);
            compose_group_transform (anchor_to_item, current_to_anchor,
                                     transform);
            const gint64 mapped_x = (gint64) transform[0] * x +
                                    (gint64) transform[1] * y +
                                    (transform[2] >> 1);
            const gint64 mapped_y = (gint64) transform[3] * x +
                                    (gint64) transform[4] * y +
                                    (transform[5] >> 1);

            covered = mapped_x >= 0 && mapped_x <= maximum_x &&
                      mapped_y >= 0 && mapped_y <= maximum_y;
          }
        uncovered += !covered;
      }
  return uncovered < 20 ? 0 : uncovered;
}

/* Merging ungrouped subtemplates into the group, called when the template has a group,
 * the new subtemplate joined it and some accepted evidence subtemplates are still
 * ungrouped.  Relations R(a, b) (a > b) are the triangular slots; a merged subtemplate k
 * gets a synthetic relation with the anchor (inliers 0): R(k, anchor) = t, or
 * R(anchor, k) = t^-1 when k < anchor, where t is the k -> anchor chain below.
 * The selector is with inlier argument 0 (group_selector). */
static GoodixChicagoRelation *
group_relation (GoodixChicagoEnrollment *self, guint newer, guint older)
{
  const GoodixChicagoSubtemplate *subtemplate = g_ptr_array_index (self->subtemplates, newer);

  return &g_array_index (self->relations, GoodixChicagoRelation,
                         subtemplate->relation_base + older);
}

static GoodixChicagoSubtemplate *
group_sub (GoodixChicagoEnrollment *self, guint index)
{
  return g_ptr_array_index (self->subtemplates, index);
}

static void
group_join_anchor (GoodixChicagoEnrollment *self, guint k, const gint32 t[6])
{
  const guint anchor = self->group_anchor;
  GoodixChicagoRelation *relation;

  group_sub (self, k)->group_state = 1;
  if (k < anchor)
    {
      relation = group_relation (self, anchor, k);
      invert_group_transform (t, relation->transform);
    }
  else
    {
      relation = group_relation (self, k, anchor);
      memcpy (relation->transform, t, sizeof (relation->transform));
    }
  relation->inlier_count = 0;
}

static gint
group_pair_selector (GoodixChicagoEnrollment *self, guint newer, guint older)
{
  return group_selector (&group_sub (self, newer)->metric_data, &group_sub (self, older)->metric_data,
                         group_relation (self, newer, older)->transform);
}

/* Downwards from j.  Each ungrouped k < j (descending) joins when some
 * already joined node of the chain {j, joined k's} (latest first) has a relation to k
 * (inliers >= 0) with selector > threshold; its transform to the anchor goes back along
 * the chain through relations that exist. */
static void
group_merge_down (GoodixChicagoEnrollment *self, const gint32 j_to_anchor[6], guint j,
                  gint threshold)
{
  guint chain[GOODIX_CHICAGO_ENROLLMENT_CAPACITY + 1];
  guint chain_len = 1;

  chain[0] = j;
  for (gint k = (gint) j - 1; k >= 0; k--)
    {
      if (group_sub (self, k)->group_state == 1)
        continue;
      for (gint m = (gint) chain_len - 1; m >= 0; m--)
        {
          gint32 x[6], inverse[6], t[6];
          guint last = m;

          if (group_relation (self, chain[m], k)->inlier_count <= -1 ||
              group_pair_selector (self, chain[m], k) <= threshold)
            continue;
          memcpy (x, group_relation (self, chain[m], k)->transform, sizeof (x));
          for (gint step = m - 1; step >= 0; step--)
            {
              const GoodixChicagoRelation *link = group_relation (self, chain[step], chain[last]);

              if (link->inlier_count > -1)
                {
                  gint32 composed[6];

                  compose_group_transform (link->transform, x, composed);
                  memcpy (x, composed, sizeof (x));
                  last = step;
                }
            }
          invert_group_transform (x, inverse);
          compose_group_transform (j_to_anchor, inverse, t);
          group_join_anchor (self, k, t);
          chain[chain_len++] = k;
          break;
        }
    }
}

/* Upwards from j, k = j + 1 .. count - 2 (the new subtemplate excluded):
 * an ungrouped k with a relation to j (inliers >= 0) and selector > threshold joins. */
static void
group_merge_up (GoodixChicagoEnrollment *self, const gint32 j_to_anchor[6], guint j,
                gint threshold)
{
  const guint count = self->subtemplates->len;

  for (guint k = j + 1; k + 2 <= count && k <= count - 2; k++)
    {
      gint32 t[6];

      if (group_sub (self, k)->group_state == 1 ||
          group_relation (self, k, j)->inlier_count <= -1 ||
          group_pair_selector (self, k, j) <= threshold)
        continue;
      compose_group_transform (j_to_anchor, group_relation (self, k, j)->transform, t);
      group_join_anchor (self, k, t);
    }
}

static void
group_merge_into_group (GoodixChicagoEnrollment *self, guint new_index,
                        const guint *ungrouped, guint ungrouped_count)
{
  for (guint i = 0; i < ungrouped_count; i++)
    {
      const guint j = ungrouped[i];
      gint32 inverse[6], j_to_anchor[6];

      if (group_pair_selector (self, new_index, j) <= 208)
        continue;
      invert_group_transform (group_relation (self, new_index, j)->transform, inverse);
      compose_group_transform (group_relation (self, new_index, self->group_anchor)->transform,
                               inverse, j_to_anchor);
      group_join_anchor (self, j, j_to_anchor);
      group_merge_down (self, j_to_anchor, j, 205);
      group_merge_up (self, j_to_anchor, j, 205);
    }
}

gboolean
goodix_chicago_enrollment_insert_next (
  GoodixChicagoEnrollment          *self,
  const GoodixChicagoFeatureRecord *records,
  guint                             record_count,
  guint                             active_count,
  guint                             quality,
  guint                             coverage,
  const GoodixChicagoMetricData    *metric_data,
  GoodixChicagoEnrollmentResult    *result,
  GError                          **error)
{
  g_autofree GoodixChicagoSubtemplate **old_subtemplates = NULL;
  g_autofree GoodixChicagoRelation *relations = NULL;
  g_autofree gboolean *evidence = NULL;
  GoodixChicagoSubtemplate *new_subtemplate;
  guint old_count;
  guint candidate = G_MAXUINT;
  guint geometry_relation = G_MAXUINT;
  guint best_inliers = 0;
  gboolean has_group = FALSE;
  gboolean has_current_to_anchor = FALSE;
  gint selector_score = 0;
  gint32 current_to_anchor[6];
  guint position_loss = 80 * 64;
  guint evidence_count = 0;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  memset (result, 0, sizeof (*result));
  old_count = self->subtemplates->len;
  if (old_count == 0 || old_count >= self->capacity)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "gdix51c0: later insertion requires available enrollment state");
      return FALSE;
    }
  if ((record_count != 0 && records == NULL) ||
      record_count > self->record_limit || active_count > record_count ||
      metric_data == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "gdix51c0: invalid Chicago second subtemplate");
      return FALSE;
    }
  old_subtemplates = g_new (GoodixChicagoSubtemplate *, old_count);
  relations = g_new0 (GoodixChicagoRelation, old_count);
  evidence = g_new0 (gboolean, old_count);
  for (guint index = 0; index < old_count; index++)
    {
      gint metric_a;
      gint metric_b;

      old_subtemplates[index] = g_ptr_array_index (self->subtemplates, index);
      if (!old_subtemplates[index]->has_metric_data)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "gdix51c0: existing subtemplate has no metric data");
          return FALSE;
        }
      has_group |= old_subtemplates[index]->group_state == 1;
      goodix_chicago_enrollment_build_relation (
        old_subtemplates[index]->records,
        old_subtemplates[index]->record_count,
        &old_subtemplates[index]->metric_data, records, record_count,
        metric_data, &relations[index], &metric_a, &metric_b,
        &evidence[index]);
      /* The evidence metric predicate controls the accepted-evidence index
       * list, but the triangular relation table keeps every affine result
       * with at least six inliers.  Only weaker transforms become the
       * identity sentinel. */
      if (!evidence[index])   /* Keep only accepted evidence */
        {
          relations[index].inlier_count = -1;
          memcpy (relations[index].transform,
                  (const gint32[6]){ 0x100, 0, 0, 0, 0x100, 0 },
                  sizeof (relations[index].transform));
        }
    }

  for (guint index = 0; index < old_count; index++)
    {
      if (!evidence[index] || relations[index].inlier_count < 6 ||
          (has_group && old_subtemplates[index]->group_state != 1))
        continue;
      if (candidate == G_MAXUINT ||
          relations[index].inlier_count > best_inliers)
        {
          candidate = index;
          best_inliers = relations[index].inlier_count;
        }
    }
  /* The selector is the full result (second map pair included). */
  if (candidate != G_MAXUINT)
    selector_score = group_selector (metric_data, &old_subtemplates[candidate]->metric_data,
                                     relations[candidate].transform);

  new_subtemplate = g_new0 (GoodixChicagoSubtemplate, 1);
  new_subtemplate->record_count = record_count;
  new_subtemplate->active_count = active_count;
  new_subtemplate->quality = quality;
  new_subtemplate->coverage = coverage;
  new_subtemplate->lineage_index = old_count;
  new_subtemplate->records = g_memdup2 (records,
                                        record_count * sizeof (*records));
  new_subtemplate->has_metric_data = TRUE;
  new_subtemplate->metric_data = *metric_data;
  new_subtemplate->relation_base = self->relations->len;
  if (candidate != G_MAXUINT && (has_group || selector_score > 205))
    {
      new_subtemplate->group_state = 1;
      if (!has_group)
        {
          self->group_anchor = candidate;
          self->has_group_anchor = TRUE;
          /* Flags only the anchor candidate and the new sample. */
          old_subtemplates[candidate]->group_state = 1;
        }
    }
  g_ptr_array_add (self->subtemplates, new_subtemplate);
  self->match_order[old_count] = old_count;
  g_array_append_vals (self->relations, relations, old_count);
  self->transform_count = self->relations->len;
  if (!has_group && new_subtemplate->group_state == 1)
    {
      synthesize_first_group_relations (self, old_count);
    }
  else if (has_group && new_subtemplate->group_state == 1)
    {
      GoodixChicagoRelation *anchor_relation;
      gint32 current_to_candidate[6];

      group_transform_between (self, old_count, candidate,
                               current_to_candidate);
      if (candidate == self->group_anchor)
        {
          memcpy (current_to_anchor, current_to_candidate,
                  sizeof (current_to_anchor));
        }
      else
        {
          gint32 candidate_to_anchor[6];

          group_transform_between (self, candidate, self->group_anchor,
                                   candidate_to_anchor);
          compose_group_transform (candidate_to_anchor,
                                   current_to_candidate,
                                   current_to_anchor);
        }

      /* Canonicalizes the new-to-anchor transform in
       * relation_base + group_anchor for subsequent runtime geometry.  The
       * packed graph is rebuilt separately from real
       * relations with more than two inliers, so this synthetic zero-inlier
       * anchor slot is deliberately not serialized. */
      anchor_relation = relation_at (
        self, new_subtemplate->relation_base + self->group_anchor);
      if (anchor_relation)
        {
          memcpy (anchor_relation->transform, current_to_anchor,
                  sizeof (anchor_relation->transform));
          if (candidate != self->group_anchor &&
              anchor_relation->inlier_count < 0)
            anchor_relation->inlier_count = 0;
        }
      has_current_to_anchor = TRUE;
    }

  /* After joining/creating, accepted evidence still
   * ungrouped is merged into the group. */
  if (self->has_group_anchor && new_subtemplate->group_state == 1)
    {
      g_autofree guint *ungrouped = g_new (guint, MAX (old_count, 1u));
      guint ungrouped_count = 0;

      for (guint index = 0; index < old_count; index++)
        if (evidence[index] && old_subtemplates[index]->group_state == 0)
          ungrouped[ungrouped_count++] = index;
      if (ungrouped_count > 0)
        group_merge_into_group (self, old_count, ungrouped, ungrouped_count);
    }
  else
    {
      /* No group to merge into.  Among the ungrouped accepted
       * evidence with selector(new, j) > 205, the smallest area of
       * the new frame outside frame j (80*64 - overlap) becomes
       * the position loss (initially the whole area); the position word gets
       * 0x100 + (j << 8), which only touches the middle bytes of the result. */
      for (guint index = 0; index < old_count; index++)
        if (evidence[index] && old_subtemplates[index]->group_state == 0 &&
            group_pair_selector (self, old_count, index) > 205)
          {
            const guint uncovered = 80 * 64 - affine_rectangle_overlap (
              group_relation (self, old_count, index)->transform, 80, 64, FALSE);

            if (uncovered < position_loss)
              position_loss = uncovered;
          }
    }
  for (guint index = 0; index < old_count; index++)
    evidence_count += evidence[index] != FALSE;

  result->packed_position_detail = GOODIX_CHICAGO_FIRST_POSITION_DETAIL;
  /* Walks backward only to skip subtemplates whose
   * serialized adaptive state is 5, then tests that single relation.  Fresh
   * enrollment initializes the state to zero, so this is the immediately
   * preceding subtemplate.  A relation below six inliers leaves the fallback
   * high position byte intact; no older usable relation is searched for. */
  if (old_count > 0 && relations[old_count - 1].inlier_count >= 6)
    geometry_relation = old_count - 1;
  if (new_subtemplate->group_state == 1 && self->has_group_anchor)
    {
      if (!has_current_to_anchor)
        {
          gint32 current_to_candidate[6];

          group_transform_between (self, old_count, candidate,
                                   current_to_candidate);
          if (candidate == self->group_anchor)
            {
              memcpy (current_to_anchor, current_to_candidate,
                      sizeof (current_to_anchor));
            }
          else
            {
              gint32 candidate_to_anchor[6];

              group_transform_between (self, candidate,
                                       self->group_anchor,
                                       candidate_to_anchor);
              compose_group_transform (candidate_to_anchor,
                                       current_to_candidate,
                                       current_to_anchor);
            }
        }
      position_loss = position_map_uncovered_by_group (
        self, metric_data->position_map, old_count,
        current_to_anchor) * 4;
    }
  if (geometry_relation != G_MAXUINT)
    {
      const GoodixChicagoRelation *relation =
        &relations[geometry_relation];
      const guint full_overlap = affine_rectangle_overlap (
        relation->transform, 80, 64, FALSE);
      const guint geometric_penalty =
        ((80 * 64 - full_overlap) * 100) / (80 * 64 + 1);

      result->packed_position_detail =
        (geometric_penalty << 24) | 0x100;
      result->packed_position_detail +=
        (position_loss * 100) / (80 * 64 + 1);
    }
  else if (evidence_count > 0)
    {
      /* With accepted evidence the result is the position word
       * (high byte 0x64 unless the walk-back relation above sets the penalty)
       * plus position_loss * 100 / (80*64 + 1); only without evidence 0x64000064. */
      result->packed_position_detail &= 0xff000000;
      result->packed_position_detail |= 0x100;
      result->packed_position_detail +=
        (position_loss * 100) / (80 * 64 + 1);
    }
  result->position_x = 100 - (result->packed_position_detail >> 24);
  result->position_y =
    100 - (result->packed_position_detail & 0xff);
  result->progress = MIN (100u,
                          self->subtemplates->len * 100u /
                          self->required_samples);
  return TRUE;
}

/* Selects the largest connected component formed by
 * relations with more than two inliers.  It clears all previous group flags
 * and only marks a component when it contains at least two subtemplates. */
static void
rebuild_matcher_group (GoodixChicagoEnrollment *self)
{
  g_autofree gint *component = NULL;
  g_autofree guint *component_size = NULL;
  g_autofree guint *stack = NULL;
  const guint count = self->subtemplates->len;
  guint best_root = G_MAXUINT;
  guint best_size = 0;

  component = g_new (gint, count);
  component_size = g_new0 (guint, count);
  stack = g_new (guint, count);
  /* Resets the group anchor/flag and group_state only; matcher_value_c
   * (tag 0xa7) is the study append counter, not a group flag. */
  self->has_group_anchor = FALSE;
  for (guint index = 0; index < count; index++)
    {
      GoodixChicagoSubtemplate *subtemplate =
        g_ptr_array_index (self->subtemplates, index);

      component[index] = -1;
      subtemplate->group_state = 0;
    }

  for (guint root = 0; root < count; root++)
    {
      guint stack_count;

      if (component[root] >= 0)
        continue;
      component[root] = root;
      component_size[root] = 1;
      stack[0] = root;
      stack_count = 1;
      while (stack_count != 0)
        {
          const guint current = stack[--stack_count];

          for (guint candidate = count; candidate-- > 0;)
            {
              const GoodixChicagoRelation *relation;

              if (candidate == current || component[candidate] >= 0)
                continue;
              relation = current > candidate ?
                         relation_between_subtemplates (self, current, candidate) :
                         relation_between_subtemplates (self, candidate, current);
              if (relation == NULL || relation->inlier_count <= 2)
                continue;
              component[candidate] = root;
              component_size[root]++;
              stack[stack_count++] = candidate;
            }
        }
    }

  for (guint root = 0; root < count; root++)
    if (component_size[root] > best_size)
      {
        best_size = component_size[root];
        best_root = root;
      }
  if (best_size <= 1)
    return;

  self->group_anchor = best_root;
  self->has_group_anchor = TRUE;
  for (guint index = 0; index < count; index++)
    if ((guint) component[index] == best_root)
      ((GoodixChicagoSubtemplate *)
       g_ptr_array_index (self->subtemplates, index))->group_state = 1;
}

/* Identify's study_value_a increments, applied to the
 * gallery that template study updates and repacks. */
void
goodix_chicago_enrollment_apply_study_hits (GoodixChicagoEnrollment *self,
                                            const guint32           *hits,
                                            guint                    count)
{
  g_return_if_fail (self != NULL);
  for (guint index = 0; index < count && index < self->subtemplates->len; index++)
    ((GoodixChicagoSubtemplate *) g_ptr_array_index (self->subtemplates, index))->study_value_a +=
      hits[index];
}

gboolean
goodix_chicago_enrollment_append_study (
  GoodixChicagoEnrollment             *self,
  const GoodixChicagoSubtemplateView  *probe,
  guint                                selected_index,
  const GoodixChicagoRelation         *relations,
  guint                                relation_count,
  guint                               *appended_index,
  GoodixChicagoCapacityRelationBuilder build_unresolved_relation,
  GError                             **error)
{
  GoodixChicagoSubtemplate *source;
  GoodixChicagoSubtemplate *appended;
  guint old_count;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (probe != NULL, FALSE);
  old_count = self->subtemplates->len;
  if (old_count == 0 || old_count >= self->capacity ||
      selected_index >= old_count)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "gdix51c0: adaptive study requires available gallery state");
      return FALSE;
    }
  if (relations == NULL || relation_count != old_count ||
      !probe->has_metric_data || probe->metric_data == NULL ||
      (probe->record_count != 0 && probe->records == NULL) ||
      probe->record_count > self->record_limit ||
      probe->active_count > probe->record_count)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "gdix51c0: invalid adaptive study subtemplate");
      return FALSE;
    }

  /* Mode 1: the new slot keeps identify's relations with
   * inlier >= 1 (others: -1, identity, zero translation), then the relations of the new node are
   * propagated through the gallery graph (strength-2 synthetic, no group change) and slots
   * neither reached nor marked -2 by identify are rematched from features. */
  GoodixChicagoRelation fresh[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  gboolean visited[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];

  for (guint index = 0; index < relation_count; index++)
    if (relations[index].inlier_count >= 1)
      fresh[index] = relations[index];
    else
      fresh[index] = (GoodixChicagoRelation){ .inlier_count = -1,
                                              .transform = { 256, 0, 0, 0, 256, 0 } };
  propagate_capacity_relations (self, fresh, relation_count, visited);
  for (guint index = 0; index < relation_count; index++)
    if (!visited[index] && relations[index].inlier_count != -2 && build_unresolved_relation)
      {
        GoodixChicagoSubtemplateView gallery;

        if (goodix_chicago_enrollment_get_subtemplate (self, index, &gallery))
          build_unresolved_relation (&gallery, probe, &fresh[index]);
      }
  source = g_ptr_array_index (self->subtemplates, selected_index);
  /* Copies study_value_a/b of the source; the study_value_a increments are
   * identify's (goodix_chicago_enrollment_apply_study_hits). */
  appended = g_new0 (GoodixChicagoSubtemplate, 1);
  appended->record_count = probe->record_count;
  appended->active_count = probe->active_count;
  appended->quality = probe->quality;
  appended->coverage = probe->coverage;
  appended->records = g_memdup2 (
    probe->records, probe->record_count * sizeof (*probe->records));
  appended->has_metric_data = TRUE;
  appended->metric_data = *probe->metric_data;
  appended->group_state = source->group_state;
  appended->relation_base = self->relations->len;
  appended->study_state = 1;
  appended->lineage_index = old_count;
  appended->study_value_a = source->study_value_a;
  appended->study_value_b = source->study_value_b;
  g_ptr_array_add (self->subtemplates, appended);
  g_array_append_vals (self->relations, fresh, relation_count);
  self->transform_count = self->relations->len;
  self->match_order[old_count] = old_count;
  self->matcher_value_c++;                          /* study append counter */
  /* Gallery-only relation closure with group propagation. */
  goodix_chicago_enrollment_synthesize_capacity_relations (self, NULL, self->subtemplates->len);
  /* Groups are rebuilt only without a group
   * or when the changed slot is the group anchor (never for an appended one). */
  if (!self->has_group_anchor || self->group_anchor == old_count)
    rebuild_matcher_group (self);
  if (appended_index != NULL)
    *appended_index = old_count;
  return TRUE;
}

gboolean
goodix_chicago_enrollment_replace_study (
  GoodixChicagoEnrollment            *self,
  const GoodixChicagoSubtemplateView *probe,
  guint                               selected_index,
  guint                               replacement_index,
  const GoodixChicagoRelation        *relations,
  guint                               relation_count,
  GError                            **error)
{
  GoodixChicagoSubtemplate *source;
  GoodixChicagoSubtemplate *replacement;
  guint old_lineage;
  guint replacement_count;
  guint study_value_a;
  guint study_value_b;
  const guint count = self != NULL ? self->subtemplates->len : 0;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (probe != NULL, FALSE);
  if (count != self->capacity || selected_index >= count ||
      replacement_index >= count || relations == NULL ||
      relation_count != count || !probe->has_metric_data ||
      probe->metric_data == NULL ||
      (probe->record_count != 0 && probe->records == NULL) ||
      probe->record_count > self->record_limit ||
      probe->active_count > probe->record_count)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "gdix51c0: invalid capacity-full adaptive study replacement");
      return FALSE;
    }

  source = g_ptr_array_index (self->subtemplates, selected_index);
  replacement = g_ptr_array_index (self->subtemplates, replacement_index);

  /* Identify's retained source counter is advanced before the capacity
   * selector.  Native
   * identify is side-effect free, so apply that transition at this boundary,
   * as append_study() does for the under-capacity path. */
  /* No increment here, as in the append path (identify's +0x124 hits are applied by
   * goodix_chicago_enrollment_apply_study_hits). */
  study_value_a = source->study_value_a;
  study_value_b = source->study_value_b;
  /* Clears BC while copying the probe into an existing slot.  The
   * subsequent renumber therefore compacts every positive BC value,
   * then assigns the replacement the newest value. */
  old_lineage = 0;
  replacement_count = replacement->replacement_count + 1;

  g_free (replacement->records);
  replacement->record_count = probe->record_count;
  replacement->active_count = probe->active_count;
  replacement->quality = probe->quality;
  replacement->coverage = probe->coverage;
  replacement->records = g_memdup2 (
    probe->records, probe->record_count * sizeof (*probe->records));
  replacement->has_metric_data = TRUE;
  replacement->metric_data = *probe->metric_data;
  replacement->study_state = 2;
  replacement->replacement_count = replacement_count;
  replacement->study_value_a = study_value_a;
  replacement->study_value_b = study_value_b;

  for (guint index = 0; index < count; index++)
    {
      GoodixChicagoSubtemplate *subtemplate =
        g_ptr_array_index (self->subtemplates, index);

      if (index != replacement_index &&
          subtemplate->lineage_index > old_lineage)
        subtemplate->lineage_index--;
    }
  replacement->lineage_index = count - 1;

  for (guint index = 0; index < count; index++)
    {
      GoodixChicagoRelation *destination;

      if (index == replacement_index)
        continue;
      if (index < replacement_index)
        {
          destination = (GoodixChicagoRelation *)
                        relation_between_subtemplates (self, replacement_index, index);
          *destination = relations[index];
        }
      else
        {
          destination = (GoodixChicagoRelation *)
                        relation_between_subtemplates (self, index, replacement_index);
          destination->inlier_count = relations[index].inlier_count;
          invert_group_transform (relations[index].transform,
                                  destination->transform);
        }
    }

  /* Advances packed matcher scalar A6 for every replacement. */
  self->matcher_value_b++;
  /* The normal packed-gallery state has the group flag set and the
   * replacement is not the group anchor, so the existing group flags are
   * deliberately preserved instead of rebuilding the group. */
  return TRUE;
}

guint
goodix_chicago_enrollment_get_count (
  const GoodixChicagoEnrollment *self)
{
  g_return_val_if_fail (self != NULL, 0);
  return self->subtemplates->len;
}

gboolean
goodix_chicago_enrollment_get_match_order_index (
  const GoodixChicagoEnrollment *self,
  guint                          schedule_index,
  guint                         *gallery_index)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (gallery_index != NULL, FALSE);
  if (schedule_index >= self->subtemplates->len ||
      self->match_order[schedule_index] >= self->subtemplates->len)
    return FALSE;
  *gallery_index = self->match_order[schedule_index];
  return TRUE;
}

guint
goodix_chicago_enrollment_get_capacity (
  const GoodixChicagoEnrollment *self)
{
  g_return_val_if_fail (self != NULL, 0);
  return self->capacity;
}

static guint
build_serialized_relation_mask (const GoodixChicagoEnrollment *self,
                                guint8                        *mask)
{
  g_autofree gint *component = NULL;
  g_autofree gint *parent = NULL;
  g_autofree guint *stack = NULL;
  g_autofree guint8 *owned_mask = NULL;
  guint8 *effective_mask = mask;
  const guint count = self->subtemplates->len;
  guint selected = 0;

  component = g_new (gint, count);
  parent = g_new (gint, count);
  stack = g_new (guint, count);
  for (guint index = 0; index < count; index++)
    component[index] = parent[index] = -1;
  if (effective_mask == NULL)
    {
      owned_mask = g_new0 (guint8, self->relations->len);
      effective_mask = owned_mask;
    }
  else if (self->relations->len != 0)
    {
      memset (effective_mask, 0, self->relations->len);
    }

  /* Builds a depth-first spanning forest over the
   * triangular relation table.  Neighbours are discovered from the highest
   * subtemplate index down and consumed from a LIFO stack; only those
   * parent-child edges are serialized.  Synthetic anchor relations
   * have inlier count zero, so they remain available to runtime geometry but
   * are deliberately excluded from the packed graph. */
  for (guint root = 0; root < count; root++)
    {
      guint stack_count;

      if (component[root] >= 0)
        continue;
      component[root] = root;
      stack[0] = root;
      stack_count = 1;
      while (stack_count != 0)
        {
          const guint current = stack[--stack_count];

          for (guint candidate = count; candidate-- > 0;)
            {
              const GoodixChicagoRelation *relation;

              if (candidate == current || component[candidate] >= 0)
                continue;
              relation = current > candidate ?
                         relation_between_subtemplates (self, current, candidate) :
                         relation_between_subtemplates (self, candidate, current);
              if (relation == NULL || relation->inlier_count <= 2)
                continue;
              component[candidate] = root;
              parent[candidate] = current;
              stack[stack_count++] = candidate;
            }
        }
    }

  for (guint index = 0; index < count; index++)
    if (parent[index] >= 0)
      {
        const guint other = parent[index];
        const guint newer = MAX (index, other);
        const guint older = MIN (index, other);
        const GoodixChicagoSubtemplate *subtemplate =
          g_ptr_array_index (self->subtemplates, newer);
        const guint relation_index = subtemplate->relation_base + older;

        if (relation_index >= self->relations->len)
          continue;
        if (effective_mask[relation_index] == 0)
          {
            effective_mask[relation_index] = 1;
            selected++;
          }
      }
  /* Also reconnects grouped components using the first available
   * non-negative relation.  This retains a rewritten strength-2 forest edge
   * after capacity replacement and emits the zero-strength anchor used for a
   * disconnected but still grouped gallery node.  A component whose only
   * relations are the final -1/-2 sentinels remains disconnected. */
  {
    guint anchor = G_MAXUINT;

    for (guint index = 0; index < count; index++)
      if (((const GoodixChicagoSubtemplate *)
           g_ptr_array_index (self->subtemplates, index))->group_state == 1)
        {
          anchor = index;
          break;
        }
    if (anchor != G_MAXUINT)
      {
        for (guint current = 0; current < count; current++)
          {
            const GoodixChicagoSubtemplate *current_subtemplate =
              g_ptr_array_index (self->subtemplates, current);
            const gint current_component = component[current];
            const gint anchor_component = component[anchor];
            guint relation_index = G_MAXUINT;

            if (current_subtemplate->group_state != 1 ||
                current_component == anchor_component)
              continue;
            for (guint member = 0;
                 member < count && relation_index == G_MAXUINT; member++)
              {
                const GoodixChicagoSubtemplate *member_subtemplate =
                  g_ptr_array_index (self->subtemplates, member);

                if (component[member] != current_component ||
                    member_subtemplate->group_state != 1)
                  continue;
                for (guint target = 0; target < count; target++)
                  {
                    const GoodixChicagoSubtemplate *target_subtemplate =
                      g_ptr_array_index (self->subtemplates, target);
                    const GoodixChicagoRelation *relation;
                    const guint newer = MAX (member, target);
                    const guint older = MIN (member, target);

                    if (component[target] != anchor_component ||
                        target_subtemplate->group_state != 1)
                      continue;
                    relation = relation_between_subtemplates (
                      self, newer, older);
                    if (relation == NULL || relation->inlier_count < 0)
                      continue;
                    relation_index =
                      ((const GoodixChicagoSubtemplate *)
                       g_ptr_array_index (self->subtemplates, newer))->
                      relation_base + older;
                    break;
                  }
              }
            if (relation_index == G_MAXUINT)
              continue;
            if (effective_mask[relation_index] == 0)
              {
                effective_mask[relation_index] = 1;
                selected++;
              }
            for (guint index = 0; index < count; index++)
              if (component[index] == current_component)
                component[index] = anchor_component;
          }
      }
  }
  return selected;
}

gsize
goodix_chicago_enrollment_get_packed_size (
  const GoodixChicagoEnrollment *self)
{
  gsize size = 1443;

  g_return_val_if_fail (self != NULL, 0);
  for (guint index = 0; index < self->subtemplates->len; index++)
    {
      const GoodixChicagoSubtemplate *subtemplate =
        g_ptr_array_index (self->subtemplates, index);

      /* Packed size, template type 24: three 160-byte packed
       * planes, the 40-byte coarse mask, 32 bytes per feature record, and
       * the fixed tagged-field overhead. */
      /* The 80 x 64 path includes c7=0x200 in every
       * subtemplate container. */
      size += subtemplate->record_count * 32 + 685;
      if (subtemplate->metric_data.packed_resolution != 0)
        size += 5;
    }
  size += build_serialized_relation_mask (self, NULL) * 45;
  return size;
}

static void
packed_append_u32 (GByteArray *packed,
                   guint32     value)
{
  const guint32 little_endian = GUINT32_TO_LE (value);

  g_byte_array_append (packed, (const guint8 *) &little_endian,
                       sizeof (little_endian));
}

static void
packed_append_scalar (GByteArray *packed,
                      guint8      tag,
                      guint32     value)
{
  g_byte_array_append (packed, &tag, 1);
  packed_append_u32 (packed, value);
}

static void
packed_append_blob (GByteArray   *packed,
                    guint8        tag,
                    const guint8 *data,
                    guint32       size)
{
  g_byte_array_append (packed, &tag, 1);
  packed_append_u32 (packed, size);
  g_byte_array_append (packed, data, size);
}

static void
packed_append_map (GByteArray  *packed,
                   guint8       tag,
                   const guint8 data[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES])
{
  guint32 body_offset;

  g_byte_array_append (packed, &tag, 1);
  body_offset = packed->len;
  packed_append_u32 (packed, 0);
  packed_append_scalar (packed, 0xc1, 40);
  packed_append_scalar (packed, 0xc2, 32);
  packed_append_scalar (packed, 0xc3, G_MAXUINT32);
  packed_append_scalar (packed, 0xc4, 8);
  packed_append_blob (packed, 0xc5, data,
                      GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES);
  memcpy (&packed->data[body_offset],
          &(guint32){ GUINT32_TO_LE (packed->len - body_offset - 4) }, 4);
}

static guint8
packed_orientation (gint16 orientation)
{
  if (orientation >= 0)
    return orientation >> 8;
  return ((guint16) - orientation >> 8) + 0x80;
}

static void
packed_append_record (GByteArray                       *packed,
                      const GoodixChicagoFeatureRecord *record)
{
  static const guint8 swap[8] = { 0, 1, 1, 0, 1, 0, 0, 1 };
  guint8 descriptor[28];
  const guint8 *source = record->descriptor + 4;
  const guint32 coordinate =
    ((guint32) (guint16) record->refined_x << 16) |
    ((guint32) (guint16) record->refined_y << 4) |
    packed_orientation (record->orientation);

  for (guint index = 0; index < 8; index++)
    {
      const guint8 a = source[index];
      const guint8 b = source[index + 8];
      const guint8 first = ((a ^ b) & 0x0f) ^ b;
      const guint8 second = ((a ^ b) & 0x0f) ^ a;

      descriptor[index * 2] = swap[index] ? second : first;
      descriptor[index * 2 + 1] = swap[index] ? first : second;
    }
  memcpy (descriptor + 16, record->descriptor + 20, 4);
  memcpy (descriptor + 20, record->descriptor + 28, 8);
  packed_append_u32 (packed, coordinate);
  g_byte_array_append (packed, descriptor, sizeof (descriptor));
}

static void
packed_append_subtemplate (GByteArray                     *packed,
                           const GoodixChicagoSubtemplate *subtemplate)
{
  const guint8 tag = 0x95;
  guint32 body_offset;

  g_byte_array_append (packed, &tag, 1);
  body_offset = packed->len;
  packed_append_u32 (packed, 0);
  packed_append_map (packed, 0xb2, subtemplate->metric_data.primary);
  packed_append_map (packed, 0xcf, subtemplate->metric_data.secondary);
  packed_append_blob (packed, 0xce, subtemplate->metric_data.coarse_mask,
                      GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES);
  packed_append_map (packed, 0xcd, subtemplate->metric_data.validity);
  packed_append_scalar (packed, 0xb3, subtemplate->record_count);
  g_byte_array_append (packed, (const guint8[]){ 0xb4 }, 1);
  packed_append_u32 (packed, subtemplate->record_count * 32);
  for (guint record = 0; record < subtemplate->record_count; record++)
    packed_append_record (packed, &subtemplate->records[record]);
  packed_append_scalar (packed, 0xb5, subtemplate->group_state);
  packed_append_scalar (packed, 0xb6, subtemplate->relation_base);
  packed_append_scalar (packed, 0xb7, subtemplate->active_count);
  packed_append_scalar (packed, 0xb8, subtemplate->quality);
  packed_append_scalar (packed, 0xb9, subtemplate->coverage);
  packed_append_scalar (packed, 0xba, subtemplate->study_state);
  packed_append_scalar (packed, 0xbb, subtemplate->study_flags);
  packed_append_scalar (packed, 0xbc, subtemplate->lineage_index);
  packed_append_scalar (packed, 0xbd, subtemplate->replacement_count);
  packed_append_scalar (packed, 0xbe, subtemplate->study_value_a);
  packed_append_scalar (packed, 0xc0, subtemplate->study_value_b);
  if (subtemplate->metric_data.packed_resolution != 0)
    packed_append_scalar (packed, 0xc7,
                          subtemplate->metric_data.packed_resolution);
  memcpy (&packed->data[body_offset],
          &(guint32){ GUINT32_TO_LE (packed->len - body_offset - 4) }, 4);
}

static void
packed_append_relation (GByteArray                  *packed,
                        guint                        index,
                        const GoodixChicagoRelation *relation)
{
  const guint8 tag = 0x96;

  g_byte_array_append (packed, &tag, 1);
  packed_append_u32 (packed, 40);
  packed_append_scalar (packed, 0xe3, index);
  packed_append_scalar (packed, 0xe1, relation->inlier_count);
  for (guint value = 0; value < G_N_ELEMENTS (relation->transform); value++)
    packed_append_scalar (packed, 0xe4 + value, relation->transform[value]);
}

static void
packed_append_matcher_state (GByteArray                    *packed,
                             const GoodixChicagoEnrollment *self)
{
  static const guint8 version[64] = "openchicago_v1.0";
  const guint8 tag = 0x94;
  guint8 zeroes[1024] = { 0, };

  g_byte_array_append (packed, &tag, 1);
  packed_append_u32 (packed, 1328);
  g_byte_array_append (packed, (const guint8[]){ 0xa1 }, 1);
  packed_append_u32 (packed, 200);
  for (guint index = 0; index < G_N_ELEMENTS (self->match_order); index++)
    packed_append_u32 (packed, self->match_order[index]);
  packed_append_scalar (packed, 0xa2, self->matcher_active_index);
  packed_append_blob (packed, 0xa3, version, sizeof (version));
  packed_append_blob (packed, 0xa4, zeroes, sizeof (zeroes));
  packed_append_scalar (packed, 0xa5, self->matcher_value_a);
  packed_append_scalar (packed, 0xa6, self->matcher_value_b);
  packed_append_scalar (packed, 0xa7, self->matcher_value_c);
  packed_append_scalar (packed, 0xa8, self->matcher_value_d);
}

static guint32
packed_crc32 (const guint8 *data,
              gsize         size)
{
  guint32 crc = G_MAXUINT32;

  for (gsize offset = 0; offset < size; offset++)
    {
      crc ^= data[offset];
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
  return ~crc;
}

typedef struct
{
  const guint8 *data;
  gsize         size;
  gsize         offset;
} PackedCursor;

static gboolean
packed_cursor_take (PackedCursor  *cursor,
                    gsize          size,
                    const guint8 **value)
{
  if (cursor->offset > cursor->size ||
      size > cursor->size - cursor->offset)
    return FALSE;
  if (value != NULL)
    *value = cursor->data + cursor->offset;
  cursor->offset += size;
  return TRUE;
}

static gboolean
packed_cursor_u32 (PackedCursor *cursor,
                   guint32      *value)
{
  const guint8 *bytes;
  guint32 little_endian;

  if (!packed_cursor_take (cursor, sizeof (little_endian), &bytes))
    return FALSE;
  memcpy (&little_endian, bytes, sizeof (little_endian));
  *value = GUINT32_FROM_LE (little_endian);
  return TRUE;
}

static gboolean
packed_cursor_scalar (PackedCursor *cursor,
                      guint8        expected_tag,
                      guint32      *value)
{
  const guint8 *tag;

  return packed_cursor_take (cursor, 1, &tag) && *tag == expected_tag &&
         packed_cursor_u32 (cursor, value);
}

static gboolean
packed_cursor_blob (PackedCursor  *cursor,
                    guint8         expected_tag,
                    const guint8 **value,
                    guint32       *size)
{
  const guint8 *tag;

  return packed_cursor_take (cursor, 1, &tag) && *tag == expected_tag &&
         packed_cursor_u32 (cursor, size) &&
         packed_cursor_take (cursor, *size, value);
}

static gboolean
packed_cursor_container (PackedCursor *cursor,
                         guint8        expected_tag,
                         PackedCursor *body)
{
  const guint8 *value;
  guint32 size;

  if (!packed_cursor_blob (cursor, expected_tag, &value, &size))
    return FALSE;
  *body = (PackedCursor){ value, size, 0 };
  return TRUE;
}

static gboolean
packed_cursor_map (PackedCursor *cursor,
                   guint8        expected_tag,
                   guint8        output[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES])
{
  PackedCursor body;
  const guint8 *bits;
  guint32 width;
  guint32 height;
  guint32 sentinel;
  guint32 depth;
  guint32 size;

  if (!packed_cursor_container (cursor, expected_tag, &body) ||
      !packed_cursor_scalar (&body, 0xc1, &width) || width != 40 ||
      !packed_cursor_scalar (&body, 0xc2, &height) || height != 32 ||
      !packed_cursor_scalar (&body, 0xc3, &sentinel) ||
      sentinel != G_MAXUINT32 ||
      !packed_cursor_scalar (&body, 0xc4, &depth) || depth != 8 ||
      !packed_cursor_blob (&body, 0xc5, &bits, &size) ||
      size != GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES ||
      body.offset != body.size)
    return FALSE;
  memcpy (output, bits, size);
  return TRUE;
}

static void
packed_decode_record (const guint8                packed[32],
                      GoodixChicagoFeatureRecord *record)
{
  static const guint8 swap[8] = { 0, 1, 1, 0, 1, 0, 0, 1 };
  guint32 coordinate;
  guint8 orientation;

  memset (record, 0, sizeof (*record));
  memcpy (&coordinate, packed, sizeof (coordinate));
  coordinate = GUINT32_FROM_LE (coordinate);
  record->refined_x = (gint16) ((coordinate >> 16) & 0xfff0);
  record->refined_y = (gint16) ((coordinate >> 4) & 0xfff0);
  orientation = coordinate & 0xff;
  record->orientation = orientation < 0x80 ?
                        (gint16) orientation << 8 :
                        (gint16) - (((gint) orientation - 0x80) * 0x100);
  {
    /* The dword +0xc is the boundary class that feature extraction stores (see
     * goodix-chicago-feature.c: 1 near the top, 2 near the bottom, else 0);
     * unpacked gallery records get the same class computed from the unpacked y.
     * The matcher's pass-2 cross-class pairing depends on it. */
    guint32 boundary;

    if ((guint16) record->refined_y < 0x1400)
      boundary = 1;
    else if ((guint16) record->refined_y >=
             (guint16) (GOODIX_CHICAGO_FEATURE_HEIGHT * 0x100 - 0x1400))
      boundary = 2;
    else
      boundary = 0;
    memcpy (record->descriptor, &boundary, sizeof (boundary));
  }
  for (guint index = 0; index < 8; index++)
    {
      guint8 first = packed[4 + index * 2];
      guint8 second = packed[5 + index * 2];

      if (swap[index])
        {
          const guint8 temporary = first;

          first = second;
          second = temporary;
        }
      record->descriptor[4 + index] =
        (second & 0xf0u) | (first & 0x0fu);
      record->descriptor[12 + index] =
        (first & 0xf0u) | (second & 0x0fu);
    }
  memcpy (record->descriptor + 20, packed + 20, 4);
  memcpy (record->descriptor + 28, packed + 24, 8);
}

GoodixChicagoEnrollment *
goodix_chicago_enrollment_unpack (const guint8 *data,
                                  gsize         data_len,
                                  GError      **error)
{
  GoodixChicagoEnrollment *self = NULL;
  PackedCursor cursor;
  guint32 stored_crc;
  guint32 declared_size;
  guint32 value;
  guint32 subtemplate_count;
  guint32 relation_count;
  PackedCursor scheduler_state;
  PackedCursor matcher_state;
  const guint8 *matcher_order;
  const guint8 *ignored_blob;
  guint32 matcher_order_size;
  guint32 ignored_size;

  g_return_val_if_fail (data != NULL, NULL);
  if (data_len < 10)
    goto invalid;
  memcpy (&stored_crc, data + 1, sizeof (stored_crc));
  stored_crc = GUINT32_FROM_LE (stored_crc);
  memcpy (&declared_size, data + 6, sizeof (declared_size));
  declared_size = GUINT32_FROM_LE (declared_size);
  if (data[0] != 0x87 || data[5] != 0x86 ||
      declared_size != data_len - 10 ||
      stored_crc != packed_crc32 (data + 10, data_len - 10))
    goto invalid;

  cursor = (PackedCursor){ data + 10, data_len - 10, 0 };
  if (!packed_cursor_scalar (&cursor, 0x81, &value) || value != 0x002e14ef ||
      !packed_cursor_scalar (&cursor, 0x88, &value) || value != 0x002e14ef ||
      !packed_cursor_scalar (&cursor, 0x89, &value) || value != 0x002df160 ||
      !packed_cursor_scalar (&cursor, 0x98, &value) || value != 24 ||
      !packed_cursor_scalar (&cursor, 0x9a, &value) || value != 64 ||
      !packed_cursor_scalar (&cursor, 0x9b, &value) || value != 80 ||
      !packed_cursor_scalar (&cursor, 0x91, &subtemplate_count) ||
      !packed_cursor_scalar (&cursor, 0x97, &value))
    goto invalid;
  self = goodix_chicago_enrollment_new ();
  self->capacity = value;
  if (!packed_cursor_scalar (&cursor, 0x92, &relation_count) ||
      !packed_cursor_scalar (&cursor, 0x9e, &value))
    goto invalid;
  self->record_limit = value;
  if (!packed_cursor_scalar (&cursor, 0x9f, &value) ||
      value != self->record_limit ||
      !packed_cursor_scalar (&cursor, 0x9c, &value) || value != 1 ||
      !packed_cursor_scalar (&cursor, 0x9d, &value) || value != 1 ||
      !packed_cursor_scalar (&cursor, 0xfa, &value) || value != 0 ||
      !packed_cursor_scalar (&cursor, 0xfb, &value) || value != 0 ||
      subtemplate_count > self->capacity ||
      self->capacity > GOODIX_CHICAGO_ENROLLMENT_CAPACITY ||
      self->record_limit > GOODIX_CHICAGO_SUBTEMPLATE_RECORD_LIMIT ||
      relation_count > subtemplate_count * (subtemplate_count + 1) / 2)
    goto invalid;

  for (guint index = 0; index < subtemplate_count; index++)
    {
      GoodixChicagoSubtemplate *subtemplate = g_new0 (
        GoodixChicagoSubtemplate, 1);
      PackedCursor body;
      const guint8 *coarse;
      const guint8 *records;
      guint32 coarse_size;
      guint32 records_size;
      guint32 record_count;
      guint32 serialized_index;

      if (!packed_cursor_container (&cursor, 0x95, &body) ||
          !packed_cursor_map (&body, 0xb2,
                              subtemplate->metric_data.primary) ||
          !packed_cursor_map (&body, 0xcf,
                              subtemplate->metric_data.secondary) ||
          !packed_cursor_blob (&body, 0xce, &coarse, &coarse_size) ||
          coarse_size != GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES ||
          !packed_cursor_map (&body, 0xcd,
                              subtemplate->metric_data.validity) ||
          !packed_cursor_scalar (&body, 0xb3, &record_count) ||
          record_count > self->record_limit ||
          !packed_cursor_blob (&body, 0xb4, &records, &records_size) ||
          records_size != record_count * 32)
        {
          subtemplate_free (subtemplate);
          goto invalid;
        }
      memcpy (subtemplate->metric_data.coarse_mask, coarse, coarse_size);
      metric_data_build_position_map (&subtemplate->metric_data);
      subtemplate->record_count = record_count;
      subtemplate->records = g_new0 (GoodixChicagoFeatureRecord,
                                     MAX (record_count, 1));
      for (guint record = 0; record < record_count; record++)
        packed_decode_record (records + record * 32,
                              &subtemplate->records[record]);
      if (!packed_cursor_scalar (&body, 0xb5, &value))
        goto invalid_subtemplate;
      subtemplate->group_state = value;
      if (!packed_cursor_scalar (&body, 0xb6, &value))
        goto invalid_subtemplate;
      subtemplate->relation_base = value;
      if (!packed_cursor_scalar (&body, 0xb7, &value) ||
          value > record_count)
        goto invalid_subtemplate;
      subtemplate->active_count = value;
      for (guint record = subtemplate->active_count;
           record < record_count;
           record++)
        subtemplate->records[record].foreground = 1;
      if (!packed_cursor_scalar (&body, 0xb8, &value))
        goto invalid_subtemplate;
      subtemplate->quality = value;
      if (!packed_cursor_scalar (&body, 0xb9, &value))
        goto invalid_subtemplate;
      subtemplate->coverage = value;
      if (!packed_cursor_scalar (&body, 0xba, &value))
        goto invalid_subtemplate;
      subtemplate->study_state = value;
      if (!packed_cursor_scalar (&body, 0xbb, &value))
        goto invalid_subtemplate;
      subtemplate->study_flags = value;
      if (!packed_cursor_scalar (&body, 0xbc, &serialized_index) ||
          serialized_index >= self->capacity)
        goto invalid_subtemplate;
      subtemplate->lineage_index = serialized_index;
      if (!packed_cursor_scalar (&body, 0xbd, &value))
        goto invalid_subtemplate;
      subtemplate->replacement_count = value;
      if (!packed_cursor_scalar (&body, 0xbe, &value))
        goto invalid_subtemplate;
      subtemplate->study_value_a = value;
      if (!packed_cursor_scalar (&body, 0xc0, &value))
        goto invalid_subtemplate;
      subtemplate->study_value_b = value;
      if (body.offset < body.size && body.data[body.offset] == 0xc7)
        {
          if (!packed_cursor_scalar (&body, 0xc7, &value))
            goto invalid_subtemplate;
          subtemplate->metric_data.packed_resolution = value;
        }
      if (body.offset != body.size)
        goto invalid_subtemplate;
      subtemplate->has_metric_data = TRUE;
      g_ptr_array_add (self->subtemplates, subtemplate);
      continue;

invalid_subtemplate:
      subtemplate_free (subtemplate);
      goto invalid;
    }

  /* The relations of subtemplate i with the older ones are
   * relation_base + [0, i); all of them must exist. */
  for (guint index = 0; index < self->subtemplates->len; index++)
    {
      const GoodixChicagoSubtemplate *subtemplate =
        g_ptr_array_index (self->subtemplates, index);

      if (subtemplate->relation_base > relation_count ||
          relation_count - subtemplate->relation_base < index)
        goto invalid;
    }
  g_array_set_size (self->relations, relation_count);
  for (guint index = 0; index < relation_count; index++)
    {
      GoodixChicagoRelation *relation = &g_array_index (
        self->relations, GoodixChicagoRelation, index);

      memset (relation, 0, sizeof (*relation));
      relation->inlier_count = -1;
      if (index == 0)
        identity_group_transform (relation->transform);
    }
  while (cursor.offset < cursor.size && cursor.data[cursor.offset] == 0x96)
    {
      PackedCursor body;
      GoodixChicagoRelation relation;
      guint32 relation_index;

      memset (&relation, 0, sizeof (relation));
      if (!packed_cursor_container (&cursor, 0x96, &body) ||
          !packed_cursor_scalar (&body, 0xe3, &relation_index) ||
          relation_index >= relation_count ||
          !packed_cursor_scalar (&body, 0xe1, &value))
        goto invalid;
      relation.inlier_count = (gint32) value;
      for (guint coefficient = 0; coefficient < 6; coefficient++)
        {
          if (!packed_cursor_scalar (&body, 0xe4 + coefficient, &value))
            goto invalid;
          relation.transform[coefficient] = (gint32) value;
        }
      if (body.offset != body.size)
        goto invalid;
      g_array_index (self->relations, GoodixChicagoRelation,
                     relation_index) = relation;
    }
  self->transform_count = relation_count;
  if (!packed_cursor_container (&cursor, 0x93, &scheduler_state) ||
      scheduler_state.size != 20)
    goto invalid;
  /* The 0x93 state carries the group anchor and the group flag. */
  {
    guint32 anchor, unused, flag;

    if (!packed_cursor_scalar (&scheduler_state, 0xf2, &anchor) ||
        !packed_cursor_scalar (&scheduler_state, 0xf3, &unused) ||
        !packed_cursor_scalar (&scheduler_state, 0xf4, &unused) ||
        !packed_cursor_scalar (&scheduler_state, 0xf5, &flag))
      goto invalid;
    self->has_group_anchor = flag == 1 && anchor < subtemplate_count;
    self->group_anchor = self->has_group_anchor ? anchor : 0;
  }
  if (!packed_cursor_container (&cursor, 0x94, &matcher_state) ||
      matcher_state.size != 1328 ||
      cursor.offset != cursor.size)
    goto invalid;
  if (!packed_cursor_blob (&matcher_state, 0xa1, &matcher_order,
                           &matcher_order_size) ||
      matcher_order_size != sizeof (self->match_order))
    goto invalid;
  for (guint schedule_index = 0; schedule_index < self->capacity;
       schedule_index++)
    {
      guint32 little_endian;

      memcpy (&little_endian, matcher_order + schedule_index * 4, 4);
      self->match_order[schedule_index] = GUINT32_FROM_LE (little_endian);
    }
  for (guint schedule_index = 0; schedule_index < subtemplate_count;
       schedule_index++)
    {
      const guint32 gallery_index = self->match_order[schedule_index];

      if (gallery_index >= subtemplate_count)
        goto invalid;
      for (guint previous = 0; previous < schedule_index; previous++)
        if (self->match_order[previous] == gallery_index)
          goto invalid;
    }
  for (guint schedule_index = subtemplate_count;
       schedule_index < self->capacity; schedule_index++)
    if (self->match_order[schedule_index] != G_MAXUINT32)
      goto invalid;
  if (!packed_cursor_scalar (&matcher_state, 0xa2,
                             &self->matcher_active_index) ||
      !packed_cursor_blob (&matcher_state, 0xa3, &ignored_blob,
                           &ignored_size) || ignored_size != 64 ||
      !packed_cursor_blob (&matcher_state, 0xa4, &ignored_blob,
                           &ignored_size) || ignored_size != 1024 ||
      !packed_cursor_scalar (&matcher_state, 0xa5,
                             &self->matcher_value_a) ||
      !packed_cursor_scalar (&matcher_state, 0xa6,
                             &self->matcher_value_b) ||
      !packed_cursor_scalar (&matcher_state, 0xa7,
                             &self->matcher_value_c) ||
      !packed_cursor_scalar (&matcher_state, 0xa8,
                             &self->matcher_value_d) ||
      matcher_state.offset != matcher_state.size)
    goto invalid;
  /* Unpacking runs the gallery-only closure for
   * type 24: synthetic strength-2 relations and group propagation in the live template. */
  goodix_chicago_enrollment_synthesize_capacity_relations (self, NULL, self->subtemplates->len);
  return self;

invalid:
  goodix_chicago_enrollment_free (self);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "gdix51c0: invalid packed Chicago template");
  return NULL;
}

GBytes *
goodix_chicago_enrollment_pack (const GoodixChicagoEnrollment *self,
                                GError                       **error)
{
  g_autoptr(GByteArray) packed = NULL;
  g_autofree guint8 *relation_mask = NULL;
  gsize expected_size;
  guint32 crc;

  g_return_val_if_fail (self != NULL, NULL);
  expected_size = goodix_chicago_enrollment_get_packed_size (self);
  packed = g_byte_array_sized_new (expected_size);
  for (guint index = 0; index < self->subtemplates->len; index++)
    {
      const GoodixChicagoSubtemplate *subtemplate =
        g_ptr_array_index (self->subtemplates, index);

      if (!subtemplate->has_metric_data)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "gdix51c0: cannot pack a subtemplate without metric data");
          return NULL;
        }
    }

  g_byte_array_append (packed, (const guint8[]){ 0x87, 0, 0, 0, 0, 0x86 }, 6);
  packed_append_u32 (packed, 0);
  packed_append_scalar (packed, 0x81, 0x002e14ef);
  packed_append_scalar (packed, 0x88, 0x002e14ef);
  packed_append_scalar (packed, 0x89, 0x002df160);
  packed_append_scalar (packed, 0x98, 24);
  packed_append_scalar (packed, 0x9a, 64);
  packed_append_scalar (packed, 0x9b, 80);
  packed_append_scalar (packed, 0x91, self->subtemplates->len);
  packed_append_scalar (packed, 0x97, self->capacity);
  packed_append_scalar (packed, 0x92, self->relations->len);
  packed_append_scalar (packed, 0x9e, self->record_limit);
  packed_append_scalar (packed, 0x9f, self->record_limit);
  packed_append_scalar (packed, 0x9c, 1);
  packed_append_scalar (packed, 0x9d, 1);
  packed_append_scalar (packed, 0xfa, 0);
  packed_append_scalar (packed, 0xfb, 0);

  for (guint index = 0; index < self->subtemplates->len; index++)
    packed_append_subtemplate (packed,
                               g_ptr_array_index (self->subtemplates, index));
  relation_mask = g_new0 (guint8, self->relations->len);
  build_serialized_relation_mask (self, relation_mask);
  for (guint relation_index = 0;
       relation_index < self->relations->len;
       relation_index++)
    if (relation_mask[relation_index] != 0)
      {
        packed_append_relation (
          packed, relation_index,
          &g_array_index (self->relations, GoodixChicagoRelation,
                          relation_index));
      }

  g_byte_array_append (packed, (const guint8[]){ 0x93 }, 1);
  packed_append_u32 (packed, 20);
  /* Writes the group anchor and the group flag. */
  packed_append_scalar (packed, 0xf2, self->has_group_anchor ? self->group_anchor : G_MAXUINT32);
  packed_append_scalar (packed, 0xf3, G_MAXUINT32);
  packed_append_scalar (packed, 0xf4, G_MAXUINT32);
  packed_append_scalar (packed, 0xf5, self->has_group_anchor ? 1 : 0);
  packed_append_matcher_state (packed, self);

  if (packed->len != expected_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: packed template size mismatch %u/%" G_GSIZE_FORMAT,
                   packed->len, expected_size);
      return NULL;
    }
  memcpy (&packed->data[6],
          &(guint32){ GUINT32_TO_LE (packed->len - 10) }, 4);
  crc = GUINT32_TO_LE (packed_crc32 (packed->data + 10, packed->len - 10));
  memcpy (&packed->data[1], &crc, 4);
  return g_byte_array_free_to_bytes (g_steal_pointer (&packed));
}

gboolean
goodix_chicago_enrollment_get_subtemplate (
  const GoodixChicagoEnrollment *self,
  guint                          index,
  GoodixChicagoSubtemplateView  *view)
{
  GoodixChicagoSubtemplate *subtemplate;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (view != NULL, FALSE);
  memset (view, 0, sizeof (*view));
  if (index >= self->subtemplates->len)
    return FALSE;

  subtemplate = g_ptr_array_index (self->subtemplates, index);
  view->record_count = subtemplate->record_count;
  view->active_count = subtemplate->active_count;
  view->quality = subtemplate->quality;
  view->coverage = subtemplate->coverage;
  view->records = subtemplate->records;
  view->has_metric_data = subtemplate->has_metric_data;
  view->metric_data = subtemplate->has_metric_data ?
                      &subtemplate->metric_data : NULL;
  view->group_state = subtemplate->group_state;
  view->relation_base = subtemplate->relation_base;
  view->study_state = subtemplate->study_state;
  view->study_flags = subtemplate->study_flags;
  view->lineage_index = subtemplate->lineage_index;
  view->replacement_count = subtemplate->replacement_count;
  view->study_value_a = subtemplate->study_value_a;
  view->study_value_b = subtemplate->study_value_b;
  return TRUE;
}
