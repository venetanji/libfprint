// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: identify decision, geometry and study metrics;
 * debug trace and unused code removed. */

#include <string.h>

#include "goodix-chicago-match.h"

#include <stdbool.h>

G_STATIC_ASSERT (sizeof (GoodixChicagoMatchScoreConfig) == 0x58);
G_STATIC_ASSERT (sizeof (GoodixChicagoMatchFeatureOverlap) == 0x2c);

static const guint16 match_cordic_angles[13] = {
  0x0c91, 0x076b, 0x03eb, 0x01fd, 0x0100, 0x0080, 0x0040,
  0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001,
};

static const gint32 match_cordic_gains[13] = {
  0xb505, 0xa1e9, 0x9d13, 0x9bdd, 0x9b8f, 0x9b7b, 0x9b77,
  0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75,
};

static guint hamming_words (const guint8 *a,
                            const guint8 *b,
                            guint         words);

static gint32
match_divide_nearest (gint32 numerator,
                      gint32 denominator)
{
  g_return_val_if_fail (denominator > 0, 0);
  return (numerator + denominator / 2) / denominator;
}

gboolean
goodix_chicago_match_accept_type24 (gint32 selector,
                                    gint32 agreement,
                                    gint32 helper_status,
                                    gint32 selector_threshold)
{
  return helper_status == 1 ||
         (selector > selector_threshold && agreement > 195);
}

void
goodix_chicago_match_scheduler_evidence_type24 (
  const GoodixChicagoMatchScoreRecord *record,
  gint32                               quality_a,
  gint32                               quality_b,
  gboolean                             penalize_low_coverage,
  GoodixChicagoMatchSchedulerEvidence *evidence)
{
  static const gint32 agreement_by_primary[8] = {
    0x0fffffff, 206, 201, 196, 185, 185, 185, 185,
  };
  static const gint32 agreement_by_secondary[8] = {
    0x0fffffff, 0x0fffffff, 0x0fffffff, 207, 205, 203, 196, 187,
  };
  static const gint32 confidence_by_secondary[8] = {
    0x0fffffff, 0x0fffffff, 209, 208, 208, 207, 207, 207,
  };
  gint32 primary;
  gint32 secondary;
  gint32 agreement;
  gint32 adjusted_agreement;
  gint32 primary_index;
  gint32 secondary_index;
  gint32 combined_index;
  gint32 primary_limit;
  gint32 secondary_limit;
  gboolean confident = FALSE;

  g_return_if_fail (record != NULL);
  g_return_if_fail (evidence != NULL);
  memset (evidence, 0, sizeof (*evidence));
  primary = record->geometry_count;
  secondary = record->secondary_geometry_count;
  agreement = record->agreement;
  adjusted_agreement = agreement;
  if (primary <= 4)
    agreement -= 4;
  if (record->matched_percent > 60 && primary > 4)
    {
      const gint32 bonus = 1 + (record->matched_percent - 60) / 5;

      primary += bonus;
      secondary += bonus;
    }
  if (penalize_low_coverage && record->normalized_coverage < 128)
    {
      const gint32 penalty =
        1 + (128 - record->normalized_coverage) / 10;

      primary -= penalty;
      secondary -= penalty;
    }
  primary_index = CLAMP (secondary - 7, 0, 7);
  secondary_index = CLAMP (primary - 7, 0, 7);
  combined_index = CLAMP (secondary - 7, 0, 7);
  if (secondary <= 10)
    {
      const gint32 flag_penalty =
        4 * (record->penalty_flag_a + record->penalty_flag_b);

      adjusted_agreement -= flag_penalty;
      agreement -= flag_penalty;
    }
  primary_limit = agreement_by_primary[secondary_index];
  secondary_limit = agreement_by_secondary[combined_index];
  evidence->status = adjusted_agreement > primary_limit ||
                     adjusted_agreement > secondary_limit || primary >= 14;
  if (evidence->status && quality_a > 15 && quality_b >= 65)
    {
      if (primary <= 4 && record->selector < 235 && agreement < 217)
        confident = FALSE;
      else if (agreement > confidence_by_secondary[primary_index] ||
               (agreement >= 195 && primary >= 16) ||
               (agreement >= 190 && primary >= 18) ||
               (secondary > 18 && primary > 10 && agreement > 196) ||
               (record->matched_percent > 60 && adjusted_agreement >= 198 &&
                primary > 12) ||
               (secondary > 16 && primary > 10 &&
                adjusted_agreement > 200))
        confident = TRUE;
    }
  evidence->confidence = confident;
  evidence->special = !((primary > 20 && adjusted_agreement >= 185) ||
                        adjusted_agreement >= 205 ||
                        adjusted_agreement >= primary_limit + 10 ||
                        adjusted_agreement >= secondary_limit + 10);
  if (evidence->confidence == 1 && record->geometry_count > 7 &&
      record->secondary_geometry_count > 11 &&
      record->geometry_percent > 35)
    evidence->confidence++;
}

static gint32
match_transform_coordinate (gint64 value)
{
  if (value > 0)
    return (gint32) ((value + 0x80) >> 8);
  return (gint32) - ((0x80 - value) >> 8);
}

void
goodix_chicago_match_feature_overlap_type24 (
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  guint                             gallery_width,
  guint                             gallery_height,
  const gint32                      transform[6],
  gint32                            geometry_count,
  GoodixChicagoMatchFeatureOverlap *result)
{
  g_return_if_fail (gallery_count == 0 || gallery_records != NULL);
  g_return_if_fail (probe_count == 0 || probe_records != NULL);
  g_return_if_fail (transform != NULL);
  g_return_if_fail (result != NULL);
  memset (result, 0, sizeof (*result));
  for (guint probe_index = 0; probe_index < probe_count; probe_index++)
    {
      const GoodixChicagoFeatureRecord *probe = &probe_records[probe_index];
      const gint32 probe_x = (guint16) probe->refined_x;
      const gint32 probe_y = (guint16) probe->refined_y;
      const gint32 transformed_x = match_transform_coordinate (
        (gint64) transform[0] * probe_x +
        (gint64) transform[1] * probe_y +
        (gint64) transform[2] * 0x100);
      const gint32 transformed_y = match_transform_coordinate (
        (gint64) transform[3] * probe_x +
        (gint64) transform[4] * probe_y +
        (gint64) transform[5] * 0x100);
      gint best_index = -1;
      gint32 best_squared = G_MAXINT32;

      if (transformed_x < 0x600 ||
          transformed_x >= ((gint32) gallery_width - 7) * 0x100 ||
          transformed_y < 0x600 ||
          transformed_y >= ((gint32) gallery_height - 7) * 0x100)
        continue;
      result->eligible_count++;
      for (guint gallery_index = 0; gallery_index < gallery_count;
           gallery_index++)
        {
          const GoodixChicagoFeatureRecord *gallery =
            &gallery_records[gallery_index];
          const gint32 dx = transformed_x - (guint16) gallery->refined_x;
          const gint32 dy = transformed_y - (guint16) gallery->refined_y;
          gint32 squared;

          if ((gallery->foreground & 3) != (probe->foreground & 3) ||
              ABS (dx) > 0x200 || ABS (dy) > 0x200)
            continue;
          squared = dx * dx + dy * dy;
          if (squared < best_squared)
            {
              best_squared = squared;
              best_index = gallery_index;
            }
        }
      if (best_index >= 0 && best_squared < 0x40000)
        {
          const guint8 *gallery =
            (const guint8 *) &gallery_records[best_index];
          const guint8 *probe_bytes = (const guint8 *) probe;
          const gint32 straight = hamming_words (gallery + 0x28,
                                                 probe_bytes + 0x28, 2);
          const gint32 shifted = hamming_words (gallery + 0x28,
                                                probe_bytes + 0x30, 2);

          result->matched_count++;
          result->mean_distance += MIN (straight, shifted);
        }
    }
  if (result->matched_count > 0)
    result->mean_distance /= result->matched_count;
  if (result->eligible_count > 0)
    {
      result->matched_percent =
        result->matched_count * 100 / result->eligible_count;
      result->geometry_percent =
        geometry_count * 100 / result->eligible_count;
    }
}

static guint32
match_integer_sqrt (guint32 value)
{
  guint32 remainder = value;
  guint32 root = 0;
  guint32 bit = 0x8000;
  gint shift = 15;

  if (value <= 1)
    return value;
  while (bit != 0)
    {
      const guint32 trial = (bit + root * 2) << shift;

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

static guint16
match_cordic_orientation (gint32  vertical,
                          gint32 *horizontal)
{
  const gint32 original_horizontal = *horizontal;
  const gint32 original_vertical = vertical;
  gint32 x = ABS (original_horizontal);
  gint32 y = ABS (original_vertical);
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
          angle = (guint16) (angle + match_cordic_angles[iteration]);
        }
      else
        {
          x -= y_shift;
          y += x_shift;
          angle = (guint16) (angle - match_cordic_angles[iteration]);
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
  *horizontal = (gint32) (((gint64) match_cordic_gains[stop] * x +
                           0x8000) >> 16);
  return angle;
}

static gint32
circular_orientation_distance (gint32 value)
{
  if (value < 0)
    value += 0x6488;
  if (value > 0x6488)
    value -= 0x6488;
  return MIN (value, 0x6488 - value);
}

guint
goodix_chicago_match_filter_orientations (
  const gint32  transform[6],
  const gint32 *target_orientations,
  const gint32 *source_orientations,
  guint         point_count,
  guint8        inliers[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT])
{
  guint32 first_scale;
  guint32 second_scale;
  gint32 scale;
  gint32 horizontal;
  gint32 vertical;
  gint32 rotation;
  guint count = 0;

  g_return_val_if_fail (transform != NULL, 0);
  g_return_val_if_fail (point_count == 0 || target_orientations != NULL, 0);
  g_return_val_if_fail (point_count == 0 || source_orientations != NULL, 0);
  g_return_val_if_fail (point_count <= GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT,
                        0);
  g_return_val_if_fail (inliers != NULL, 0);
  first_scale = match_integer_sqrt (
    (guint32) ((gint64) transform[0] * transform[0] +
               (gint64) transform[3] * transform[3]));
  second_scale = match_integer_sqrt (
    (guint32) ((gint64) transform[1] * transform[1] +
               (gint64) transform[4] * transform[4]));
  scale = (gint32) ((first_scale + second_scale) / 2);
  if (scale == 0)
    goto count_survivors;
  horizontal = ((transform[4] + transform[0]) / 2 * 256) / scale;
  vertical = ((transform[3] - transform[1]) / 2 * 256) / scale;
  rotation = (gint16) match_cordic_orientation (vertical, &horizontal);
  if (rotation < 0)
    rotation += 0x6488;
  for (guint index = 0; index < point_count; index++)
    {
      gint32 delta;
      gint32 distance;

      if (!inliers[index])
        continue;
      delta = target_orientations[index] - source_orientations[index] +
              rotation;
      distance = MIN (circular_orientation_distance (delta),
                      circular_orientation_distance (delta + 0x3244));
      if (distance > 0x506)
        inliers[index] = 0;
    }

count_survivors:
  for (guint index = 0; index < point_count; index++)
    count += inliers[index] != 0;
  return count;
}

static gint32
divide_nearest (gint64 numerator,
                gint64 denominator)
{
  const gint64 half = denominator >> 1;

  if (numerator >= 0)
    return (gint32) ((numerator + half) / denominator);
  return (gint32) - ((half - numerator) / denominator);
}

gboolean
goodix_chicago_match_refine_geometry (
  const GoodixChicagoMatchPoint *source_points,
  const GoodixChicagoMatchPoint *target_points,
  const guint8                  *inliers,
  guint                          point_count,
  gint32                         error_limit,
  gint32                         transform[6])
{
  gint64 design[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT][3] = { { 0, } };
  gint64 target[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT][2] = { { 0, } };
  gint64 gram[3][3] = { { 0, } };
  gint64 cross[3][2] = { { 0, } };
  gint64 cofactor[3][3];
  gint32 coefficients[3][2];
  gint32 candidate[6];
  guint active_count = 0;
  guint candidate_inliers = 0;
  guint64 residual_sum = 0;
  guint64 average_error;
  gint64 determinant;

  g_return_val_if_fail (point_count == 0 || source_points != NULL, FALSE);
  g_return_val_if_fail (point_count == 0 || target_points != NULL, FALSE);
  g_return_val_if_fail (point_count == 0 || inliers != NULL, FALSE);
  g_return_val_if_fail (point_count <= GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT,
                        FALSE);
  g_return_val_if_fail (transform != NULL, FALSE);
  for (guint index = 0; index < point_count; index++)
    {
      if (!inliers[index])
        continue;
      design[active_count][0] = source_points[index].x;
      design[active_count][1] = source_points[index].y;
      design[active_count][2] = 0x100;
      target[active_count][0] = target_points[index].x;
      target[active_count][1] = target_points[index].y;
      active_count++;
    }
  for (guint row = 0; row < 3; row++)
    for (guint column = 0; column < 3; column++)
      {
        gint64 sum = 0;

        for (guint index = 0; index < active_count; index++)
          sum += design[index][row] * design[index][column];
        gram[row][column] = (sum + 0x80) >> 8;
      }
  for (guint row = 0; row < 3; row++)
    for (guint column = 0; column < 2; column++)
      {
        gint64 sum = 0;

        for (guint index = 0; index < active_count; index++)
          sum += design[index][row] * target[index][column];
        cross[row][column] = (sum + 0x80) >> 8;
      }
  cofactor[0][0] = gram[1][1] * gram[2][2] -
                   gram[1][2] * gram[2][1];
  cofactor[0][1] = gram[1][2] * gram[2][0] -
                   gram[1][0] * gram[2][2];
  cofactor[0][2] = gram[1][0] * gram[2][1] -
                   gram[1][1] * gram[2][0];
  cofactor[1][0] = gram[0][2] * gram[2][1] -
                   gram[0][1] * gram[2][2];
  cofactor[1][1] = gram[0][0] * gram[2][2] -
                   gram[0][2] * gram[2][0];
  cofactor[1][2] = gram[0][1] * gram[2][0] -
                   gram[0][0] * gram[2][1];
  cofactor[2][0] = gram[0][1] * gram[1][2] -
                   gram[0][2] * gram[1][1];
  cofactor[2][1] = gram[0][2] * gram[1][0] -
                   gram[0][0] * gram[1][2];
  cofactor[2][2] = gram[0][0] * gram[1][1] -
                   gram[0][1] * gram[1][0];
  determinant = (gram[0][0] * cofactor[0][0] +
                 gram[0][1] * cofactor[0][1] +
                 gram[0][2] * cofactor[0][2] + 0x80) >> 8;
  if (determinant == 0)
    return FALSE;
  for (guint coefficient = 0; coefficient < 3; coefficient++)
    for (guint axis = 0; axis < 2; axis++)
      {
        gint64 numerator = 0;

        for (guint row = 0; row < 3; row++)
          numerator = (gint64) ((guint64) numerator +
                                (guint64) cofactor[row][coefficient] *
                                (guint64) cross[row][axis]);
        coefficients[coefficient][axis] =
          divide_nearest (numerator, determinant);
      }
  candidate[0] = coefficients[0][0];
  candidate[1] = coefficients[1][0];
  candidate[2] = coefficients[2][0];
  candidate[3] = coefficients[0][1];
  candidate[4] = coefficients[1][1];
  candidate[5] = coefficients[2][1];
  for (guint index = 0; index < point_count; index++)
    {
      const gint32 predicted_x =
        (gint32) ((((gint64) candidate[0] * source_points[index].x +
                    (gint64) candidate[1] * source_points[index].y +
                    0x80) >> 8) + candidate[2]);
      const gint32 predicted_y =
        (gint32) ((((gint64) candidate[3] * source_points[index].x +
                    (gint64) candidate[4] * source_points[index].y +
                    0x80) >> 8) + candidate[5]);
      const gint64 dx = (gint64) predicted_x - target_points[index].x;
      const gint64 dy = (gint64) predicted_y - target_points[index].y;
      const guint64 squared = (guint64) (dx * dx + dy * dy);

      if (squared < 0x64000)
        {
          candidate_inliers++;
          residual_sum += squared;
        }
    }
  average_error = candidate_inliers == 0 ? 0x190000 :
                  (residual_sum + (candidate_inliers >> 1)) / candidate_inliers;
  if (average_error >= (guint64) (gint64) error_limit ||
      candidate_inliers < active_count)
    return FALSE;
  memcpy (transform, candidate, sizeof (candidate));
  return TRUE;
}

guint
goodix_chicago_match_geometry_consensus (
  const GoodixChicagoMatchPoint *source_points,
  const GoodixChicagoMatchPoint *target_points,
  const gint32                  *target_orientations,
  const gint32                  *source_orientations,
  guint                          point_count,
  guint                          minimum_inliers,
  GoodixChicagoMatchGeometry    *result)
{
  guint surviving;

  g_return_val_if_fail (point_count == 0 || source_points != NULL, 0);
  g_return_val_if_fail (point_count == 0 || target_points != NULL, 0);
  g_return_val_if_fail (point_count == 0 || target_orientations != NULL, 0);
  g_return_val_if_fail (point_count == 0 || source_orientations != NULL, 0);
  g_return_val_if_fail (point_count <= GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT,
                        0);
  g_return_val_if_fail (result != NULL, 0);
  goodix_chicago_match_estimate_geometry (
    source_points, target_points, target_orientations, source_orientations,
    point_count, minimum_inliers, TRUE, result);
  surviving = goodix_chicago_match_filter_orientations (
    result->transform, target_orientations, source_orientations,
    point_count, result->inliers);
  result->inlier_count = surviving;
  if (surviving >= 4 && result->error > 0x4000)
    goodix_chicago_match_refine_geometry (
      source_points, target_points, result->inliers, point_count,
      result->error, result->transform);
  return surviving;
}

guint
goodix_chicago_match_geometry_consensus_records (
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  const GoodixChicagoMatchPair     *pairs,
  guint                             pair_count,
  guint                             minimum_inliers,
  GoodixChicagoMatchGeometry       *result)
{
  GoodixChicagoMatchPoint source[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  GoodixChicagoMatchPoint target[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  gint32 source_orientation[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  gint32 target_orientation[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  guint count = 0;

  g_return_val_if_fail (gallery_count == 0 || gallery_records != NULL, 0);
  g_return_val_if_fail (probe_count == 0 || probe_records != NULL, 0);
  g_return_val_if_fail (pair_count == 0 || pairs != NULL, 0);
  g_return_val_if_fail (result != NULL, 0);
  for (guint index = 0;
       index < pair_count && count < GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT;
       index++)
    {
      const gint gallery_index = pairs[index].old_index;
      const gint probe_index = pairs[index].new_index;
      const GoodixChicagoFeatureRecord *gallery;
      const GoodixChicagoFeatureRecord *probe;

      if (gallery_index < 0 || probe_index < 0 ||
          (guint) gallery_index >= gallery_count ||
          (guint) probe_index >= probe_count)
        continue;
      gallery = &gallery_records[gallery_index];
      probe = &probe_records[probe_index];
      source[count] = (GoodixChicagoMatchPoint){
        (guint16) probe->refined_x, (guint16) probe->refined_y,
      };
      target[count] = (GoodixChicagoMatchPoint){
        (guint16) gallery->refined_x, (guint16) gallery->refined_y,
      };
      source_orientation[count] = probe->orientation;
      target_orientation[count] = gallery->orientation;
      count++;
    }
  return goodix_chicago_match_geometry_consensus (
    source, target, target_orientation, source_orientation, count,
    minimum_inliers, result);
}

static gint32
wrap_orientation (gint32 value,
                  gint32 half_range,
                  gint32 full_range)
{
  if (value > half_range)
    value -= full_range;
  if (value < -half_range)
    value += full_range;
  return value;
}

static gint32
point_distance_quarter (const GoodixChicagoMatchPoint *a,
                        const GoodixChicagoMatchPoint *b)
{
  const gint32 dx = a->x - b->x;
  const gint32 dy = a->y - b->y;

  return (dx * dx >> 2) + (dy * dy >> 2);
}

static gboolean
triangle_edge_is_compatible (gint32 source_distance,
                             gint32 target_distance)
{
  return source_distance * 5 <= target_distance * 6 &&
         source_distance * 6 >= target_distance * 5 &&
         source_distance >= 0x30000 && target_distance >= 0x30000;
}

static gboolean
orientation_triplet_is_compatible (const gint32 delta[3],
                                   gint32       threshold)
{
  const gint32 mean = (delta[0] + delta[1] + delta[2]) / 3;

  for (guint index = 0; index < 3; index++)
    if (delta[index] - mean > threshold ||
        delta[index] - mean < -threshold)
      return FALSE;
  return TRUE;
}

static void
affine_from_three_points (const GoodixChicagoMatchPoint source[3],
                          const GoodixChicagoMatchPoint target[3],
                          gint32                        transform[6])
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
      c_q10 = d_q10 = G_MAXINT32;
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
        a_q10 = b_q10 = G_MAXINT32;
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
transform_is_rigid_and_scaled (const gint32 transform[6])
{
  gint64 a, b, c, d, m00, m01, m11, trace, discriminant;
  gint64 upper_delta, lower_delta;

  /* The coefficient bounds come first: degenerate (collinear) triples give
   * coefficients near 2^29, whose products below would overflow. */
  if (ABS (transform[0] - transform[4]) >= 0x32 ||
      ABS (transform[3] + transform[1]) >= 0x32 ||
      ABS (transform[0]) >= 0x12c || ABS (transform[1]) >= 0x12c ||
      ABS (transform[3]) >= 0x12c || ABS (transform[4]) >= 0x12c)
    return FALSE;

  a = transform[0];
  b = transform[1];
  c = transform[3];
  d = transform[4];
  m00 = a * a + b * b;
  m01 = a * c + b * d;
  m11 = c * c + d * d;
  trace = m00 + m11;
  discriminant = trace * trace + 4 * (m01 * m01 - m11 * m00);
  upper_delta = ((gint64) 0xa3 << 9) - trace;
  lower_delta = ((gint64) 0x191 << 9) - trace;

  return discriminant >= 0 && trace >= 0 && upper_delta <= 0 &&
         lower_delta >= 0 && upper_delta * upper_delta > discriminant &&
         lower_delta * lower_delta > discriminant;
}

void
goodix_chicago_match_estimate_geometry (
  const GoodixChicagoMatchPoint *source_points,
  const GoodixChicagoMatchPoint *target_points,
  const gint32                  *target_orientations,
  const gint32                  *source_orientations,
  guint                          point_count,
  guint                          minimum_inliers,
  gboolean                       strict,
  GoodixChicagoMatchGeometry    *result)
{
  const gint32 half_range = strict ? 0x1922 : 0x3244;
  const gint32 full_range = strict ? 0x3244 : 0x6488;
  const gint32 orientation_threshold = strict ? 0x400 : 0x800;
  guint attempted = 0;

  g_return_if_fail (point_count == 0 || source_points != NULL);
  g_return_if_fail (point_count == 0 || target_points != NULL);
  g_return_if_fail (point_count == 0 || target_orientations != NULL);
  g_return_if_fail (point_count == 0 || source_orientations != NULL);
  g_return_if_fail (point_count <= GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT);
  g_return_if_fail (result != NULL);
  memset (result, 0, sizeof (*result));
  result->error = 0x190000;

  for (guint first = 0; first + 2 < point_count && attempted < 0x3b2; first++)
    {
      const gint32 first_delta = wrap_orientation (
        target_orientations[first] - source_orientations[first],
        half_range, full_range);

      for (guint second = first + 1; second + 1 < point_count; second++)
        {
          const gint32 first_second_source =
            point_distance_quarter (&source_points[first],
                                    &source_points[second]);
          const gint32 first_second_target =
            point_distance_quarter (&target_points[first],
                                    &target_points[second]);
          const gint32 second_delta = wrap_orientation (
            target_orientations[second] - source_orientations[second],
            half_range, full_range);

          if (!triangle_edge_is_compatible (first_second_source,
                                            first_second_target))
            continue;
          for (guint third = second + 1; third < point_count; third++)
            {
              gint32 deltas[3] = {
                first_delta,
                second_delta,
                wrap_orientation (target_orientations[third] -
                                  source_orientations[third],
                                  half_range, full_range),
              };
              const gint32 first_third_source =
                point_distance_quarter (&source_points[first],
                                        &source_points[third]);
              const gint32 first_third_target =
                point_distance_quarter (&target_points[first],
                                        &target_points[third]);
              const gint32 second_third_source =
                point_distance_quarter (&source_points[second],
                                        &source_points[third]);
              const gint32 second_third_target =
                point_distance_quarter (&target_points[second],
                                        &target_points[third]);
              GoodixChicagoMatchPoint source[3];
              GoodixChicagoMatchPoint target[3];
              gint32 transform[6];
              guint8 inliers[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT] = { 0, };
              guint inlier_count = 0;
              gint32 residual_sum = 0;
              gint32 average_error;

              if (!orientation_triplet_is_compatible (
                    deltas, orientation_threshold))
                {
                  if (!strict)
                    continue;
                  for (guint index = 0; index < 3; index++)
                    deltas[index] = wrap_orientation (deltas[index] +
                                                      half_range,
                                                      half_range,
                                                      full_range);
                  if (!orientation_triplet_is_compatible (
                        deltas, orientation_threshold))
                    continue;
                }
              if (!triangle_edge_is_compatible (first_third_source,
                                                first_third_target) ||
                  !triangle_edge_is_compatible (second_third_source,
                                                second_third_target))
                continue;
              attempted++;
              source[0] = source_points[first];
              source[1] = source_points[second];
              source[2] = source_points[third];
              target[0] = target_points[first];
              target[1] = target_points[second];
              target[2] = target_points[third];
              affine_from_three_points (source, target, transform);
              if (!transform_is_rigid_and_scaled (transform))
                continue;
              for (guint index = 0; index < point_count; index++)
                {
                  const gint32 predicted_x =
                    (gint32) ((((gint64) transform[0] * source_points[index].x +
                                (gint64) transform[1] * source_points[index].y +
                                0x80) >> 8) + transform[2]);
                  const gint32 predicted_y =
                    (gint32) ((((gint64) transform[3] * source_points[index].x +
                                (gint64) transform[4] * source_points[index].y +
                                0x80) >> 8) + transform[5]);
                  const gint32 dx = predicted_x - target_points[index].x;
                  const gint32 dy = predicted_y - target_points[index].y;
                  gint32 squared;

                  /* far-off pairs would overflow the square */
                  if (ABS (dx) > 0x280 || ABS (dy) > 0x280)
                    continue;
                  squared = dx * dx + dy * dy;
                  if (squared < 0x64000)
                    {
                      inliers[index] = 1;
                      inlier_count++;
                      residual_sum += squared;
                    }
                }
              if (inlier_count < minimum_inliers)
                continue;
              average_error = inlier_count == 0 ? 0x190000 :
                              ((gint32) (inlier_count >> 1) + residual_sum) /
                              (gint32) inlier_count;
              if (inlier_count < result->inlier_count ||
                  (inlier_count == result->inlier_count &&
                   average_error >= result->error))
                continue;
              memcpy (result->transform, transform, sizeof (transform));
              memcpy (result->inliers, inliers, sizeof (inliers));
              result->inlier_count = inlier_count;
              result->error = average_error;
              if (inlier_count > 20)
                return;
            }
        }
    }
}

static guint
hamming_words (const guint8 *a,
               const guint8 *b,
               guint         words)
{
  guint distance = 0;

  for (guint word = 0; word < words; word++)
    {
      guint32 a_value;
      guint32 b_value;

      memcpy (&a_value, a + word * 4, sizeof (a_value));
      memcpy (&b_value, b + word * 4, sizeof (b_value));
      distance += __builtin_popcount (a_value ^ b_value);
    }
  return distance;
}

void
goodix_chicago_match_init_candidates (
  GoodixChicagoMatchCandidate *candidates,
  guint                        count)
{
  g_return_if_fail (count == 0 || candidates != NULL);

  for (guint index = 0; index < count; index++)
    candidates[index] = (GoodixChicagoMatchCandidate){
      .best_distance = 192,
      .second_distance = 192,
      .best_index = -1,
      .second_index = -1,
    };
}

void
goodix_chicago_match_update_candidates (
  const GoodixChicagoFeatureRecord        *old_records,
  const GoodixChicagoFeatureRecord        *new_records,
  const GoodixChicagoMatchCandidateConfig *config,
  GoodixChicagoMatchCandidate             *candidates,
  guint8                                  *distance_matrix,
  guint8                                  *direction_matrix)
{
  g_return_if_fail (old_records != NULL);
  g_return_if_fail (new_records != NULL);
  g_return_if_fail (config != NULL);
  g_return_if_fail (candidates != NULL);
  g_return_if_fail (distance_matrix != NULL);
  g_return_if_fail (direction_matrix != NULL);
  g_return_if_fail (config->old_begin <= config->old_end);
  g_return_if_fail (config->new_begin <= config->new_end);
  g_return_if_fail (config->new_end <=
                    GOODIX_CHICAGO_MATCH_MATRIX_STRIDE);

  for (guint old_index = config->old_begin;
       old_index < config->old_end;
       old_index++)
    {
      const guint8 *old_record = (const guint8 *) &old_records[old_index];
      GoodixChicagoMatchCandidate *candidate = &candidates[old_index];

      for (guint new_index = config->new_begin;
           new_index < config->new_end;
           new_index++)
        {
          const guint8 *new_record =
            (const guint8 *) &new_records[new_index];
          const guint first = hamming_words (old_record + 0x10,
                                             new_record + 0x10, 2);
          guint straight;
          guint shifted;
          guint straight_tail;
          guint shifted_tail;
          guint distance;
          const guint matrix_index =
            old_index * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE + new_index;

          if (first > config->first_half_gate)
            continue;
          straight = first + hamming_words (old_record + 0x18,
                                            new_record + 0x18, 2);
          shifted = first - hamming_words (old_record + 0x18,
                                           new_record + 0x18, 2) + 64;
          if (straight > config->combined_gate &&
              shifted > config->combined_gate)
            continue;

          straight_tail = straight > config->combined_gate ? 192 :
                          hamming_words (old_record + 0x20, new_record + 0x20, 1) +
                          hamming_words (old_record + 0x28, new_record + 0x28, 1);
          shifted_tail = shifted > config->combined_gate ? 192 :
                         hamming_words (old_record + 0x20, new_record + 0x24, 1) +
                         hamming_words (old_record + 0x28, new_record + 0x30, 1);
          straight += straight_tail;
          shifted += shifted_tail;
          distance = MIN (straight, shifted);
          distance_matrix[matrix_index] = distance;
          direction_matrix[matrix_index] = straight < shifted;

          if ((gint) distance < candidate->best_distance)
            {
              candidate->second_distance = candidate->best_distance;
              candidate->second_index = candidate->best_index;
              candidate->best_distance = distance;
              candidate->best_index = new_index;
            }
          else if ((gint) distance < candidate->second_distance)
            {
              candidate->second_distance = distance;
              candidate->second_index = new_index;
            }
        }
    }
}

guint
goodix_chicago_match_select_candidates (
  const GoodixChicagoFeatureRecord  *new_records,
  const GoodixChicagoMatchCandidate *candidates,
  guint                              candidate_count,
  guint                              limit,
  guint                              best_multiplier,
  guint                              second_multiplier,
  GoodixChicagoMatchPair            *pairs)
{
  typedef struct
  {
    gint old_index;
    gint new_index;
    gint best_distance;
    gint second_distance;
  } Selected;
  g_autofree Selected *selected = NULL;
  guint selected_count = 0;

  g_return_val_if_fail (new_records != NULL, 0);
  g_return_val_if_fail (candidates != NULL, 0);
  g_return_val_if_fail (limit == 0 || pairs != NULL, 0);
  if (limit == 0)
    return 0;
  selected = g_new0 (Selected, limit);
  for (guint old_index = 0; old_index < candidate_count; old_index++)
    {
      const GoodixChicagoMatchCandidate *candidate =
        &candidates[old_index];
      guint scan;
      gboolean replaced = FALSE;

      if (candidate->best_index < 0 ||
          candidate->best_distance * best_multiplier >=
          candidate->second_distance * second_multiplier)
        continue;
      for (scan = 0; scan < selected_count; scan++)
        {
          const gint dx =
            (gint) (guint16) new_records[candidate->best_index].refined_x -
            (gint) (guint16) new_records[selected[scan].new_index].refined_x;
          const gint dy =
            (gint) (guint16) new_records[candidate->best_index].refined_y -
            (gint) (guint16) new_records[selected[scan].new_index].refined_y;

          if (dx * dx + dy * dy >= 0x10000)
            continue;
          if (selected[scan].best_distance <= candidate->best_distance)
            {
              if (!replaced)
                goto next_candidate;
              continue;
            }
          if (!replaced)
            {
              selected[scan] = (Selected){
                old_index, candidate->best_index,
                candidate->best_distance, candidate->second_distance,
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
      if (selected_count < limit)
        {
          selected[selected_count++] = (Selected){
            old_index, candidate->best_index,
            candidate->best_distance, candidate->second_distance,
          };
        }
      else
        {
          guint worst = 0;

          for (guint index = 1; index < selected_count; index++)
            if (selected[index].best_distance > selected[worst].best_distance)
              worst = index;
          if (candidate->best_distance < selected[worst].best_distance)
            {
              selected[worst] = (Selected){
                old_index, candidate->best_index,
                candidate->best_distance, candidate->second_distance,
              };
            }
        }
next_candidate:
      ;
    }
  for (guint index = 0; index < limit; index++)
    pairs[index] = index < selected_count ?
                   (GoodixChicagoMatchPair){
      selected[index].old_index, selected[index].new_index,
    } : (GoodixChicagoMatchPair){ -1, -1 };
  return selected_count;
}

static void
update_column_candidate (GoodixChicagoMatchCandidate *candidate,
                         gint                         row_index,
                         guint8                       distance)
{
  if ((gint) distance < candidate->best_distance)
    {
      candidate->second_distance = candidate->best_distance;
      candidate->second_index = candidate->best_index;
      candidate->best_distance = distance;
      candidate->best_index = row_index;
    }
  else if ((gint) distance < candidate->second_distance)
    {
      candidate->second_distance = distance;
      candidate->second_index = row_index;
    }
}

static guint
ordinary_correspondences_with_gates (
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  guint                             gallery_split,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  guint                             probe_split,
  guint                             first_half_gate,
  guint                             combined_gate,
  GoodixChicagoMatchPair            pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT])
{
  g_autofree GoodixChicagoMatchCandidate *candidates = NULL;
  g_autofree guint8 *distance_matrix = NULL;
  g_autofree guint8 *direction_matrix = NULL;
  GoodixChicagoMatchCandidateConfig config;
  gsize matrix_size;

  g_return_val_if_fail (gallery_count == 0 || gallery_records != NULL, 0);
  g_return_val_if_fail (probe_count == 0 || probe_records != NULL, 0);
  g_return_val_if_fail (gallery_count <= GOODIX_CHICAGO_MATCH_MATRIX_STRIDE,
                        0);
  g_return_val_if_fail (probe_count <= GOODIX_CHICAGO_MATCH_MATRIX_STRIDE,
                        0);
  g_return_val_if_fail (gallery_split <= gallery_count, 0);
  g_return_val_if_fail (probe_split <= probe_count, 0);
  g_return_val_if_fail (pairs != NULL, 0);

  matrix_size = gallery_count * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE;
  candidates = g_new (GoodixChicagoMatchCandidate, gallery_count);
  distance_matrix = g_malloc (MAX (matrix_size, (gsize) 1));
  direction_matrix = g_malloc0 (MAX (matrix_size, (gsize) 1));
  memset (distance_matrix, 0xff, MAX (matrix_size, (gsize) 1));
  goodix_chicago_match_init_candidates (candidates, gallery_count);
  config = (GoodixChicagoMatchCandidateConfig){
    0, gallery_split, 0, probe_split, first_half_gate, combined_gate,
  };
  goodix_chicago_match_update_candidates (
    gallery_records, probe_records, &config, candidates,
    distance_matrix, direction_matrix);
  config = (GoodixChicagoMatchCandidateConfig){
    gallery_split, gallery_count, probe_split, probe_count,
    first_half_gate, combined_gate,
  };
  goodix_chicago_match_update_candidates (
    gallery_records, probe_records, &config, candidates,
    distance_matrix, direction_matrix);
  return goodix_chicago_match_select_candidates (
    probe_records, candidates, gallery_count,
    GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, pairs);
}

void
goodix_chicago_match_build_capacity_relation (
  const GoodixChicagoSubtemplateView *gallery,
  const GoodixChicagoSubtemplateView *probe,
  GoodixChicagoRelation              *relation)
{
  static const gint32 identity[6] = { 0x100, 0, 0, 0, 0x100, 0 };
  GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
  GoodixChicagoMatchPoint source[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  GoodixChicagoMatchPoint target[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  gint32 source_orientation[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  gint32 target_orientation[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
  GoodixChicagoMatchGeometry geometry;
  guint pair_count;
  guint point_count = 0;
  gint selector_score = 0;
  gint metric_a = 0;
  gint metric_b = 0;
  gboolean accepted;

  g_return_if_fail (gallery != NULL);
  g_return_if_fail (probe != NULL);
  g_return_if_fail (relation != NULL);
  pair_count = ordinary_correspondences_with_gates (
    gallery->records, gallery->record_count, gallery->active_count,
    probe->records, probe->record_count, probe->active_count,
    22, 45, pairs);
  for (guint index = 0;
       index < pair_count &&
       point_count < GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT;
       index++)
    {
      const gint probe_index = pairs[index].old_index;
      const gint gallery_index = pairs[index].new_index;

      if (gallery_index < 0 || probe_index < 0 ||
          (guint) gallery_index >= gallery->record_count ||
          (guint) probe_index >= probe->record_count)
        continue;
      source[point_count] = (GoodixChicagoMatchPoint){
        (guint16) gallery->records[gallery_index].refined_x,
        (guint16) gallery->records[gallery_index].refined_y,
      };
      target[point_count] = (GoodixChicagoMatchPoint){
        (guint16) probe->records[probe_index].refined_x,
        (guint16) probe->records[probe_index].refined_y,
      };
      source_orientation[point_count] =
        gallery->records[gallery_index].orientation;
      target_orientation[point_count] =
        probe->records[probe_index].orientation;
      point_count++;
    }
  goodix_chicago_match_estimate_geometry (
    source, target, target_orientation, source_orientation, point_count,
    2, TRUE, &geometry);
  if (geometry.inlier_count > 4 &&
      gallery->has_metric_data && probe->has_metric_data)
    {
      selector_score =
        goodix_chicago_enrollment_calculate_config1_metrics (
          probe->metric_data, gallery->metric_data, geometry.transform,
          &metric_a, &metric_b);
    }

  accepted = geometry.inlier_count >= 15 ||
             (geometry.inlier_count >= 10 &&
              (selector_score > 210 || metric_a >= 205)) ||
             (geometry.inlier_count >= 7 &&
              (selector_score > 220 || metric_a >= 210)) ||
             (geometry.inlier_count >= 5 &&
              (selector_score > 228 || metric_a >= 215));
  if (accepted)
    {
      relation->inlier_count = (gint32) geometry.inlier_count;
      memcpy (relation->transform, geometry.transform,
              sizeof (relation->transform));
    }
  else
    {
      relation->inlier_count = -1;
      memcpy (relation->transform, identity, sizeof (relation->transform));
    }
}

/* Transform shape: mean column norm (Q8 scale), rotation folded to
 * 0..90 degrees, |cos| between the columns in Q16.  The scheduler turns it
 * into the record flags penalty_flag_a (scale outside 234..281),
 * penalty_flag_b (shear >= 0x147b), reserved38[0] (shear >= 0x28f6) and
 * auxiliary_score (angle). */
static guint32
match_integer_sqrt64 (guint64 value)
{
  guint64 bit = 0x80000000u;
  guint32 root = 0;
  gint shift = 31;

  if (value <= 1)
    return (guint32) value;
  while (bit != 0)
    {
      const guint64 trial = ((guint64) (guint32) (root * 2) + bit) << shift;

      shift--;
      if (value >= trial)
        {
          root += (guint32) bit;
          value -= trial;
        }
      bit >>= 1;
    }
  return root;
}

static void
match_transform_shape_type24 (const gint32 transform[6],
                              gint32      *scale,
                              gint32      *angle,
                              gint32      *shear)
{
  const gint32 a = transform[0];
  const gint32 b = transform[1];
  const gint32 c = transform[3];
  const gint32 d = transform[4];
  /* 32-bit wraparound (unsigned: defined in C) for degenerate transforms */
  const gint32 norm_ac = (gint32) ((guint32) a * (guint32) a +
                                   (guint32) c * (guint32) c);
  const gint32 norm_bd = (gint32) ((guint32) b * (guint32) b +
                                   (guint32) d * (guint32) d);
  const gint32 root_ac = (gint32) match_integer_sqrt ((guint32) norm_ac);
  gint32 horizontal;
  gint32 degrees;
  gint32 folded;
  gint16 orientation;
  guint32 denominator;

  *scale = (gint32) (match_integer_sqrt ((guint32) norm_bd) + root_ac) >> 1;
  *angle = 0;
  *shear = 0;
  if (root_ac == 0)
    return;
  horizontal = (a * 256) / root_ac;
  orientation = (gint16) match_cordic_orientation ((c * 256) / root_ac,
                                                   &horizontal);
  if (orientation < 0)
    orientation = (gint16) (orientation + 0x6488);
  degrees = (gint32) (((gint64) orientation * 0x395) >> 16);
  if (degrees >= 180)
    degrees -= 180;
  folded = 180 - degrees;
  *angle = degrees > folded ? folded : degrees;
  denominator = match_integer_sqrt64 ((guint64) ((gint64) norm_ac * norm_bd));
  if (denominator != 0)
    {
      /* 32-bit wraparound */
      const gint64 numerator =
        (gint64) (gint32) ((guint32) b * (guint32) a + (guint32) d * (guint32) c) *
        0x10000;

      *shear = (gint32) (numerator / (gint64) denominator);
      if (*shear < 0)
        *shear = -*shear;
    }
}

/* The transform-guided candidate stage of the
 * second pass-1 geometry attempt.  Each gallery record that the
 * inverse transform maps inside the probe (5-pixel margin) keeps the two
 * nearest probe records among those that have a descriptor distance in the
 * first attempt's matrix (!= 0xff), whose forward-mapped position lies within
 * 0x1500 (Q8, 21 px) of the gallery record on both axes and inside the margin.
 * Coordinates are Q8; the transform maps probe -> gallery. */
static void
match_guided_candidates_type24 (const GoodixChicagoFeatureRecord *gallery_records,
                                guint                             gallery_begin,
                                guint                             gallery_end,
                                const GoodixChicagoFeatureRecord *probe_records,
                                guint                             probe_begin,
                                guint                             probe_end,
                                const gint32                      transform[6],
                                const guint8                     *distance_matrix,
                                GoodixChicagoMatchCandidate      *candidates)
{
  const gint32 width = 80;
  const gint32 height = 64;
  gint32 inverse[6];

  goodix_chicago_match_invert_transform_q8_type24 (transform, inverse);
  for (guint g = gallery_begin; g < gallery_end; g++)
    {
      const gint32 x = (guint16) gallery_records[g].refined_x;
      const gint32 y = (guint16) gallery_records[g].refined_y;
      const gint32 px = ((((inverse[2] * 256) + y * inverse[1] +
                           x * inverse[0] + 0x80) >> 8) + 0x80) >> 8;
      const gint32 py = ((((inverse[5] * 256) + 0x80 + y * inverse[4] +
                           x * inverse[3]) >> 8) + 0x80) >> 8;
      GoodixChicagoMatchCandidate *candidate = &candidates[g];

      if (px >= width - 5 || py >= height - 5 || px <= 5 || py <= 5)
        continue;
      for (guint p = probe_begin; p < probe_end; p++)
        {
          const gint32 d =
            distance_matrix[g * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE + p];
          const gint32 X = (guint16) probe_records[p].refined_x;
          const gint32 Y = (guint16) probe_records[p].refined_y;
          const gint32 fx = transform[1] * Y + (transform[2] * 256) +
                            transform[0] * X;
          const gint32 fy = transform[4] * Y + transform[3] * X +
                            (transform[5] * 256);
          gint32 qx;
          gint32 qy;

          if (d == 0xff || ABS ((fx >> 8) - x) > 0x1500 ||
              ABS ((fy >> 8) - y) > 0x1500)
            continue;
          qx = (((fx + 0x80) >> 8) + 0x80) >> 8;
          qy = (((fy + 0x80) >> 8) + 0x80) >> 8;
          if (qx > width - 5 || qy > height - 5 || qx <= 5 || qy <= 5)
            continue;
          if (d < candidate->best_distance)
            {
              candidate->second_distance = candidate->best_distance;
              candidate->second_index = candidate->best_index;
              candidate->best_distance = d;
              candidate->best_index = p;
            }
          else if (d < candidate->second_distance)
            {
              candidate->second_distance = d;
              candidate->second_index = p;
            }
        }
    }
}

/* Descriptor distance of pass 2: the straight or
 * the shifted half of the second descriptor block, 2 words each. */
static guint
match_pass2_distance (const GoodixChicagoFeatureRecord *gallery_record,
                      const GoodixChicagoFeatureRecord *probe_record,
                      gboolean                          straight)
{
  return hamming_words ((const guint8 *) gallery_record + 0x28,
                        (const guint8 *) probe_record + (straight ? 0x28 : 0x30), 2);
}

static void
match_candidate_offer (GoodixChicagoMatchCandidate *candidate,
                       gint                         distance,
                       gint                         index)
{
  if (distance < candidate->best_distance)
    {
      candidate->second_distance = candidate->best_distance;
      candidate->second_index = candidate->best_index;
      candidate->best_distance = distance;
      candidate->best_index = index;
    }
  else if (distance < candidate->second_distance)
    {
      candidate->second_distance = distance;
      candidate->second_index = index;
    }
}

/* Pass-2 variant of the transform-guided candidate stage: the distance is
 * recomputed as the smaller of both halves (not taken from the attempt-1
 * matrix) and must be <= 16. */
static void
match_guided_candidates_pass2_type24 (const GoodixChicagoFeatureRecord *gallery_records,
                                      guint                             gallery_begin,
                                      guint                             gallery_end,
                                      const GoodixChicagoFeatureRecord *probe_records,
                                      guint                             probe_begin,
                                      guint                             probe_end,
                                      const gint32                      transform[6],
                                      GoodixChicagoMatchCandidate      *candidates)
{
  const gint32 width = 80;
  const gint32 height = 64;
  gint32 inverse[6];

  goodix_chicago_match_invert_transform_q8_type24 (transform, inverse);
  for (guint g = gallery_begin; g < gallery_end; g++)
    {
      const gint32 x = (guint16) gallery_records[g].refined_x;
      const gint32 y = (guint16) gallery_records[g].refined_y;
      const gint32 px = ((((inverse[2] * 256) + y * inverse[1] +
                           x * inverse[0] + 0x80) >> 8) + 0x80) >> 8;
      const gint32 py = ((((inverse[5] * 256) + 0x80 + y * inverse[4] +
                           x * inverse[3]) >> 8) + 0x80) >> 8;

      if (px >= width - 5 || py >= height - 5 || px <= 5 || py <= 5)
        continue;
      for (guint p = probe_begin; p < probe_end; p++)
        {
          const gint32 X = (guint16) probe_records[p].refined_x;
          const gint32 Y = (guint16) probe_records[p].refined_y;
          const gint32 fx = transform[1] * Y + (transform[2] * 256) +
                            transform[0] * X;
          const gint32 fy = transform[4] * Y + transform[3] * X +
                            (transform[5] * 256);
          gint32 qx, qy;
          guint d, d2;

          if (ABS ((fx >> 8) - x) > 0x1500 || ABS ((fy >> 8) - y) > 0x1500)
            continue;
          qx = (((fx + 0x80) >> 8) + 0x80) >> 8;
          qy = (((fy + 0x80) >> 8) + 0x80) >> 8;
          if (qx > width - 5 || qy > height - 5 || qx <= 5 || qy <= 5)
            continue;
          d = match_pass2_distance (&gallery_records[g], &probe_records[p], TRUE);
          d2 = match_pass2_distance (&gallery_records[g], &probe_records[p], FALSE);
          d = MIN (d, d2);
          if (d > 16)
            continue;
          match_candidate_offer (&candidates[g], d, p);
        }
    }
}

/* Cross-class candidates (pass 2 with <= 3
 * inliers): gallery records of one minutia class (dword +0xc, 1 or 2) against
 * probe records of the other class; only pairs that have an attempt-1 matrix
 * entry (!= 0xff), the half chosen by the attempt-1 direction, distance <= 16. */
static void
match_cross_class_candidates_type24 (const GoodixChicagoFeatureRecord *gallery_records,
                                     const GoodixChicagoFeatureRecord *probe_records,
                                     const guint8                     *distance_matrix,
                                     const guint8                     *direction_matrix,
                                     const guint8                     *gallery_list,
                                     guint                             gallery_list_count,
                                     const guint8                     *probe_list,
                                     guint                             probe_list_count,
                                     GoodixChicagoMatchCandidate      *candidates)
{
  for (guint i = 0; i < gallery_list_count; i++)
    for (guint j = 0; j < probe_list_count; j++)
      {
        const guint g = gallery_list[i];
        const guint p = probe_list[j];
        const guint m = g * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE + p;
        guint d;

        if (distance_matrix[m] == 0xff)
          continue;
        d = match_pass2_distance (&gallery_records[g], &probe_records[p],
                                  direction_matrix[m] != 0);
        if (d > 16)
          continue;
        match_candidate_offer (&candidates[g], d, p);
      }
}

static gint32
match_record_class (const GoodixChicagoFeatureRecord *record)
{
  gint32 value;

  memcpy (&value, (const guint8 *) record + 0xc, sizeof (value));
  return value;
}

/* Indices of the class-1 and class-2 records in [begin, end). */
static void
match_split_classes (const GoodixChicagoFeatureRecord *records,
                     guint begin, guint end,
                     guint8 *class1, guint *count1,
                     guint8 *class2, guint *count2)
{
  *count1 = *count2 = 0;
  for (guint i = begin; i < end; i++)
    {
      const gint32 c = match_record_class (&records[i]);

      if (c == 1)
        class1[(*count1)++] = i;
      else if (c == 2)
        class2[(*count2)++] = i;
    }
}

/* One-pair metric (inlier-gated selector, shared occupancy). */
extern gint goodix_chicago_enrollment_calculate_config1_metrics_ex (const GoodixChicagoMetricData *new_metric_data,
                                                                    const GoodixChicagoMetricData *old_metric_data,
                                                                    const gint32                   transform[6],
                                                                    gint                           inliers,
                                                                    gint                          *occupancy_io,
                                                                    gint                          *metric_a,
                                                                    gint                          *metric_b) __attribute__((weak));

static gint
match_config1_pair (const GoodixChicagoMetricData *probe_md,
                    const GoodixChicagoMetricData *gallery_md,
                    const gint32 transform[6], gint inliers, gint *occupancy,
                    gint *a, gint *c)
{
  if (goodix_chicago_enrollment_calculate_config1_metrics_ex)
    return goodix_chicago_enrollment_calculate_config1_metrics_ex (
      probe_md, gallery_md, transform, inliers, occupancy, a, c);
  return goodix_chicago_enrollment_calculate_config1_metrics (probe_md, gallery_md,
                                                              transform, a, c);
}

/* Metric as used by the matcher (with the inlier count of the transform). */
static void
match_record_metrics_type24 (const GoodixChicagoSubtemplateView *gallery,
                             const GoodixChicagoSubtemplateView *probe,
                             const gint32                        transform[6],
                             gint                                inliers,
                             gint32                             *selector,
                             gint32                             *agreement,
                             gint32                             *coverage,
                             gint32                             *flag_scale,
                             gint32                             *flag_shear,
                             gint32                             *flag_shear2,
                             gint32                             *angle)
{
  gint32 scale, shear;
  gint a = 0, c = 0, occupancy = -1;

  *selector = match_config1_pair (probe->metric_data, gallery->metric_data, transform,
                                  inliers, &occupancy, &a, &c);
  *agreement = a;
  {
    /* The same metric on the second map pair; a higher
     * agreement is taken when the first selector is > 220, a higher selector
     * when the second agreement is > 195. */
    GoodixChicagoMetricData p2 = *probe->metric_data;
    GoodixChicagoMetricData g2 = *gallery->metric_data;
    gint a2 = 0, c2 = 0, s2;

    memcpy (p2.primary, probe->metric_data->secondary, sizeof (p2.primary));
    memcpy (g2.primary, gallery->metric_data->secondary, sizeof (g2.primary));
    s2 = match_config1_pair (&p2, &g2, transform, inliers, &occupancy, &a2, &c2);
    if (a2 > a && *selector > 220)
      *agreement = a2;
    if (s2 > *selector && a2 > 195)
      *selector = s2;
  }
  *coverage = (c * 137) >> 8;
  match_transform_shape_type24 (transform, &scale, angle, &shear);
  *flag_scale = (guint32) (scale - 234) > 47;
  *flag_shear = shear >= 0x147b;
  *flag_shear2 = shear >= 0x28f6;
}

/* Pass 2 of the scheduler, for type 24
 * (the record as left by pass 1; transform = the pass-1 transform, replaced in
 * place).  Only with geometry_count <= 8 and secondary_geometry_count <= 8:
 * the base is the pass-1 transform (secondary_geometry_count > 3) or the better of two cross-class
 * geometries.  With >= 3 base
 * inliers, pairs are re-selected around the base transform
 * and the geometry re-run.  If that has more inliers than the base: its metrics
 * replace the record's (and the transform) when selector > 128 and the
 * flag-penalised agreement is higher, and its count becomes reserved08[0] when its shape
 * is plausible and the overlap is large enough.  In every case the tail adds one
 * inlier to both geometry counts (at most 31) when matched_percent > 30 and
 * the mean distance of the last overlap is < 10. */
static void
match_pass2_type24 (const GoodixChicagoSubtemplateView *gallery,
                    const GoodixChicagoSubtemplateView *probe,
                    GoodixChicagoMatchScoreRecord      *record,
                    gint32                              transform[6],
                    const guint8                       *distance_matrix,
                    const guint8                       *direction_matrix,
                    GoodixChicagoMatchCandidate        *candidates,
                    gint32                             *mean_distance)
{
  static const gint32 identity[6] = { 0x100, 0, 0, 0, 0x100, 0 };
  const guint gallery_count = gallery->record_count;
  GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
  GoodixChicagoMatchGeometry second;
  gint32 base[6];
  guint n1;
  guint n2 = 0;

  if (record->geometry_count > 8 || record->secondary_geometry_count > 8)
    goto tail;
  n1 = record->secondary_geometry_count;
  if (n1 > 3)
    {
      memcpy (base, transform, sizeof (base));
    }
  else
    {
      guint8 g1[GOODIX_CHICAGO_MATCH_MATRIX_STRIDE], g2[GOODIX_CHICAGO_MATCH_MATRIX_STRIDE];
      guint8 p1[GOODIX_CHICAGO_MATCH_MATRIX_STRIDE], p2[GOODIX_CHICAGO_MATCH_MATRIX_STRIDE];
      guint ng1, ng2, np1, np2;
      g_autofree GoodixChicagoMatchCandidate *set2 =
        g_new (GoodixChicagoMatchCandidate, MAX (gallery_count, 1u));
      GoodixChicagoMatchGeometry a, b;

      goodix_chicago_match_init_candidates (candidates, gallery_count);
      goodix_chicago_match_init_candidates (set2, gallery_count);
      match_split_classes (gallery->records, 0, gallery->active_count, g1, &ng1, g2, &ng2);
      match_split_classes (probe->records, 0, probe->active_count, p1, &np1, p2, &np2);
      match_cross_class_candidates_type24 (gallery->records, probe->records,
                                           distance_matrix, direction_matrix,
                                           g1, ng1, p2, np2, candidates);
      match_cross_class_candidates_type24 (gallery->records, probe->records,
                                           distance_matrix, direction_matrix,
                                           g2, ng2, p1, np1, set2);
      match_split_classes (gallery->records, gallery->active_count, gallery_count,
                           g1, &ng1, g2, &ng2);
      match_split_classes (probe->records, probe->active_count, probe->record_count,
                           p1, &np1, p2, &np2);
      match_cross_class_candidates_type24 (gallery->records, probe->records,
                                           distance_matrix, direction_matrix,
                                           g1, ng1, p2, np2, candidates);
      match_cross_class_candidates_type24 (gallery->records, probe->records,
                                           distance_matrix, direction_matrix,
                                           g2, ng2, p1, np1, set2);
      goodix_chicago_match_select_candidates (
        probe->records, candidates, gallery_count,
        GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, pairs);
      memset (&a, 0, sizeof (a));
      memcpy (a.transform, identity, sizeof (identity));
      goodix_chicago_match_geometry_consensus_records (
        gallery->records, gallery_count, probe->records, probe->record_count,
        pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, &a);
      goodix_chicago_match_select_candidates (
        probe->records, set2, gallery_count,
        GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, pairs);
      memset (&b, 0, sizeof (b));
      memcpy (b.transform, identity, sizeof (identity));
      goodix_chicago_match_geometry_consensus_records (
        gallery->records, gallery_count, probe->records, probe->record_count,
        pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, &b);
      if (a.inlier_count > b.inlier_count)
        {
          n1 = a.inlier_count;
          memcpy (base, a.transform, sizeof (base));
        }
      else
        {
          n1 = b.inlier_count;
          memcpy (base, b.transform, sizeof (base));
        }
      if ((gint32) n1 > record->geometry_count)
        record->geometry_count = n1;
    }
  if (n1 >= 3)
    {
      goodix_chicago_match_init_candidates (candidates, gallery_count);
      match_guided_candidates_pass2_type24 (gallery->records, 0, gallery->active_count,
                                            probe->records, 0, probe->active_count,
                                            base, candidates);
      match_guided_candidates_pass2_type24 (gallery->records, gallery->active_count,
                                            gallery_count, probe->records,
                                            probe->active_count, probe->record_count,
                                            base, candidates);
      goodix_chicago_match_select_candidates (
        probe->records, candidates, gallery_count,
        GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, pairs);
      memset (&second, 0, sizeof (second));
      memcpy (second.transform, identity, sizeof (identity));
      goodix_chicago_match_geometry_consensus_records (
        gallery->records, gallery_count, probe->records, probe->record_count,
        pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, &second);
      n2 = second.inlier_count;
    }
  if (n2 <= n1)
    goto tail;
  {
    GoodixChicagoMatchFeatureOverlap overlap;
    gint32 selector2, agreement2, coverage2, flag_a2, flag_b2, flag_c2, dummy;
    gint32 scale, angle, shear;

    if (gallery->has_metric_data && probe->has_metric_data)
      {
        match_record_metrics_type24 (gallery, probe, second.transform, n2,
                                     &selector2, &agreement2, &coverage2,
                                     &flag_a2, &flag_b2, &flag_c2, &dummy);
        if (selector2 > 128 &&
            agreement2 - 4 * (flag_a2 + flag_b2) >
            record->agreement - 4 * (record->penalty_flag_a +
                                     record->penalty_flag_b))
          {
            record->selector = selector2;
            record->agreement = agreement2;
            record->normalized_coverage = coverage2;
            record->penalty_flag_a = flag_a2;
            record->penalty_flag_b = flag_b2;
            record->reserved38[0] = flag_c2;
            memcpy (transform, second.transform, sizeof (second.transform));
          }
      }
    goodix_chicago_match_feature_overlap_type24 (
      gallery->records, gallery_count, probe->records, probe->record_count,
      80, 64, second.transform, n2, &overlap);
    match_transform_shape_type24 (second.transform, &scale, &angle, &shear);
    if (shear >= 0x28f6 || (guint32) (scale - 234) > 47)
      goto tail;
    if (!((overlap.matched_percent > 40 && overlap.matched_count >= 24) ||
          (overlap.matched_percent > 45 && overlap.matched_count >= 12) ||
          overlap.matched_percent > 75 || n2 > 16))
      goto tail;
    record->matched_percent = overlap.matched_percent;
    record->geometry_percent = overlap.geometry_percent;
    record->reserved08[0] = n2;
    *mean_distance = overlap.mean_distance;
  }
tail:
  if (record->matched_percent > 30 && *mean_distance < 10)
    {
      record->geometry_count++;
      record->secondary_geometry_count++;
    }
  record->geometry_count = MIN (record->geometry_count, 31);
  record->secondary_geometry_count = MIN (record->secondary_geometry_count, 31);
}

/* Pass 1 for type 24.  Attempt 1 is the
 * ordinary correspondence + geometry (both geometry counts = inliers).  When its
 * agreement is > 180 and it has < 16 inliers, attempt 2 re-pairs the records
 * guided by the attempt-1 transform and re-runs the geometry.  Attempt 2's transform
 * replaces the first when its selector is > 128 and its agreement, minus 4
 * per scale/shear flag, is higher; if it has more inliers, secondary_geometry_count takes its
 * count and the feature overlap is taken from it (type 24 skips the other
 * acceptance tests).  With > 4 inliers and a
 * matched percent < 30 the overlap is recomputed from attempt 1's count and
 * the final transform. */
void
goodix_chicago_match_score_subtemplate_type24 (
  const GoodixChicagoSubtemplateView *gallery,
  const GoodixChicagoSubtemplateView *probe,
  GoodixChicagoMatchScoreRecord      *record,
  GoodixChicagoMatchGeometry         *geometry)
{
  GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
  GoodixChicagoMatchFeatureOverlap overlap;
  g_autofree guint8 *distance_matrix = NULL;
  g_autofree guint8 *direction_matrix = NULL;
  g_autofree GoodixChicagoMatchCandidate *candidates = NULL;
  GoodixChicagoMatchCandidateConfig config;
  const guint gallery_count = gallery->record_count;
  const gsize matrix_size = MAX ((gsize) gallery_count *
                                 GOODIX_CHICAGO_MATCH_MATRIX_STRIDE, (gsize) 1);
  guint first_count;
  gint32 dummy;
  gint32 mean_distance = 0;     /* mean distance of the last overlap */

  g_return_if_fail (gallery != NULL);
  g_return_if_fail (probe != NULL);
  g_return_if_fail (record != NULL);
  g_return_if_fail (geometry != NULL);
  memset (record, 0, sizeof (*record));
  record->probe_quality = probe->quality;
  record->probe_coverage = probe->coverage;
  record->gallery_quality = gallery->quality;
  record->gallery_coverage = gallery->coverage;
  /* the candidate ranges below cover the active records only */
  g_return_if_fail (gallery->active_count <= gallery_count);
  g_return_if_fail (probe->active_count <= probe->record_count);

  /* attempt 1: == goodix_chicago_match_ordinary_correspondences, keeping the
   * distance matrix for attempt 2 */
  candidates = g_new (GoodixChicagoMatchCandidate, MAX (gallery_count, 1u));
  distance_matrix = g_malloc (matrix_size);
  direction_matrix = g_malloc0 (matrix_size);
  memset (distance_matrix, 0xff, matrix_size);
  goodix_chicago_match_init_candidates (candidates, gallery_count);
  config = (GoodixChicagoMatchCandidateConfig){
    0, gallery->active_count, 0, probe->active_count, 23, 47,
  };
  goodix_chicago_match_update_candidates (
    gallery->records, probe->records, &config, candidates,
    distance_matrix, direction_matrix);
  config = (GoodixChicagoMatchCandidateConfig){
    gallery->active_count, gallery_count,
    probe->active_count, probe->record_count, 23, 47,
  };
  goodix_chicago_match_update_candidates (
    gallery->records, probe->records, &config, candidates,
    distance_matrix, direction_matrix);
  goodix_chicago_match_select_candidates (
    probe->records, candidates, gallery_count,
    GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, pairs);
  goodix_chicago_match_geometry_consensus_records (
    gallery->records, gallery_count,
    probe->records, probe->record_count,
    pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, geometry);
  first_count = geometry->inlier_count;
  record->geometry_count = first_count;
  record->secondary_geometry_count = first_count;
  if (first_count < 3 || !gallery->has_metric_data ||
      !probe->has_metric_data)
    goto pass2;

  match_record_metrics_type24 (gallery, probe, geometry->transform, first_count,
                               &record->selector, &record->agreement,
                               &record->normalized_coverage,
                               &record->penalty_flag_a, &record->penalty_flag_b,
                               &record->reserved38[0], &record->auxiliary_score);

  if (record->agreement > 180 && first_count < 16)
    {
      GoodixChicagoMatchGeometry second;
      gint32 selector2, agreement2, coverage2, flag_a2, flag_b2, flag_c2;

      goodix_chicago_match_init_candidates (candidates, gallery_count);
      match_guided_candidates_type24 (gallery->records, 0, gallery->active_count,
                                      probe->records, 0, probe->active_count,
                                      geometry->transform, distance_matrix,
                                      candidates);
      match_guided_candidates_type24 (gallery->records, gallery->active_count,
                                      gallery_count, probe->records,
                                      probe->active_count, probe->record_count,
                                      geometry->transform, distance_matrix,
                                      candidates);
      goodix_chicago_match_select_candidates (
        probe->records, candidates, gallery_count,
        GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, first_count <= 10 ? 36 : 38,
        pairs);
      goodix_chicago_match_geometry_consensus_records (
        gallery->records, gallery_count,
        probe->records, probe->record_count,
        pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, &second);
      if (second.inlier_count >= 3)
        {
          match_record_metrics_type24 (gallery, probe, second.transform,
                                       second.inlier_count, &selector2, &agreement2, &coverage2,
                                       &flag_a2, &flag_b2, &flag_c2, &dummy);
          if (selector2 > 128 &&
              agreement2 - 4 * (flag_a2 + flag_b2) >
              record->agreement - 4 * (record->penalty_flag_a +
                                       record->penalty_flag_b))
            {
              record->selector = selector2;
              record->agreement = agreement2;
              record->normalized_coverage = coverage2;
              record->penalty_flag_a = flag_a2;
              record->penalty_flag_b = flag_b2;
              record->reserved38[0] = flag_c2;
              memcpy (geometry->transform, second.transform,
                      sizeof (geometry->transform));
              match_transform_shape_type24 (geometry->transform, &dummy,
                                            &record->auxiliary_score, &dummy);
            }
          if (second.inlier_count > first_count)
            {
              goodix_chicago_match_feature_overlap_type24 (
                gallery->records, gallery_count,
                probe->records, probe->record_count, 80, 64,
                second.transform, second.inlier_count, &overlap);
              record->matched_percent = overlap.matched_percent;
              record->geometry_percent = overlap.geometry_percent;
              record->secondary_geometry_count = second.inlier_count;
              mean_distance = overlap.mean_distance;
            }
        }
    }
  if (first_count > 4 && record->matched_percent < 30)
    {
      goodix_chicago_match_feature_overlap_type24 (
        gallery->records, gallery_count,
        probe->records, probe->record_count, 80, 64,
        geometry->transform, first_count, &overlap);
      record->matched_percent = overlap.matched_percent;
      record->geometry_percent = overlap.geometry_percent;
      mean_distance = overlap.mean_distance;
    }
pass2:
  /* Pass 2, then secondary_geometry_count = max(it, reserved08[0]). */
  match_pass2_type24 (gallery, probe, record, geometry->transform,
                      distance_matrix, direction_matrix, candidates,
                      &mean_distance);
  record->secondary_geometry_count = MAX (record->secondary_geometry_count,
                                          record->reserved08[0]);
  geometry->inlier_count = record->secondary_geometry_count;
  if (!gallery->has_metric_data || !probe->has_metric_data)
    return;
  /* The scheduler recomputes the metrics and the shape
   * flags from the final transform (pass 2 may have replaced it). */
  match_record_metrics_type24 (gallery, probe, geometry->transform,
                               record->secondary_geometry_count,
                               &record->selector, &record->agreement,
                               &record->normalized_coverage,
                               &record->penalty_flag_a, &record->penalty_flag_b,
                               &record->reserved38[0], &record->auxiliary_score);
  goodix_chicago_enrollment_calculate_study_metrics (
    probe->metric_data, gallery->metric_data, geometry->transform,
    &record->study_metric_18, &record->study_metric_1c,
    &record->study_metric_20);
}


void
goodix_chicago_match_invert_transform_q8_type24 (
  const gint32 transform[6],
  gint32       inverse[6])
{
  gint64 determinant;

  g_return_if_fail (transform != NULL);
  g_return_if_fail (inverse != NULL);
  determinant = (gint64) transform[0] * transform[4] -
                (gint64) transform[1] * transform[3];
  if (determinant == 0)
    {
      memcpy (inverse, transform, sizeof (gint32) * 6);
      return;
    }

  /* Preserves Q8 coefficients by promoting the inverse
   * numerator to Q16.  C's signed division provides the required truncation
   * toward zero for negative transforms as well. */
  inverse[0] = (gint32) ((gint64) transform[4] * 0x10000 /
                         determinant);
  inverse[1] = (gint32) (-(gint64) transform[1] * 0x10000 /
                         determinant);
  inverse[2] = (gint32) ((((gint64) transform[5] * transform[1] -
                           (gint64) transform[4] * transform[2]) * 0x100) /
                         determinant);
  inverse[3] = (gint32) (-(gint64) transform[3] * 0x10000 /
                         determinant);
  inverse[4] = (gint32) ((gint64) transform[0] * 0x10000 /
                         determinant);
  inverse[5] = (gint32) ((((gint64) transform[3] * transform[2] -
                           (gint64) transform[5] * transform[0]) * 0x100) /
                         determinant);
}

gint32
goodix_chicago_match_transform_overlap_area_type24 (
  guint        width,
  guint        height,
  const gint32 transform[6])
{
  const gint64 maximum_x = ((gint64) width - 1) * 0x100;
  const gint64 maximum_y = ((gint64) height - 1) * 0x100;
  gint32 area = 0;

  g_return_val_if_fail (width > 0, 0);
  g_return_val_if_fail (height > 0, 0);
  g_return_val_if_fail (transform != NULL, 0);
  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        const gint64 transformed_x =
          (gint64) transform[0] * x + (gint64) transform[1] * y +
          transform[2];
        const gint64 transformed_y =
          (gint64) transform[3] * x + (gint64) transform[4] * y +
          transform[5];

        if (transformed_x >= 0 && transformed_x <= maximum_x &&
            transformed_y >= 0 && transformed_y <= maximum_y)
          area++;
      }
  return area;
}

void
goodix_chicago_match_decode_gallery_resolution_evidence (
  guint32                               packed_resolution,
  GoodixChicagoMatchResolutionEvidence *evidence)
{
  static const gint32 primary_map[4] = { 0, 2, 2, 3 };
  static const gint32 secondary_map[8] = { 0, 1, 2, 4, 5, 5, 0, 0 };

  g_return_if_fail (evidence != NULL);
  evidence->primary = primary_map[packed_resolution & 3];
  evidence->secondary = secondary_map[(packed_resolution >> 8) & 7];
}

void
goodix_chicago_match_scheduler_auxiliary_init_type24 (
  guint32                               probe_packed_resolution,
  GoodixChicagoMatchSchedulerAuxiliary *state)
{
  GoodixChicagoMatchResolutionEvidence evidence;

  g_return_if_fail (state != NULL);
  goodix_chicago_match_decode_gallery_resolution_evidence (
    probe_packed_resolution, &evidence);
  *state = (GoodixChicagoMatchSchedulerAuxiliary){
    .auxiliary_count = evidence.secondary,
  };
}

gint32
goodix_chicago_match_scheduler_auxiliary_consume_type24 (
  GoodixChicagoMatchSchedulerAuxiliary *state,
  guint32                               gallery_packed_resolution,
  gint32                                prior_rejection_count)
{
  GoodixChicagoMatchResolutionEvidence evidence;
  gint32 combined_count;

  g_return_val_if_fail (state != NULL, 0);
  goodix_chicago_match_decode_gallery_resolution_evidence (
    gallery_packed_resolution, &evidence);

  /* Type 24 fixes the scheduler mode to two, selecting addition
   * rather than max() for the per-gallery combined evidence. */
  combined_count = MIN (state->auxiliary_count + evidence.secondary, 5);
  if (evidence.secondary > 3)
    state->auxiliary_count = MAX (state->auxiliary_count,
                                  evidence.secondary);
  if (prior_rejection_count > 5 &&
      state->auxiliary_count < 5 &&
      !state->raised)
    {
      state->auxiliary_count++;
      state->raised = TRUE;
    }
  return combined_count;
}

gint32
goodix_chicago_match_study_aggregate_q8_type24 (gint32 count_zero,
                                                gint32 count_mixed,
                                                gint32 count_one)
{
  gint32 denominator;

  g_return_val_if_fail (count_zero >= 0, 0);
  g_return_val_if_fail (count_mixed >= 0, 0);
  g_return_val_if_fail (count_one >= 0, 0);
  g_return_val_if_fail (count_zero + count_mixed + count_one <=
                        GOODIX_CHICAGO_METRIC_MAP_WIDTH *
                        GOODIX_CHICAGO_METRIC_MAP_HEIGHT, 0);
  denominator = count_zero + count_mixed + count_one + 1;
  return ((count_zero + count_one) << 8) / denominator;
}

gboolean
goodix_chicago_match_candidate_prefilter_type24 (
  const GoodixChicagoMatchScoreRecord *record,
  gint32                               auxiliary_count,
  gint32                               candidate_metric)
{
  const gint32 *value = (const gint32 *) record;
  gboolean rejected = FALSE;
  gint32 metric_24;

  g_return_val_if_fail (record != NULL, TRUE);
  if (candidate_metric < 4 ||
      ((value[5] > 204 || value[0] > 15) &&
       (value[5] > 209 || value[0] > 6)))
    {
      if (candidate_metric > 2)
        {
          if (value[5] > 189 || value[11] > 29 || value[4] > 209)
            {
              if (auxiliary_count < 2)
                goto done;
              if (value[5] > 199 || value[11] > 34 || value[4] > 224)
                goto done;
            }
          rejected = TRUE;
        }
    }
  else
    {
      rejected = TRUE;
    }

  metric_24 = value[9];
  if (auxiliary_count > 1 &&
      (((metric_24 < 60 && value[1] < 9 && value[5] < 214 &&
         value[8] < 213) ||
        (metric_24 < 98 && value[1] < 15 && value[8] < 202 &&
         value[5] < 205 && value[11] < 30) ||
        (metric_24 < 109 && value[1] < 15 && value[8] < 203 &&
         value[5] < 193 && value[11] < 27) ||
        (metric_24 < 105 && value[1] < 17 && auxiliary_count > 2 &&
         value[5] < 200 && value[11] < 37) ||
        (metric_24 < 93 && value[1] < 10 && auxiliary_count > 2 &&
         value[5] < 215 && value[11] < 24) ||
        (candidate_metric > 0 && metric_24 < 95 && value[1] < 10 &&
         value[8] < 210 && value[5] < 205 && value[11] < 31))))
    rejected = TRUE;

done:
  return rejected;
}

void
goodix_chicago_match_filter_study_admission_type24 (
  const GoodixChicagoMatchScoreRecord         *record,
  const GoodixChicagoMatchStudyAdmissionInput *input,
  gint32                                      *status,
  gint32                                      *study_eligible)
{
  gint32 scaled_coverage;
  gboolean inspect_weak_evidence;
  gboolean weak_source;

  g_return_if_fail (record != NULL);
  g_return_if_fail (input != NULL);
  g_return_if_fail (status != NULL);
  g_return_if_fail (study_eligible != NULL);

  scaled_coverage =
    (record->geometry_percent * record->normalized_coverage) >> 8;

  if (input->auxiliary_count >= 1 &&
      record->geometry_count < 10 &&
      record->normalized_coverage < 180 &&
      record->geometry_percent < 20 &&
      record->agreement <= 210 &&
      record->study_metric_20 < 195 &&
      record->matched_percent < 40)
    *study_eligible = 0;
  if (input->probe_auxiliary >= 1 &&
      record->geometry_count < 10 &&
      record->geometry_percent < 40 &&
      record->normalized_coverage < 80 &&
      record->agreement <= 218 &&
      record->study_metric_20 < 205 &&
      record->matched_percent < 70)
    *study_eligible = 0;

  if (input->auxiliary_count < 2 && input->candidate_metric == 0)
    return;
  if (*status == 0)
    return;

  if (*status == 1 &&
      input->auxiliary_count >= 5 &&
      input->candidate_metric >= 1 &&
      record->matched_percent < 47 &&
      input->probe_quality < 58 &&
      scaled_coverage < 11)
    {
      *status = 0;
      *study_eligible = 0;
    }

  inspect_weak_evidence =
    (input->auxiliary_count >= 3 &&
     input->candidate_metric >= 4 &&
     input->probe_quality < 80 &&
     record->matched_percent < 70) ||
    (input->auxiliary_count >= 2 &&
     input->candidate_metric >= 2 &&
     input->probe_quality < 55 &&
     scaled_coverage < 20);
  if (*status == 1 && inspect_weak_evidence)
    {
      if (record->geometry_count <= 18 &&
          record->agreement <= 200 &&
          record->study_metric_20 < 200 &&
          scaled_coverage < 15)
        {
          *status = 0;
          *study_eligible = 0;
        }
      else if (record->geometry_count <= 12 &&
               ((input->auxiliary_count >= 4 &&
                 record->agreement <= 200 &&
                 record->study_metric_20 < 200) ||
                (record->agreement <= 215 &&
                 record->study_metric_20 < 210 &&
                 scaled_coverage < 18)))
        {
          *status = 0;
          *study_eligible = 0;
        }
      else if (record->geometry_count <= 10 &&
               ((record->agreement <= 210 &&
                 record->study_metric_20 < 215 &&
                 scaled_coverage < 15) ||
                (record->agreement <= 200 &&
                 record->study_metric_20 < 200)))
        {
          *status = 0;
          *study_eligible = 0;
        }
    }

  if (input->auxiliary_count >= 4 &&
      input->candidate_metric == 1 &&
      input->probe_quality < 50 &&
      input->aggregate_metric >= 220)
    {
      *study_eligible = 0;
      return;
    }
  if (input->auxiliary_count >= 3 &&
      input->candidate_metric >= 2 &&
      input->probe_quality < 50 &&
      record->geometry_count <= 7 &&
      record->agreement <= 202 &&
      record->study_metric_20 < 200 &&
      scaled_coverage < 17 &&
      record->normalized_coverage < 170)
    {
      *study_eligible = 0;
      return;
    }
  if (input->auxiliary_count >= 2 &&
      input->candidate_metric >= 3 &&
      input->aggregate_metric >= 150 &&
      record->agreement <= 200 &&
      record->study_metric_20 < 200 &&
      scaled_coverage < 17)
    {
      *study_eligible = 0;
      return;
    }
  if (input->auxiliary_count >= 2 &&
      input->probe_quality < 35 &&
      record->agreement < 208 &&
      record->study_metric_20 < 197 &&
      scaled_coverage < 20)
    {
      *study_eligible = 0;
      return;
    }

  weak_source =
    (input->auxiliary_count >= 5 && record->geometry_count <= 5) ||
    (input->auxiliary_count >= 4 &&
     record->geometry_count <= 7 &&
     record->geometry_percent < 25) ||
    (input->auxiliary_count >= 2 &&
     ((record->geometry_count <= 7 &&
       record->study_metric_20 < 215 &&
       record->study_metric_1c < 165) ||
      (record->geometry_count <= 10 &&
       record->study_metric_20 < 210 &&
       record->study_metric_1c < 155) ||
      (record->geometry_count <= 11 &&
       record->study_metric_20 < 192 &&
       record->study_metric_1c < 90)));
  if (!weak_source)
    return;

  /* Type 24 is one of the template types selected by the
   * 0x07010400 bitset. */
  *study_eligible = 0;
  if ((record->geometry_count < 6 &&
       record->study_metric_20 < 210 &&
       record->study_metric_1c < 135) ||
      (record->geometry_count <= 11 &&
       record->study_metric_20 < 192 &&
       record->study_metric_1c < 90))
    *status = 0;
}

/* Flag re-evaluation for type 24, profile flag 0 / study flag 1.
 *
 * The two rule lists below are the type-24 confidence rules and the
 * status/confidence rules, written out as decision trees over the record
 * fields.  t* are the record sums the rules compare. */

/* Confidence rules. */
static void
match_reeval_conf_tree_type24 (const GoodixChicagoMatchScoreRecord *rec, gint32 *conf)
{
  const gint32 t0 = rec->study_metric_20 - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a));
  const gint32 t1 = rec->agreement - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a));
  const gint32 t2 = rec->study_metric_20 - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a)) + (rec->agreement - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a)));
  const gint32 t3 = rec->gallery_quality + rec->probe_quality;
  const gint32 t4 = rec->study_metric_20 + rec->agreement + rec->selector;

  if (rec->secondary_geometry_count <= 11 && t0 < 180)
    *conf = 0;
  else if (rec->geometry_count < rec->secondary_geometry_count && rec->secondary_geometry_count <= 12 && t0 <= 187)
    *conf = 0;
  else if (*conf != 0 && rec->normalized_coverage < 30 && rec->geometry_count < 10)
    *conf = 0;
  if ((rec->secondary_geometry_count > 16 && (((rec->normalized_coverage >= 95 || rec->geometry_count > 9) && rec->secondary_geometry_count < 12 && rec->geometry_count < rec->secondary_geometry_count && t2 <= 410) || (rec->normalized_coverage < 95 && rec->geometry_count <= 9 && (t2 < 407 || (rec->secondary_geometry_count < 12 && rec->geometry_count < rec->secondary_geometry_count && t2 <= 410))))) || (rec->secondary_geometry_count <= 16 && rec->normalized_coverage >= 125 && (((rec->normalized_coverage >= 95 || rec->geometry_count > 9) && rec->secondary_geometry_count < 12 && rec->geometry_count < rec->secondary_geometry_count && t2 <= 410) || (rec->normalized_coverage < 95 && rec->geometry_count <= 9 && (t2 < 407 || (rec->secondary_geometry_count < 12 && rec->geometry_count < rec->secondary_geometry_count && t2 <= 410))))))
    {
      *conf = 0;
    }
  else if (*conf != 0)
    {
      const gboolean b1_0 = rec->secondary_geometry_count <= 17 && t1 <= 205 && t0 <= 170 && rec->geometry_percent <= 18;
      const gboolean b1_1 = rec->geometry_percent <= 21 || b1_0;
      const gboolean b1_2 = ((rec->geometry_count > 13 || rec->secondary_geometry_count > 16 || t1 > 214 || rec->matched_percent > 50) && b1_0) || (rec->geometry_count <= 13 && rec->secondary_geometry_count <= 16 && t1 <= 214 && rec->matched_percent <= 50 && b1_1);
      const gboolean b1_3 = t0 <= 170 || b1_2;
      const gboolean b1_4 = ((rec->geometry_count > 16 || t1 > 200) && b1_2) || (rec->geometry_count <= 16 && t1 <= 200 && b1_3);
      const gboolean b1_5 = t1 <= 205 || b1_4;
      const gboolean b1_6 = (rec->secondary_geometry_count > 14 && b1_4) || (rec->secondary_geometry_count <= 14 && b1_5);
      if (rec->selector == 128 && ((rec->geometry_count >= 7 && b1_6) || (rec->geometry_count < 7 && (rec->geometry_percent < 10 || b1_6))))
        *conf = 0;
      if (rec->geometry_count <= 3 && rec->secondary_geometry_count <= 7 && rec->normalized_coverage <= 60 && rec->matched_percent <= 50)
        {
          *conf = 0;
        }
      else if (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 11 && rec->normalized_coverage <= 110 && rec->geometry_percent <= 24 && t2 <= 427 && rec->selector <= 234)
        {
          *conf = 0;
        }
      else if (rec->geometry_count <= 3 && rec->secondary_geometry_count <= 10 && rec->normalized_coverage <= 56 && rec->geometry_percent <= 45 && t2 <= 438 && rec->selector <= 225)
        {
          *conf = 0;
        }
      else if (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 6 && rec->normalized_coverage <= 61 && rec->geometry_percent <= 28 && t2 <= 448 && rec->selector <= 226)
        {
          *conf = 0;
        }
      else
        {
          const gboolean b2_0 = t4 <= 664;
          const gboolean b2_1 = rec->geometry_count <= 6 && rec->secondary_geometry_count <= 8 && rec->normalized_coverage <= 50 && rec->matched_percent <= 45 && b2_0;
          const gboolean b2_2 = rec->selector <= 233 || b2_1;
          const gboolean b2_3 = ((rec->secondary_geometry_count > 13 || rec->geometry_percent > 21 || t1 > 212) && b2_1) || (rec->secondary_geometry_count <= 13 && rec->geometry_percent <= 21 && t1 <= 212 && b2_2);
          const gboolean b2_4 = rec->selector <= 234 || b2_3;
          const gboolean b2_5 = ((rec->secondary_geometry_count > 16 || rec->geometry_percent > 15 || t1 > 205) && b2_3) || (rec->secondary_geometry_count <= 16 && rec->geometry_percent <= 15 && t1 <= 205 && b2_4);
          const gboolean b2_6 = t4 <= 655 || b2_5;
          const gboolean b2_7 = ((rec->secondary_geometry_count > 9 || rec->normalized_coverage > 55 || rec->matched_percent > 62) && b2_5) || (rec->secondary_geometry_count <= 9 && rec->normalized_coverage <= 55 && rec->matched_percent <= 62 && b2_6);
          if ((rec->geometry_count > 5 && b2_1) || (rec->geometry_count <= 5 && (((rec->secondary_geometry_count > 7 || rec->normalized_coverage > 52 || rec->geometry_percent > 35) && b2_7) || (rec->secondary_geometry_count <= 7 && rec->normalized_coverage <= 52 && rec->geometry_percent <= 35 && (t4 <= 665 || b2_7)))))
            *conf = 0;
          else if (rec->geometry_count <= 8 && rec->secondary_geometry_count <= 10 && rec->selector <= 236 && t1 <= 224 && rec->normalized_coverage <= 125 && rec->geometry_percent <= 18)
            *conf = 0;
          else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 8 && rec->selector <= 223 && t1 <= 221 && rec->normalized_coverage <= 64 && rec->geometry_percent <= 42)
            *conf = 0;
          else if (rec->geometry_count <= 5 && (((rec->secondary_geometry_count > 10 || rec->selector > 230 || t1 > 220 || rec->normalized_coverage > 91) && rec->secondary_geometry_count <= 6 && rec->selector <= 227 && t1 <= 227 && rec->normalized_coverage <= 87 && rec->geometry_percent <= 15) || (rec->secondary_geometry_count <= 10 && rec->selector <= 230 && t1 <= 220 && rec->normalized_coverage <= 91 && (rec->geometry_percent <= 20 || (rec->secondary_geometry_count <= 6 && rec->selector <= 227 && t1 <= 227 && rec->normalized_coverage <= 87 && rec->geometry_percent <= 15)))))
            *conf = 0;
        }
      if (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 13 && rec->normalized_coverage <= 111 && rec->geometry_percent <= 23 && t1 <= 213 && t0 <= 192)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 12 && rec->secondary_geometry_count <= 18 && t1 <= 200 && t0 <= 196 && rec->normalized_coverage <= 98 && rec->geometry_percent <= 42)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 10 && (((t1 > 215 || t0 > 198 || rec->normalized_coverage > 110) && t1 <= 205 && t0 <= 196 && rec->normalized_coverage <= 112 && rec->geometry_percent <= 24) || (t1 <= 215 && t0 <= 198 && rec->normalized_coverage <= 110 && (rec->geometry_percent <= 12 || (t1 <= 205 && t0 <= 196 && rec->normalized_coverage <= 112 && rec->geometry_percent <= 24)))))
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->secondary_geometry_count <= 14 && rec->selector <= 224 && t1 <= 211 && t0 <= 197 && rec->normalized_coverage <= 80 && rec->geometry_percent <= 33)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 12 && t1 <= 204 && t0 <= 189 && rec->normalized_coverage <= 124 && rec->geometry_percent <= 22)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 11 && (((rec->secondary_geometry_count > 16 || t1 > 212 || t0 > 178 || rec->normalized_coverage > 116) && rec->secondary_geometry_count <= 18 && t1 <= 196 && t0 <= 170 && rec->normalized_coverage <= 127 && rec->geometry_percent <= 24) || (rec->secondary_geometry_count <= 16 && t1 <= 212 && t0 <= 178 && rec->normalized_coverage <= 116 && (rec->geometry_percent <= 24 || (rec->secondary_geometry_count <= 18 && t1 <= 196 && t0 <= 170 && rec->normalized_coverage <= 127 && rec->geometry_percent <= 24)))))
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 13 && rec->secondary_geometry_count <= 16 && (((t1 > 194 || t0 > 188 || rec->normalized_coverage > 86) && t1 <= 196 && t0 <= 167 && rec->normalized_coverage <= 95 && rec->geometry_percent <= 31) || (t1 <= 194 && t0 <= 188 && rec->normalized_coverage <= 86 && (rec->geometry_percent <= 43 || (t1 <= 196 && t0 <= 167 && rec->normalized_coverage <= 95 && rec->geometry_percent <= 31)))))
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 14 && rec->secondary_geometry_count <= 16 && t1 <= 203 && t0 <= 152 && rec->normalized_coverage <= 93 && rec->geometry_percent <= 30)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count <= 13 && rec->secondary_geometry_count <= 15 && t1 <= 205 && t0 <= 187 && rec->normalized_coverage <= 164 && rec->geometry_percent <= 12)
        {
          *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->geometry_count > 9)
        {
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      else if (rec->secondary_geometry_count <= 10)
        {
          if (t1 <= 215 && t0 <= 199 && rec->normalized_coverage <= 110 && rec->geometry_percent <= 18)
            *conf = 0;
          if (rec->secondary_geometry_count <= 10 && (((t1 > 209 || t0 > 190) && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11) || (t1 <= 209 && t0 <= 190 && (rec->geometry_percent <= 14 || (t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 11)))))
            *conf = 0;
        }
      if (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 11 && (((t1 > 224 || rec->normalized_coverage > 117) && t1 <= 217 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 12) || (t1 <= 224 && rec->normalized_coverage <= 117 && (rec->geometry_percent <= 20 || (t1 <= 217 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 12)))))
        *conf = 0;
      else if (rec->geometry_count <= 5 && rec->secondary_geometry_count <= 11 && t1 <= 218 && rec->normalized_coverage <= 120 && rec->geometry_percent <= 16)
        *conf = 0;
      else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 12 && t2 <= 408 && rec->selector <= 231 && t1 <= 212 && rec->matched_percent <= 48 && rec->geometry_percent <= 19)
        *conf = 0;
      else if (rec->geometry_count <= 6 && rec->secondary_geometry_count <= 11 && t2 <= 422 && rec->selector <= 231 && t1 <= 214 && rec->matched_percent <= 39 && rec->geometry_percent <= 19)
        *conf = 0;
      else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 11 && t2 <= 429 && rec->selector <= 227 && t1 <= 218 && rec->normalized_coverage <= 81 && rec->geometry_percent <= 32)
        *conf = 0;
      else if (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 12 && t2 <= 426 && rec->selector <= 228 && t1 <= 214 && rec->normalized_coverage <= 95 && rec->matched_percent <= 47 && rec->geometry_percent <= 35)
        *conf = 0;
      else if (rec->geometry_count <= 6 && rec->secondary_geometry_count <= 11 && t2 <= 429 && rec->selector <= 230 && t1 <= 218 && rec->normalized_coverage <= 100 && rec->geometry_percent <= 28)
        *conf = 0;
      else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 11 && rec->selector <= 230 && t1 <= 220 && rec->normalized_coverage <= 119 && rec->geometry_percent <= 15)
        *conf = 0;
      else if (rec->geometry_count <= 6 && rec->secondary_geometry_count <= 11 && rec->selector <= 224 && t1 <= 223 && t0 <= 211 && rec->matched_percent <= 51)
        *conf = 0;
      else if (rec->geometry_count <= 10 && (((rec->secondary_geometry_count > 14 || t1 > 210 || t0 > 186) && rec->secondary_geometry_count <= 15 && t1 <= 210 && rec->matched_percent <= 60 && t2 <= 390) || (rec->secondary_geometry_count <= 14 && t1 <= 210 && t0 <= 186 && (rec->normalized_coverage <= 111 || (rec->secondary_geometry_count <= 15 && t1 <= 210 && rec->matched_percent <= 60 && t2 <= 390)))))
        *conf = 0;
      else if (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 15 && t1 <= 208 && rec->matched_percent <= 50 && t0 <= 205 && rec->geometry_percent <= 30)
        *conf = 0;
      else if (rec->geometry_count <= 10 && t1 <= 201 && t2 <= 405 && rec->selector <= 224 && rec->normalized_coverage <= 90 && rec->geometry_percent <= 40)
        *conf = 0;
      else if (rec->geometry_count <= 8 && rec->secondary_geometry_count <= 13 && t1 <= 210 && t0 <= 205 && rec->normalized_coverage <= 118 && rec->geometry_percent <= 22)
        *conf = 0;
      else if (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 12 && t1 <= 212 && t0 <= 205 && rec->normalized_coverage <= 80 && rec->geometry_percent <= 30)
        *conf = 0;
      if (rec->geometry_count <= 7 && rec->selector <= 233 && t1 <= 210 && rec->normalized_coverage <= 129 && rec->geometry_percent <= 19)
        *conf = 0;
      else if (rec->geometry_count <= 9 && t1 <= 200 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 27)
        *conf = 0;
      else if ((rec->geometry_count > 11 && rec->secondary_geometry_count <= 13 && t1 <= 209 && t0 <= 187 && rec->normalized_coverage <= 100) || (rec->geometry_count <= 11 && (((t1 > 209 || t0 > 165) && (((t1 > 196 || t0 > 177 || rec->normalized_coverage > 112) && rec->secondary_geometry_count <= 13 && t1 <= 209 && t0 <= 187 && rec->normalized_coverage <= 100) || (t1 <= 196 && t0 <= 177 && rec->normalized_coverage <= 112 && (rec->geometry_percent <= 27 || (rec->secondary_geometry_count <= 13 && t1 <= 209 && t0 <= 187 && rec->normalized_coverage <= 100))))) || (t1 <= 209 && t0 <= 165 && (rec->normalized_coverage <= 75 || ((t1 > 196 || t0 > 177 || rec->normalized_coverage > 112) && rec->secondary_geometry_count <= 13 && t1 <= 209 && t0 <= 187 && rec->normalized_coverage <= 100) || (t1 <= 196 && t0 <= 177 && rec->normalized_coverage <= 112 && (rec->geometry_percent <= 27 || (rec->secondary_geometry_count <= 13 && t1 <= 209 && t0 <= 187 && rec->normalized_coverage <= 100))))))))
        *conf = 0;
      else if (rec->geometry_count <= 6 && rec->selector <= 232 && t1 <= 219 && rec->normalized_coverage <= 111 && rec->geometry_percent <= 23)
        *conf = 0;
      else if (rec->geometry_count <= 7 && t1 <= 217 && t0 <= 199 && rec->normalized_coverage <= 125 && rec->geometry_percent <= 20)
        *conf = 0;
      else if (rec->geometry_count <= 12 && t1 <= 205 && t0 <= 197 && rec->normalized_coverage <= 101 && rec->geometry_percent <= 25)
        *conf = 0;
      else if (rec->secondary_geometry_count <= 15 && t1 <= 200 && rec->normalized_coverage <= 110 && rec->geometry_percent <= 26)
        *conf = 0;
      else if (rec->secondary_geometry_count <= 14 && t1 <= 207 && t0 <= 201 && rec->normalized_coverage <= 100 && rec->geometry_percent <= 25)
        *conf = 0;
      else if (rec->geometry_count <= 6 && rec->selector <= 223 && t1 <= 223 && rec->normalized_coverage <= 53 && rec->geometry_percent <= 39)
        *conf = 0;
      else if (rec->geometry_count <= 7 && rec->selector <= 224 && t1 <= 219 && t0 <= 213 && rec->normalized_coverage <= 74 && rec->geometry_percent < 27)
        *conf = 0;
      else if (((rec->geometry_count > 6 || rec->selector > 226 || t1 > 222 || t0 > 225 || rec->normalized_coverage > 68) && rec->geometry_count <= 8 && t1 <= 209 && rec->normalized_coverage <= 105 && rec->geometry_percent <= 25) || (rec->geometry_count <= 6 && rec->selector <= 226 && t1 <= 222 && t0 <= 225 && rec->normalized_coverage <= 68 && (rec->geometry_percent < 30 || (rec->geometry_count <= 8 && t1 <= 209 && rec->normalized_coverage <= 105 && rec->geometry_percent <= 25))))
        *conf = 0;
      if (rec->secondary_geometry_count <= 14 && t0 <= 168 && rec->normalized_coverage <= 80)
        *conf = 0;
      else if (rec->secondary_geometry_count <= 13 && rec->selector <= 222 && t1 <= 209 && t0 <= 189 && rec->normalized_coverage <= 85)
        *conf = 0;
      if (rec->geometry_count < rec->secondary_geometry_count)
        {
          if (((rec->geometry_count > 9 || rec->secondary_geometry_count > 15 || rec->selector > 228 || t1 > 211 || rec->normalized_coverage > 121) && rec->geometry_count <= 10 && t1 <= 209 && t0 <= 190 && rec->normalized_coverage <= 100 && rec->geometry_percent <= 21) || (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 15 && rec->selector <= 228 && t1 <= 211 && rec->normalized_coverage <= 121 && (rec->geometry_percent <= 18 || (rec->geometry_count <= 10 && t1 <= 209 && t0 <= 190 && rec->normalized_coverage <= 100 && rec->geometry_percent <= 21))))
            *conf = 0;
          else if (rec->geometry_count <= 11 && (((rec->selector > 225 || t1 > 206 || t0 > 196 || rec->normalized_coverage > 100) && t1 <= 201 && t0 <= 180 && rec->normalized_coverage <= 95 && rec->matched_percent <= 45) || (rec->selector <= 225 && t1 <= 206 && t0 <= 196 && rec->normalized_coverage <= 100 && (rec->geometry_percent <= 23 || (t1 <= 201 && t0 <= 180 && rec->normalized_coverage <= 95 && rec->matched_percent <= 45)))))
            *conf = 0;
          else if (rec->geometry_count <= 13 && rec->secondary_geometry_count <= 17 && t1 <= 208 && t0 <= 201 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 20)
            *conf = 0;
          else if ((FALSE /* never: geometry_count > 9 && geometry_count <= 8 */ && t1 <= 203 && t0 <= 195 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 21) || (rec->geometry_count <= 9 && (((rec->secondary_geometry_count > 13 || t1 > 212 || t0 > 199 || rec->normalized_coverage > 121) && (((rec->secondary_geometry_count > 14 || t1 > 217 || t0 > 195 || rec->normalized_coverage > 78) && rec->geometry_count <= 8 && t1 <= 203 && t0 <= 195 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 21) || (rec->secondary_geometry_count <= 14 && t1 <= 217 && t0 <= 195 && rec->normalized_coverage <= 78 && (rec->geometry_percent <= 19 || (rec->geometry_count <= 8 && t1 <= 203 && t0 <= 195 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 21))))) || (rec->secondary_geometry_count <= 13 && t1 <= 212 && t0 <= 199 && rec->normalized_coverage <= 121 && (rec->geometry_percent <= 18 || ((rec->secondary_geometry_count > 14 || t1 > 217 || t0 > 195 || rec->normalized_coverage > 78) && rec->geometry_count <= 8 && t1 <= 203 && t0 <= 195 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 21) || (rec->secondary_geometry_count <= 14 && t1 <= 217 && t0 <= 195 && rec->normalized_coverage <= 78 && (rec->geometry_percent <= 19 || (rec->geometry_count <= 8 && t1 <= 203 && t0 <= 195 && rec->normalized_coverage <= 135 && rec->geometry_percent <= 21))))))))
            *conf = 0;
          else if (rec->geometry_count <= 10 && rec->secondary_geometry_count <= 16 && t1 <= 205 && t0 <= 200 && rec->normalized_coverage <= 106 && rec->geometry_percent <= 26)
            *conf = 0;
          else if (rec->geometry_count <= 12 && rec->secondary_geometry_count <= 17 && rec->selector <= 228 && t1 <= 208 && t0 <= 191 && rec->normalized_coverage <= 128 && rec->geometry_percent <= 25)
            *conf = 0;
          else if (rec->geometry_count <= 10 && t1 <= 209 && t0 <= 195 && rec->normalized_coverage <= 117 && rec->matched_percent <= 60 && rec->geometry_percent <= 21)
            *conf = 0;
          else if (rec->geometry_count <= 12 && t1 <= 202 && t0 <= 180 && rec->matched_percent <= 45 && rec->geometry_percent <= 18)
            *conf = 0;
          else if (rec->geometry_count <= 11 && t1 <= 204 && t0 <= 204 && rec->normalized_coverage <= 85 && rec->geometry_percent <= 30)
            *conf = 0;
          else if (rec->geometry_count <= 10 && t1 <= 207 && t0 <= 198 && rec->normalized_coverage <= 105 && rec->geometry_percent <= 26)
            *conf = 0;
          if (t2 <= 388 && t1 <= 201 && ((rec->secondary_geometry_count > 16 && ((rec->geometry_count > 11 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->geometry_count <= 11 && (((rec->normalized_coverage > 100 || rec->geometry_percent > 35) && ((rec->matched_percent > 40 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->matched_percent <= 40 && (rec->geometry_percent <= 17 || (rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15))))) || (rec->normalized_coverage <= 100 && rec->geometry_percent <= 35 && (rec->selector <= 217 || (rec->matched_percent > 40 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->matched_percent <= 40 && (rec->geometry_percent <= 17 || (rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15))))))))) || (rec->secondary_geometry_count <= 16 && (rec->geometry_percent <= 15 || (rec->geometry_count > 11 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->geometry_count <= 11 && (((rec->normalized_coverage > 100 || rec->geometry_percent > 35) && ((rec->matched_percent > 40 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->matched_percent <= 40 && (rec->geometry_percent <= 17 || (rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15))))) || (rec->normalized_coverage <= 100 && rec->geometry_percent <= 35 && (rec->selector <= 217 || (rec->matched_percent > 40 && rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15) || (rec->matched_percent <= 40 && (rec->geometry_percent <= 17 || (rec->geometry_count <= 13 && t1 <= 205 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 15)))))))))))
            *conf = 0;
        }
      if (rec->geometry_count <= 4 && t0 <= 202 && rec->normalized_coverage <= 107 && rec->geometry_percent <= 20)
        *conf = 0;
      else if (rec->geometry_count <= 5 && rec->secondary_geometry_count <= 8 && t1 <= 211 && rec->normalized_coverage <= 65 && rec->geometry_percent <= 32)
        *conf = 0;
      else if (rec->geometry_count <= 9 && rec->selector <= 231 && t1 <= 206 && rec->normalized_coverage <= 114 && rec->geometry_percent <= 28)
        *conf = 0;
      else if (rec->geometry_count <= 8 && rec->secondary_geometry_count <= 15 && t1 <= 211 && t0 <= 182 && rec->normalized_coverage <= 120 && rec->geometry_percent <= 22)
        *conf = 0;
      else if (rec->secondary_geometry_count <= 14 && t1 <= 209 && t0 <= 190 && rec->normalized_coverage <= 145 && rec->matched_percent <= 40 && rec->geometry_percent <= 20)
        *conf = 0;
      else if (rec->secondary_geometry_count <= 15 && t1 <= 201 && t0 <= 191 && rec->normalized_coverage <= 106 && rec->matched_percent <= 44 && rec->geometry_percent <= 20)
        *conf = 0;
      else if (rec->geometry_count <= 11 && (((rec->secondary_geometry_count > 13 || t1 > 215 || t0 > 185 || rec->normalized_coverage > 90) && t1 <= 207 && t0 <= 185 && rec->normalized_coverage <= 95 && rec->geometry_percent <= 30) || (rec->secondary_geometry_count <= 13 && t1 <= 215 && t0 <= 185 && rec->normalized_coverage <= 90 && (rec->geometry_percent <= 30 || (t1 <= 207 && t0 <= 185 && rec->normalized_coverage <= 95 && rec->geometry_percent <= 30)))))
        *conf = 0;
      if (rec->geometry_count < rec->secondary_geometry_count && rec->selector == 128 && (((rec->geometry_count > 12 || t0 > 165) && rec->secondary_geometry_count <= 14 && t2 <= 400 && rec->geometry_percent <= 10) || (rec->geometry_count <= 12 && t0 <= 165 && (rec->geometry_percent <= 10 || (rec->secondary_geometry_count <= 14 && t2 <= 400 && rec->geometry_percent <= 10)))))
        {
          *conf = 0;
        }
      else if (*conf != 0)
        {
          if (rec->normalized_coverage < 52 && rec->secondary_geometry_count <= 10 && t2 <= 446)
            *conf = 0;
          else if (rec->normalized_coverage <= 55 && rec->secondary_geometry_count <= 16 && t2 < 433)
            *conf = 0;
          else if (rec->normalized_coverage <= 50 && rec->secondary_geometry_count <= 19 && t2 <= 410)
            *conf = 0;
          else if (rec->normalized_coverage <= 56 && rec->secondary_geometry_count <= 22 && t2 < 429)
            *conf = 0;
          else if (rec->normalized_coverage < 65 && rec->secondary_geometry_count <= 6 && t2 < 445)
            *conf = 0;
          else if (rec->normalized_coverage < 77 && rec->secondary_geometry_count <= 16 && t2 < 429)
            *conf = 0;
          else if (rec->normalized_coverage < 105 && rec->geometry_count <= 8 && t2 <= 436 && t1 < 218)
            *conf = 0;
          else if (rec->normalized_coverage < 110 && rec->geometry_count <= 8 && t2 <= 403 && t1 < 205)
            *conf = 0;
          else if (rec->normalized_coverage < 128 && rec->geometry_count <= 7 && t2 <= 410)
            *conf = 0;
          else if (rec->geometry_count < rec->secondary_geometry_count && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 12 && t1 <= 215 && rec->matched_percent <= 65 && t2 <= 420)
            *conf = 0;
        }
    }
  if (*conf != 0 && rec->selector == 128 && t2 < 360)
    *conf = 0;
  if (*conf != 0 && rec->geometry_count <= rec->secondary_geometry_count && rec->geometry_count <= 7 && t1 <= 218 && rec->matched_percent <= 65 && t2 <= 421)
    *conf = 0;
  if (rec->selector == 128 && t2 < 390 && rec->secondary_geometry_count < 18)
    *conf = 0;
  if (t2 < 395 && rec->secondary_geometry_count < 13 && rec->geometry_percent < 20 && rec->matched_percent < 40)
    *conf = 0;
  if (t2 < 400 && rec->secondary_geometry_count < 10 && rec->geometry_percent < 20 && rec->matched_percent < 60)
    {
      *conf = 0;
    }
  else if (*conf != 0)
    {
      if (rec->normalized_coverage < 65 && t2 < 410 && rec->geometry_count <= 14 && t1 < 211)
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 90 && t2 < 420 && rec->geometry_count < 10 && rec->geometry_percent < 45)
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 77 && t2 < 419 && rec->geometry_count < 12 && rec->geometry_percent < 42)
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 80 && (((t2 > 435 || rec->geometry_count >= 11) && t2 < 395 && rec->geometry_count < 12 && rec->geometry_percent < 48) || (t2 <= 435 && rec->geometry_count < 11 && (rec->geometry_percent < 35 || (t2 < 395 && rec->geometry_count < 12 && rec->geometry_percent < 48)))))
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 100 && (((t2 >= 430 || rec->geometry_count > 12) && t2 < 432 && rec->geometry_count <= 12 && rec->geometry_percent < 20) || (t2 < 430 && rec->geometry_count <= 12 && (rec->geometry_percent < 32 || (t2 < 432 && rec->geometry_count <= 12 && rec->geometry_percent < 20)))))
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 93 && t2 < 419 && rec->geometry_count <= 12 && rec->geometry_percent <= 35)
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage <= 90 && t2 < 400 && rec->geometry_count <= 13 && rec->geometry_percent <= 30)
        {
          *conf = 0;
        }
      else if (rec->normalized_coverage < 67 && t2 < 440 && rec->geometry_count < 10 && rec->geometry_percent < 50)
        {
          *conf = 0;
        }
      else
        {
          const gboolean b3_0 = rec->geometry_percent < 20;
          const gboolean b3_1 = rec->normalized_coverage < 128 && t2 < 415 && rec->secondary_geometry_count < 10 && b3_0;
          const gboolean b3_2 = rec->geometry_percent < 35 || b3_1;
          const gboolean b3_3 = ((rec->normalized_coverage >= 110 || t2 >= 395 || rec->secondary_geometry_count >= 15) && b3_1) || (rec->normalized_coverage < 110 && t2 < 395 && rec->secondary_geometry_count < 15 && b3_2);
          const gboolean b3_4 = rec->geometry_percent < 25 || b3_3;
          const gboolean b3_5 = ((t2 >= 405 || rec->secondary_geometry_count >= 13) && b3_3) || (t2 < 405 && rec->secondary_geometry_count < 13 && b3_4);
          const gboolean b3_6 = rec->geometry_percent < 20 || b3_5;
          const gboolean b3_7 = ((t2 >= 400 || rec->secondary_geometry_count >= 16) && b3_5) || (t2 < 400 && rec->secondary_geometry_count < 16 && b3_6);
          const gboolean b3_8 = rec->geometry_percent < 30 || b3_7;
          const gboolean b3_9 = ((t2 >= 385 || rec->secondary_geometry_count >= 15) && b3_7) || (t2 < 385 && rec->secondary_geometry_count < 15 && b3_8);
          if ((rec->normalized_coverage >= 128 && b3_3) || (rec->normalized_coverage < 128 && (((t2 >= 400 || rec->secondary_geometry_count >= 15) && b3_9) || (t2 < 400 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b3_9)))))
            *conf = 0;
          else if (rec->normalized_coverage < 65 && t2 < 420 && rec->geometry_count <= 12 && t1 <= 211 && rec->geometry_percent < 40)
            *conf = 0;
        }
      if (t3 > 120 && (((rec->normalized_coverage >= 90 || rec->geometry_count >= 11 || rec->secondary_geometry_count >= 21 || t2 >= 395) && rec->normalized_coverage < 80 && rec->geometry_count < 12 && rec->secondary_geometry_count < 20 && t2 < 400 && rec->geometry_percent < 45) || (rec->normalized_coverage < 90 && rec->geometry_count < 11 && rec->secondary_geometry_count < 21 && t2 < 395 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && rec->geometry_count < 12 && rec->secondary_geometry_count < 20 && t2 < 400 && rec->geometry_percent < 45)))))
        *conf = 0;
      if (t3 > 130 && (((rec->normalized_coverage >= 120 || rec->geometry_count >= 13 || rec->secondary_geometry_count >= 20 || t2 >= 408) && rec->normalized_coverage < 80 && rec->geometry_count < 11 && rec->secondary_geometry_count < 15 && t2 < 408 && rec->geometry_percent < 35) || (rec->normalized_coverage < 120 && rec->geometry_count < 13 && rec->secondary_geometry_count < 20 && t2 < 408 && (rec->geometry_percent < 25 || (rec->normalized_coverage < 80 && rec->geometry_count < 11 && rec->secondary_geometry_count < 15 && t2 < 408 && rec->geometry_percent < 35)))))
        *conf = 0;
      const gboolean b4_0 = rec->normalized_coverage < 55 && rec->geometry_count < 12 && rec->secondary_geometry_count < 17 && t2 < 425 && rec->geometry_percent < 45;
      const gboolean b4_1 = rec->geometry_percent < 45 || b4_0;
      const gboolean b4_2 = ((rec->normalized_coverage >= 70 || rec->geometry_count >= 16 || rec->secondary_geometry_count >= 20 || t2 >= 405) && b4_0) || (rec->normalized_coverage < 70 && rec->geometry_count < 16 && rec->secondary_geometry_count < 20 && t2 < 405 && b4_1);
      const gboolean b4_3 = rec->geometry_percent < 35 || b4_2;
      const gboolean b4_4 = ((rec->normalized_coverage >= 80 || rec->geometry_count >= 12 || rec->secondary_geometry_count >= 16 || t2 >= 415) && b4_2) || (rec->normalized_coverage < 80 && rec->geometry_count < 12 && rec->secondary_geometry_count < 16 && t2 < 415 && b4_3);
      const gboolean b4_5 = rec->geometry_percent < 16 || b4_4;
      const gboolean b4_6 = ((rec->geometry_count >= 14 || rec->secondary_geometry_count >= 16 || t2 >= 415) && b4_4) || (rec->geometry_count < 14 && rec->secondary_geometry_count < 16 && t2 < 415 && b4_5);
      const gboolean b4_7 = ((rec->geometry_count >= 16 || rec->secondary_geometry_count >= 24 || t2 >= 405) && b4_6) || (rec->geometry_count < 16 && rec->secondary_geometry_count < 24 && t2 < 405 && (rec->geometry_percent < 40 || b4_6));
      const gboolean b4_8 = (rec->normalized_coverage >= 90 && b4_4) || (rec->normalized_coverage < 90 && b4_7);
      if (t3 > 145 && (((rec->normalized_coverage >= 128 || rec->geometry_count >= 19 || rec->secondary_geometry_count >= 20 || t2 >= 406) && b4_8) || (rec->normalized_coverage < 128 && rec->geometry_count < 19 && rec->secondary_geometry_count < 20 && t2 < 406 && (rec->geometry_percent < 35 || b4_8))))
        *conf = 0;
      if (t3 > 155 && rec->normalized_coverage <= 50 && rec->geometry_count < 12 && rec->secondary_geometry_count < 15 && t2 < 437 && rec->geometry_percent < 46 && rec->matched_percent < 71 && t1 < 224 && rec->selector < 222)
        *conf = 0;
      if (t3 > 160 && (((rec->normalized_coverage >= 50 || rec->secondary_geometry_count >= 10) && ((FALSE /* never: normalized_coverage >= 85 && normalized_coverage < 80 */ && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->normalized_coverage < 85 && (((rec->geometry_count >= 9 || rec->secondary_geometry_count >= 13) && (((rec->geometry_count >= 13 || rec->secondary_geometry_count >= 21 || t2 >= 415) && rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->geometry_count < 13 && rec->secondary_geometry_count < 21 && t2 < 415 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40))))) || (rec->geometry_count < 9 && rec->secondary_geometry_count < 13 && (t2 < 440 || ((rec->geometry_count >= 13 || rec->secondary_geometry_count >= 21 || t2 >= 415) && rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->geometry_count < 13 && rec->secondary_geometry_count < 21 && t2 < 415 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40))))))))) || (rec->normalized_coverage < 50 && rec->secondary_geometry_count < 10 && (t2 < 450 || (FALSE /* never: normalized_coverage >= 85 && normalized_coverage < 80 */ && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->normalized_coverage < 85 && (((rec->geometry_count >= 9 || rec->secondary_geometry_count >= 13) && (((rec->geometry_count >= 13 || rec->secondary_geometry_count >= 21 || t2 >= 415) && rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->geometry_count < 13 && rec->secondary_geometry_count < 21 && t2 < 415 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40))))) || (rec->geometry_count < 9 && rec->secondary_geometry_count < 13 && (t2 < 440 || ((rec->geometry_count >= 13 || rec->secondary_geometry_count >= 21 || t2 >= 415) && rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40) || (rec->geometry_count < 13 && rec->secondary_geometry_count < 21 && t2 < 415 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 418 && rec->geometry_percent < 40)))))))))))
        *conf = 0;
      const gboolean b5_0 = rec->normalized_coverage < 45 && rec->geometry_count < 6 && rec->secondary_geometry_count < 10 && t2 < 455 && rec->geometry_percent < 40;
      const gboolean b5_1 = t2 < 450 || b5_0;
      const gboolean b5_2 = ((rec->normalized_coverage >= 50 || rec->secondary_geometry_count >= 10) && b5_0) || (rec->normalized_coverage < 50 && rec->secondary_geometry_count < 10 && b5_1);
      const gboolean b5_3 = rec->geometry_percent < 35 || b5_2;
      const gboolean b5_4 = ((rec->normalized_coverage > 75 || rec->geometry_count >= 14 || rec->secondary_geometry_count >= 19 || t2 >= 446) && b5_2) || (rec->normalized_coverage <= 75 && rec->geometry_count < 14 && rec->secondary_geometry_count < 19 && t2 < 446 && b5_3);
      const gboolean b5_5 = t2 < 440 || b5_4;
      const gboolean b5_6 = ((rec->geometry_count >= 9 || rec->secondary_geometry_count >= 13) && b5_4) || (rec->geometry_count < 9 && rec->secondary_geometry_count < 13 && b5_5);
      const gboolean b5_7 = ((rec->geometry_count >= 14 || rec->secondary_geometry_count >= 18 || t2 >= 423) && b5_6) || (rec->geometry_count < 14 && rec->secondary_geometry_count < 18 && t2 < 423 && (rec->geometry_percent < 46 || b5_6));
      const gboolean b5_8 = (rec->normalized_coverage >= 85 && b5_4) || (rec->normalized_coverage < 85 && b5_7);
      const gboolean b5_9 = rec->geometry_percent < 25 || b5_8;
      const gboolean b5_10 = ((rec->normalized_coverage >= 90 || rec->geometry_count >= 7 || rec->secondary_geometry_count >= 10 || t2 >= 436) && b5_8) || (rec->normalized_coverage < 90 && rec->geometry_count < 7 && rec->secondary_geometry_count < 10 && t2 < 436 && b5_9);
      const gboolean b5_11 = rec->geometry_percent < 40 || b5_10;
      const gboolean b5_12 = ((rec->normalized_coverage >= 95 || rec->geometry_count >= 13 || rec->secondary_geometry_count >= 18 || t2 >= 415) && b5_10) || (rec->normalized_coverage < 95 && rec->geometry_count < 13 && rec->secondary_geometry_count < 18 && t2 < 415 && b5_11);
      const gboolean b5_13 = rec->geometry_percent < 46 || b5_12;
      const gboolean b5_14 = ((rec->normalized_coverage >= 100 || rec->geometry_count >= 14 || rec->secondary_geometry_count >= 23 || t2 >= 415) && b5_12) || (rec->normalized_coverage < 100 && rec->geometry_count < 14 && rec->secondary_geometry_count < 23 && t2 < 415 && b5_13);
      const gboolean b5_15 = rec->geometry_percent < 35 || b5_14;
      const gboolean b5_16 = ((rec->normalized_coverage >= 110 || rec->geometry_count >= 16 || rec->secondary_geometry_count >= 22 || t2 >= 408) && b5_14) || (rec->normalized_coverage < 110 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 408 && b5_15);
      const gboolean b5_17 = rec->geometry_percent < 20 || b5_16;
      const gboolean b5_18 = ((rec->secondary_geometry_count >= 14 || t2 >= 415) && b5_16) || (rec->secondary_geometry_count < 14 && t2 < 415 && b5_17);
      if (t3 > 165 && ((rec->normalized_coverage >= 140 && b5_16) || (rec->normalized_coverage < 140 && (((rec->geometry_count >= 13 || rec->secondary_geometry_count > 18 || t2 >= 425) && b5_18) || (rec->geometry_count < 13 && rec->secondary_geometry_count <= 18 && t2 < 425 && (rec->geometry_percent < 25 || b5_18))))))
        *conf = 0;
      const gboolean b6_0 = rec->normalized_coverage < 68 && rec->geometry_count < 10 && rec->secondary_geometry_count < 12 && t2 < 456 && rec->geometry_percent < 45;
      const gboolean b6_1 = rec->geometry_percent < 40 || b6_0;
      const gboolean b6_2 = ((rec->normalized_coverage >= 70 || rec->geometry_count >= 9 || rec->secondary_geometry_count >= 11 || t2 >= 448) && b6_0) || (rec->normalized_coverage < 70 && rec->geometry_count < 9 && rec->secondary_geometry_count < 11 && t2 < 448 && b6_1);
      const gboolean b6_3 = rec->geometry_percent < 35 || b6_2;
      const gboolean b6_4 = ((rec->normalized_coverage >= 75 || rec->geometry_count >= 8 || rec->secondary_geometry_count >= 11 || t2 >= 450) && b6_2) || (rec->normalized_coverage < 75 && rec->geometry_count < 8 && rec->secondary_geometry_count < 11 && t2 < 450 && b6_3);
      const gboolean b6_5 = rec->geometry_percent < 40 || b6_4;
      const gboolean b6_6 = ((rec->geometry_count >= 16 || rec->secondary_geometry_count >= 22 || t2 >= 405) && b6_4) || (rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 405 && b6_5);
      const gboolean b6_7 = ((rec->geometry_count >= 13 || rec->secondary_geometry_count >= 22 || t2 >= 420) && b6_6) || (rec->geometry_count < 13 && rec->secondary_geometry_count < 22 && t2 < 420 && (rec->geometry_percent < 51 || b6_6));
      const gboolean b6_8 = (rec->normalized_coverage >= 80 && b6_4) || (rec->normalized_coverage < 80 && b6_7);
      const gboolean b6_9 = rec->geometry_percent < 35 || b6_8;
      const gboolean b6_10 = ((rec->normalized_coverage >= 85 || rec->geometry_count >= 7 || rec->secondary_geometry_count >= 14 || t2 >= 451) && b6_8) || (rec->normalized_coverage < 85 && rec->geometry_count < 7 && rec->secondary_geometry_count < 14 && t2 < 451 && b6_9);
      const gboolean b6_11 = rec->geometry_percent < 40 || b6_10;
      const gboolean b6_12 = ((rec->normalized_coverage >= 100 || rec->geometry_count >= 12 || rec->secondary_geometry_count >= 21 || t2 >= 430) && b6_10) || (rec->normalized_coverage < 100 && rec->geometry_count < 12 && rec->secondary_geometry_count < 21 && t2 < 430 && b6_11);
      const gboolean b6_13 = rec->matched_percent <= 46 || b6_12;
      const gboolean b6_14 = ((rec->normalized_coverage >= 85 || rec->geometry_count > 12 || rec->secondary_geometry_count > 13 || t2 >= 443 || rec->geometry_percent >= 22) && b6_12) || (rec->normalized_coverage < 85 && rec->geometry_count <= 12 && rec->secondary_geometry_count <= 13 && t2 < 443 && rec->geometry_percent < 22 && b6_13);
      const gboolean b6_15 = rec->geometry_percent < 28 || b6_14;
      const gboolean b6_16 = ((rec->normalized_coverage >= 100 || rec->geometry_count >= 12 || rec->secondary_geometry_count >= 16 || t2 >= 441) && b6_14) || (rec->normalized_coverage < 100 && rec->geometry_count < 12 && rec->secondary_geometry_count < 16 && t2 < 441 && b6_15);
      const gboolean b6_17 = rec->geometry_percent < 30 || b6_16;
      const gboolean b6_18 = ((rec->normalized_coverage >= 105 || rec->geometry_count >= 14 || rec->secondary_geometry_count >= 20 || t2 >= 415) && b6_16) || (rec->normalized_coverage < 105 && rec->geometry_count < 14 && rec->secondary_geometry_count < 20 && t2 < 415 && b6_17);
      const gboolean b6_19 = rec->geometry_percent < 20 || b6_18;
      const gboolean b6_20 = ((rec->normalized_coverage >= 110 || rec->geometry_count >= 7 || rec->secondary_geometry_count >= 12 || t2 >= 448) && b6_18) || (rec->normalized_coverage < 110 && rec->geometry_count < 7 && rec->secondary_geometry_count < 12 && t2 < 448 && b6_19);
      const gboolean b6_21 = rec->geometry_percent < 35 || b6_20;
      const gboolean b6_22 = ((rec->normalized_coverage >= 115 || rec->geometry_count >= 13 || rec->secondary_geometry_count >= 23 || t2 >= 405) && b6_20) || (rec->normalized_coverage < 115 && rec->geometry_count < 13 && rec->secondary_geometry_count < 23 && t2 < 405 && b6_21);
      const gboolean b6_23 = rec->geometry_percent < 25 || b6_22;
      const gboolean b6_24 = ((rec->normalized_coverage >= 120 || rec->geometry_count >= 16 || rec->secondary_geometry_count >= 22 || t2 >= 395) && b6_22) || (rec->normalized_coverage < 120 && rec->geometry_count < 16 && rec->secondary_geometry_count < 22 && t2 < 395 && b6_23);
      const gboolean b6_25 = rec->geometry_percent < 40 || b6_24;
      const gboolean b6_26 = ((rec->normalized_coverage >= 125 || rec->geometry_count >= 14 || rec->secondary_geometry_count >= 21 || t2 >= 405) && b6_24) || (rec->normalized_coverage < 125 && rec->geometry_count < 14 && rec->secondary_geometry_count < 21 && t2 < 405 && b6_25);
      const gboolean b6_27 = rec->geometry_percent < 15 || b6_26;
      const gboolean b6_28 = ((rec->normalized_coverage >= 130 || rec->geometry_count >= 8 || rec->secondary_geometry_count >= 8 || t2 >= 435) && b6_26) || (rec->normalized_coverage < 130 && rec->geometry_count < 8 && rec->secondary_geometry_count < 8 && t2 < 435 && b6_27);
      if (t3 >= 180 && (((rec->normalized_coverage >= 140 || rec->geometry_count >= 18 || rec->secondary_geometry_count >= 20 || t2 >= 436) && b6_28) || (rec->normalized_coverage < 140 && rec->geometry_count < 18 && rec->secondary_geometry_count < 20 && t2 < 436 && (rec->geometry_percent < 25 || b6_28))))
        *conf = 0;
      const gboolean b7_0 = rec->normalized_coverage < 60 && rec->geometry_count < 13 && rec->secondary_geometry_count < 18 && t2 < 425 && rec->geometry_percent < 50;
      const gboolean b7_1 = rec->geometry_percent < 40 || b7_0;
      const gboolean b7_2 = ((rec->normalized_coverage >= 80 || rec->geometry_count >= 14 || rec->secondary_geometry_count >= 18 || t2 >= 435) && b7_0) || (rec->normalized_coverage < 80 && rec->geometry_count < 14 && rec->secondary_geometry_count < 18 && t2 < 435 && b7_1);
      const gboolean b7_3 = rec->geometry_percent < 50 || b7_2;
      const gboolean b7_4 = ((rec->normalized_coverage >= 85 || rec->geometry_count >= 12 || rec->secondary_geometry_count >= 16 || t2 >= 441) && b7_2) || (rec->normalized_coverage < 85 && rec->geometry_count < 12 && rec->secondary_geometry_count < 16 && t2 < 441 && b7_3);
      const gboolean b7_5 = rec->geometry_percent < 40 || b7_4;
      const gboolean b7_6 = ((rec->geometry_count >= 15 || rec->secondary_geometry_count >= 21 || t2 >= 400) && b7_4) || (rec->geometry_count < 15 && rec->secondary_geometry_count < 21 && t2 < 400 && b7_5);
      const gboolean b7_7 = ((rec->geometry_count >= 12 || rec->secondary_geometry_count >= 14 || t2 >= 426) && b7_6) || (rec->geometry_count < 12 && rec->secondary_geometry_count < 14 && t2 < 426 && (rec->geometry_percent < 30 || b7_6));
      const gboolean b7_8 = (rec->normalized_coverage >= 105 && b7_4) || (rec->normalized_coverage < 105 && b7_7);
      const gboolean b7_9 = rec->geometry_percent < 10 || b7_8;
      const gboolean b7_10 = ((rec->normalized_coverage >= 120 || rec->geometry_count >= 7 || rec->secondary_geometry_count >= 8 || t2 >= 440) && b7_8) || (rec->normalized_coverage < 120 && rec->geometry_count < 7 && rec->secondary_geometry_count < 8 && t2 < 440 && b7_9);
      if (t3 > 190 && (((rec->normalized_coverage >= 135 || rec->geometry_count >= 11 || rec->secondary_geometry_count >= 14 || t2 >= 435) && b7_10) || (rec->normalized_coverage < 135 && rec->geometry_count < 11 && rec->secondary_geometry_count < 14 && t2 < 435 && (rec->geometry_percent < 25 || b7_10))))
        *conf = 0;
      if (rec->normalized_coverage < 70 && ((rec->secondary_geometry_count >= 13 && rec->secondary_geometry_count < 18 && t2 < 416) || (rec->secondary_geometry_count < 13 && (t2 < 444 || (rec->secondary_geometry_count < 18 && t2 < 416)))))
        *conf = 0;
      else if (rec->normalized_coverage < 65 && rec->secondary_geometry_count < 19 && t2 < 408)
        *conf = 0;
      else if (((rec->normalized_coverage >= 140 || rec->secondary_geometry_count >= 13) && rec->normalized_coverage < 40) || (rec->normalized_coverage < 140 && rec->secondary_geometry_count < 13 && (t2 < 422 || rec->normalized_coverage < 40)))
        *conf = 0;
      else if (t2 < 390)
        *conf = 0;
    }
}

/* Status/confidence rules. */
static void
match_reeval_status_tree_type24 (const GoodixChicagoMatchScoreRecord *rec, gint32 probe_resolution, gint32 *conf, gint32 *status)
{
  const gint32 t0 = rec->study_metric_20 - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a)) + (rec->agreement - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a)));
  const gint32 t1 = rec->agreement - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a));
  const gint32 t2 = rec->study_metric_20 - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a));
  const gint32 t3 = rec->gallery_quality + rec->probe_quality;
  const gint32 t4 = rec->study_metric_20 - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a)) + (rec->agreement - (3 * (rec->reserved38[0] + rec->penalty_flag_b + rec->penalty_flag_a))) + rec->selector;

  if (*status == 0)
    {
      if ((rec->penalty_flag_b == 1 && ((FALSE /* never: t0 >= 390 && t0 < 370 */ && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count) || (t0 < 390 && (rec->secondary_geometry_count < 15 || (t0 < 370 && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count))))) || (rec->penalty_flag_b != 1 && rec->geometry_count <= 6 && ((FALSE /* never: t0 >= 390 && t0 < 370 */ && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count) || (t0 < 390 && (rec->secondary_geometry_count < 15 || (t0 < 370 && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count))))))
        {
          *status = 0;
          *conf = 0;
        }
      if (rec->selector != 128)
        {
          if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
            {
              *status = 0;
              *conf = 0;
            }
          if (*status != 0)
            {
              if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
            }
          if (*status == 0)
            {
              *status = 0;
            }
          else
            {
              if (rec->normalized_coverage < 35)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->geometry_percent < 8)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                {
                  const gboolean b1_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                  const gboolean b1_1 = rec->geometry_percent < 40 || b1_0;
                  const gboolean b1_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b1_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b1_1);
                  const gboolean b1_3 = rec->geometry_percent < 45 || b1_2;
                  const gboolean b1_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b1_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b1_3);
                  const gboolean b1_5 = rec->geometry_percent < 25 || b1_4;
                  const gboolean b1_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b1_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b1_5);
                  const gboolean b1_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b1_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b1_6));
                  const gboolean b1_8 = (rec->normalized_coverage >= 90 && b1_4) || (rec->normalized_coverage < 90 && b1_7);
                  const gboolean b1_9 = rec->geometry_percent < 35 || b1_8;
                  const gboolean b1_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b1_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b1_9);
                  if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b1_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b1_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b2_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                  const gboolean b2_1 = rec->geometry_percent < 60 || b2_0;
                  const gboolean b2_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b2_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b2_1);
                  const gboolean b2_3 = rec->geometry_percent < 35 || b2_2;
                  const gboolean b2_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b2_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b2_3);
                  const gboolean b2_5 = rec->geometry_percent < 22 || b2_4;
                  const gboolean b2_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b2_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b2_5);
                  const gboolean b2_7 = rec->geometry_percent < 15 || b2_6;
                  const gboolean b2_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b2_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b2_7);
                  const gboolean b2_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b2_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b2_8));
                  const gboolean b2_10 = (rec->normalized_coverage >= 95 && b2_6) || (rec->normalized_coverage < 95 && b2_9);
                  const gboolean b2_11 = rec->geometry_percent < 10 || b2_10;
                  const gboolean b2_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b2_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b2_11);
                  const gboolean b2_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b2_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b2_12));
                  const gboolean b2_14 = (rec->normalized_coverage >= 120 && b2_10) || (rec->normalized_coverage < 120 && b2_13);
                  if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b2_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b2_14))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b3_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                  const gboolean b3_1 = rec->geometry_percent < 28 || b3_0;
                  const gboolean b3_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b3_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b3_1);
                  const gboolean b3_3 = rec->geometry_percent < 50 || b3_2;
                  const gboolean b3_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b3_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b3_3);
                  const gboolean b3_5 = rec->geometry_percent < 30 || b3_4;
                  const gboolean b3_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b3_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b3_5);
                  const gboolean b3_7 = rec->geometry_percent < 25 || b3_6;
                  const gboolean b3_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b3_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b3_7);
                  const gboolean b3_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b3_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b3_8));
                  const gboolean b3_10 = (rec->normalized_coverage >= 128 && b3_6) || (rec->normalized_coverage < 128 && b3_9);
                  if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b3_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b3_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b4_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                  const gboolean b4_1 = rec->geometry_percent < 15 || b4_0;
                  const gboolean b4_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b4_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b4_1);
                  const gboolean b4_3 = rec->secondary_geometry_count < 12 || b4_2;
                  const gboolean b4_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b4_2) || (rec->normalized_coverage < 75 && t0 < 425 && b4_3);
                  const gboolean b4_5 = rec->secondary_geometry_count < 16 || b4_4;
                  const gboolean b4_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b4_4) || (rec->normalized_coverage < 90 && t0 < 405 && b4_5);
                  const gboolean b4_7 = rec->secondary_geometry_count < 21 || b4_6;
                  const gboolean b4_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b4_6) || (rec->normalized_coverage < 110 && t0 < 390 && b4_7);
                  const gboolean b4_9 = rec->secondary_geometry_count < 14 || b4_8;
                  const gboolean b4_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b4_8) || (rec->normalized_coverage < 130 && t0 < 415 && b4_9);
                  if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b4_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b4_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b5_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                  const gboolean b5_1 = rec->secondary_geometry_count < 15 || b5_0;
                  const gboolean b5_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b5_0) || (rec->normalized_coverage < 90 && t0 < 430 && b5_1);
                  const gboolean b5_3 = rec->secondary_geometry_count < 20 || b5_2;
                  const gboolean b5_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b5_2) || (rec->normalized_coverage < 95 && t0 < 405 && b5_3);
                  const gboolean b5_5 = rec->secondary_geometry_count < 22 || b5_4;
                  const gboolean b5_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b5_4) || (rec->normalized_coverage < 100 && t0 < 400 && b5_5);
                  const gboolean b5_7 = rec->secondary_geometry_count < 7 || b5_6;
                  const gboolean b5_8 = (t0 >= 435 && b5_6) || (t0 < 435 && b5_7);
                  const gboolean b5_9 = rec->secondary_geometry_count < 10 || b5_8;
                  const gboolean b5_10 = (t0 >= 430 && b5_8) || (t0 < 430 && b5_9);
                  const gboolean b5_11 = (t0 >= 420 && b5_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b5_10));
                  const gboolean b5_12 = (rec->normalized_coverage >= 120 && b5_6) || (rec->normalized_coverage < 120 && b5_11);
                  const gboolean b5_13 = rec->secondary_geometry_count < 12 || b5_12;
                  const gboolean b5_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b5_12) || (rec->normalized_coverage < 140 && t0 < 430 && b5_13);
                  if (t3 >= 159 && ((t0 >= 380 && b5_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b5_14))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else
                    {
                      const gboolean b6_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                      const gboolean b6_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b6_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b6_0));
                      const gboolean b6_2 = t3 > 190 && b6_1;
                      const gboolean b6_3 = rec->geometry_percent < 40 || b6_2;
                      const gboolean b6_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b6_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b6_3);
                      const gboolean b6_5 = rec->geometry_percent < 30 || b6_4;
                      const gboolean b6_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b6_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b6_5);
                      const gboolean b6_7 = rec->geometry_percent < 30 || b6_6;
                      const gboolean b6_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b6_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b6_7);
                      const gboolean b6_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b6_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b6_8));
                      const gboolean b6_10 = (t3 <= 185 && b6_2) || (t3 > 185 && b6_9);
                      const gboolean b6_11 = rec->geometry_percent < 50 || b6_10;
                      const gboolean b6_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b6_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b6_11);
                      if ((t3 <= 180 && b6_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b6_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b6_12)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                    }
                }
            }
        }
      else
        {
          if (probe_resolution != 0)
            {
              *status = 0;
              *conf = 0;
            }
          if (rec->selector != 128)
            {
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b7_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b7_1 = rec->geometry_percent < 40 || b7_0;
                      const gboolean b7_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b7_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b7_1);
                      const gboolean b7_3 = rec->geometry_percent < 45 || b7_2;
                      const gboolean b7_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b7_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b7_3);
                      const gboolean b7_5 = rec->geometry_percent < 25 || b7_4;
                      const gboolean b7_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b7_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b7_5);
                      const gboolean b7_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b7_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b7_6));
                      const gboolean b7_8 = (rec->normalized_coverage >= 90 && b7_4) || (rec->normalized_coverage < 90 && b7_7);
                      const gboolean b7_9 = rec->geometry_percent < 35 || b7_8;
                      const gboolean b7_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b7_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b7_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b7_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b7_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b8_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b8_1 = rec->geometry_percent < 60 || b8_0;
                      const gboolean b8_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b8_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b8_1);
                      const gboolean b8_3 = rec->geometry_percent < 35 || b8_2;
                      const gboolean b8_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b8_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b8_3);
                      const gboolean b8_5 = rec->geometry_percent < 22 || b8_4;
                      const gboolean b8_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b8_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b8_5);
                      const gboolean b8_7 = rec->geometry_percent < 15 || b8_6;
                      const gboolean b8_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b8_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b8_7);
                      const gboolean b8_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b8_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b8_8));
                      const gboolean b8_10 = (rec->normalized_coverage >= 95 && b8_6) || (rec->normalized_coverage < 95 && b8_9);
                      const gboolean b8_11 = rec->geometry_percent < 10 || b8_10;
                      const gboolean b8_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b8_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b8_11);
                      const gboolean b8_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b8_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b8_12));
                      const gboolean b8_14 = (rec->normalized_coverage >= 120 && b8_10) || (rec->normalized_coverage < 120 && b8_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b8_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b8_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b9_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b9_1 = rec->geometry_percent < 28 || b9_0;
                      const gboolean b9_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b9_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b9_1);
                      const gboolean b9_3 = rec->geometry_percent < 50 || b9_2;
                      const gboolean b9_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b9_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b9_3);
                      const gboolean b9_5 = rec->geometry_percent < 30 || b9_4;
                      const gboolean b9_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b9_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b9_5);
                      const gboolean b9_7 = rec->geometry_percent < 25 || b9_6;
                      const gboolean b9_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b9_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b9_7);
                      const gboolean b9_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b9_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b9_8));
                      const gboolean b9_10 = (rec->normalized_coverage >= 128 && b9_6) || (rec->normalized_coverage < 128 && b9_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b9_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b9_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b10_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b10_1 = rec->geometry_percent < 15 || b10_0;
                      const gboolean b10_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b10_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b10_1);
                      const gboolean b10_3 = rec->secondary_geometry_count < 12 || b10_2;
                      const gboolean b10_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b10_2) || (rec->normalized_coverage < 75 && t0 < 425 && b10_3);
                      const gboolean b10_5 = rec->secondary_geometry_count < 16 || b10_4;
                      const gboolean b10_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b10_4) || (rec->normalized_coverage < 90 && t0 < 405 && b10_5);
                      const gboolean b10_7 = rec->secondary_geometry_count < 21 || b10_6;
                      const gboolean b10_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b10_6) || (rec->normalized_coverage < 110 && t0 < 390 && b10_7);
                      const gboolean b10_9 = rec->secondary_geometry_count < 14 || b10_8;
                      const gboolean b10_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b10_8) || (rec->normalized_coverage < 130 && t0 < 415 && b10_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b10_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b10_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b11_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b11_1 = rec->secondary_geometry_count < 15 || b11_0;
                      const gboolean b11_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b11_0) || (rec->normalized_coverage < 90 && t0 < 430 && b11_1);
                      const gboolean b11_3 = rec->secondary_geometry_count < 20 || b11_2;
                      const gboolean b11_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b11_2) || (rec->normalized_coverage < 95 && t0 < 405 && b11_3);
                      const gboolean b11_5 = rec->secondary_geometry_count < 22 || b11_4;
                      const gboolean b11_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b11_4) || (rec->normalized_coverage < 100 && t0 < 400 && b11_5);
                      const gboolean b11_7 = rec->secondary_geometry_count < 7 || b11_6;
                      const gboolean b11_8 = (t0 >= 435 && b11_6) || (t0 < 435 && b11_7);
                      const gboolean b11_9 = rec->secondary_geometry_count < 10 || b11_8;
                      const gboolean b11_10 = (t0 >= 430 && b11_8) || (t0 < 430 && b11_9);
                      const gboolean b11_11 = (t0 >= 420 && b11_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b11_10));
                      const gboolean b11_12 = (rec->normalized_coverage >= 120 && b11_6) || (rec->normalized_coverage < 120 && b11_11);
                      const gboolean b11_13 = rec->secondary_geometry_count < 12 || b11_12;
                      const gboolean b11_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b11_12) || (rec->normalized_coverage < 140 && t0 < 430 && b11_13);
                      if (t3 >= 159 && ((t0 >= 380 && b11_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b11_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b12_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b12_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b12_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b12_0));
                          const gboolean b12_2 = t3 > 190 && b12_1;
                          const gboolean b12_3 = rec->geometry_percent < 40 || b12_2;
                          const gboolean b12_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b12_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b12_3);
                          const gboolean b12_5 = rec->geometry_percent < 30 || b12_4;
                          const gboolean b12_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b12_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b12_5);
                          const gboolean b12_7 = rec->geometry_percent < 30 || b12_6;
                          const gboolean b12_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b12_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b12_7);
                          const gboolean b12_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b12_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b12_8));
                          const gboolean b12_10 = (t3 <= 185 && b12_2) || (t3 > 185 && b12_9);
                          const gboolean b12_11 = rec->geometry_percent < 50 || b12_10;
                          const gboolean b12_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b12_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b12_11);
                          if ((t3 <= 180 && b12_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b12_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b12_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
          else if (rec->study_metric_20 >= 175)
            {
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b13_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b13_1 = rec->geometry_percent < 40 || b13_0;
                      const gboolean b13_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b13_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b13_1);
                      const gboolean b13_3 = rec->geometry_percent < 45 || b13_2;
                      const gboolean b13_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b13_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b13_3);
                      const gboolean b13_5 = rec->geometry_percent < 25 || b13_4;
                      const gboolean b13_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b13_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b13_5);
                      const gboolean b13_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b13_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b13_6));
                      const gboolean b13_8 = (rec->normalized_coverage >= 90 && b13_4) || (rec->normalized_coverage < 90 && b13_7);
                      const gboolean b13_9 = rec->geometry_percent < 35 || b13_8;
                      const gboolean b13_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b13_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b13_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b13_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b13_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b14_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b14_1 = rec->geometry_percent < 60 || b14_0;
                      const gboolean b14_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b14_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b14_1);
                      const gboolean b14_3 = rec->geometry_percent < 35 || b14_2;
                      const gboolean b14_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b14_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b14_3);
                      const gboolean b14_5 = rec->geometry_percent < 22 || b14_4;
                      const gboolean b14_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b14_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b14_5);
                      const gboolean b14_7 = rec->geometry_percent < 15 || b14_6;
                      const gboolean b14_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b14_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b14_7);
                      const gboolean b14_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b14_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b14_8));
                      const gboolean b14_10 = (rec->normalized_coverage >= 95 && b14_6) || (rec->normalized_coverage < 95 && b14_9);
                      const gboolean b14_11 = rec->geometry_percent < 10 || b14_10;
                      const gboolean b14_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b14_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b14_11);
                      const gboolean b14_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b14_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b14_12));
                      const gboolean b14_14 = (rec->normalized_coverage >= 120 && b14_10) || (rec->normalized_coverage < 120 && b14_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b14_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b14_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b15_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b15_1 = rec->geometry_percent < 28 || b15_0;
                      const gboolean b15_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b15_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b15_1);
                      const gboolean b15_3 = rec->geometry_percent < 50 || b15_2;
                      const gboolean b15_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b15_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b15_3);
                      const gboolean b15_5 = rec->geometry_percent < 30 || b15_4;
                      const gboolean b15_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b15_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b15_5);
                      const gboolean b15_7 = rec->geometry_percent < 25 || b15_6;
                      const gboolean b15_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b15_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b15_7);
                      const gboolean b15_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b15_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b15_8));
                      const gboolean b15_10 = (rec->normalized_coverage >= 128 && b15_6) || (rec->normalized_coverage < 128 && b15_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b15_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b15_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b16_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b16_1 = rec->geometry_percent < 15 || b16_0;
                      const gboolean b16_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b16_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b16_1);
                      const gboolean b16_3 = rec->secondary_geometry_count < 12 || b16_2;
                      const gboolean b16_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b16_2) || (rec->normalized_coverage < 75 && t0 < 425 && b16_3);
                      const gboolean b16_5 = rec->secondary_geometry_count < 16 || b16_4;
                      const gboolean b16_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b16_4) || (rec->normalized_coverage < 90 && t0 < 405 && b16_5);
                      const gboolean b16_7 = rec->secondary_geometry_count < 21 || b16_6;
                      const gboolean b16_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b16_6) || (rec->normalized_coverage < 110 && t0 < 390 && b16_7);
                      const gboolean b16_9 = rec->secondary_geometry_count < 14 || b16_8;
                      const gboolean b16_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b16_8) || (rec->normalized_coverage < 130 && t0 < 415 && b16_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b16_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b16_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b17_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b17_1 = rec->secondary_geometry_count < 15 || b17_0;
                      const gboolean b17_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b17_0) || (rec->normalized_coverage < 90 && t0 < 430 && b17_1);
                      const gboolean b17_3 = rec->secondary_geometry_count < 20 || b17_2;
                      const gboolean b17_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b17_2) || (rec->normalized_coverage < 95 && t0 < 405 && b17_3);
                      const gboolean b17_5 = rec->secondary_geometry_count < 22 || b17_4;
                      const gboolean b17_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b17_4) || (rec->normalized_coverage < 100 && t0 < 400 && b17_5);
                      const gboolean b17_7 = rec->secondary_geometry_count < 7 || b17_6;
                      const gboolean b17_8 = (t0 >= 435 && b17_6) || (t0 < 435 && b17_7);
                      const gboolean b17_9 = rec->secondary_geometry_count < 10 || b17_8;
                      const gboolean b17_10 = (t0 >= 430 && b17_8) || (t0 < 430 && b17_9);
                      const gboolean b17_11 = (t0 >= 420 && b17_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b17_10));
                      const gboolean b17_12 = (rec->normalized_coverage >= 120 && b17_6) || (rec->normalized_coverage < 120 && b17_11);
                      const gboolean b17_13 = rec->secondary_geometry_count < 12 || b17_12;
                      const gboolean b17_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b17_12) || (rec->normalized_coverage < 140 && t0 < 430 && b17_13);
                      if (t3 >= 159 && ((t0 >= 380 && b17_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b17_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b18_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b18_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b18_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b18_0));
                          const gboolean b18_2 = t3 > 190 && b18_1;
                          const gboolean b18_3 = rec->geometry_percent < 40 || b18_2;
                          const gboolean b18_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b18_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b18_3);
                          const gboolean b18_5 = rec->geometry_percent < 30 || b18_4;
                          const gboolean b18_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b18_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b18_5);
                          const gboolean b18_7 = rec->geometry_percent < 30 || b18_6;
                          const gboolean b18_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b18_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b18_7);
                          const gboolean b18_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b18_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b18_8));
                          const gboolean b18_10 = (t3 <= 185 && b18_2) || (t3 > 185 && b18_9);
                          const gboolean b18_11 = rec->geometry_percent < 50 || b18_10;
                          const gboolean b18_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b18_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b18_11);
                          if ((t3 <= 180 && b18_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b18_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b18_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
          else
            {
              if (rec->geometry_count < 6)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->penalty_flag_b != 0)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->geometry_count < 13 && rec->normalized_coverage < 180)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b19_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b19_1 = rec->geometry_percent < 40 || b19_0;
                      const gboolean b19_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b19_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b19_1);
                      const gboolean b19_3 = rec->geometry_percent < 45 || b19_2;
                      const gboolean b19_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b19_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b19_3);
                      const gboolean b19_5 = rec->geometry_percent < 25 || b19_4;
                      const gboolean b19_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b19_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b19_5);
                      const gboolean b19_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b19_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b19_6));
                      const gboolean b19_8 = (rec->normalized_coverage >= 90 && b19_4) || (rec->normalized_coverage < 90 && b19_7);
                      const gboolean b19_9 = rec->geometry_percent < 35 || b19_8;
                      const gboolean b19_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b19_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b19_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b19_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b19_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b20_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b20_1 = rec->geometry_percent < 60 || b20_0;
                      const gboolean b20_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b20_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b20_1);
                      const gboolean b20_3 = rec->geometry_percent < 35 || b20_2;
                      const gboolean b20_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b20_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b20_3);
                      const gboolean b20_5 = rec->geometry_percent < 22 || b20_4;
                      const gboolean b20_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b20_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b20_5);
                      const gboolean b20_7 = rec->geometry_percent < 15 || b20_6;
                      const gboolean b20_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b20_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b20_7);
                      const gboolean b20_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b20_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b20_8));
                      const gboolean b20_10 = (rec->normalized_coverage >= 95 && b20_6) || (rec->normalized_coverage < 95 && b20_9);
                      const gboolean b20_11 = rec->geometry_percent < 10 || b20_10;
                      const gboolean b20_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b20_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b20_11);
                      const gboolean b20_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b20_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b20_12));
                      const gboolean b20_14 = (rec->normalized_coverage >= 120 && b20_10) || (rec->normalized_coverage < 120 && b20_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b20_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b20_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b21_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b21_1 = rec->geometry_percent < 28 || b21_0;
                      const gboolean b21_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b21_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b21_1);
                      const gboolean b21_3 = rec->geometry_percent < 50 || b21_2;
                      const gboolean b21_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b21_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b21_3);
                      const gboolean b21_5 = rec->geometry_percent < 30 || b21_4;
                      const gboolean b21_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b21_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b21_5);
                      const gboolean b21_7 = rec->geometry_percent < 25 || b21_6;
                      const gboolean b21_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b21_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b21_7);
                      const gboolean b21_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b21_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b21_8));
                      const gboolean b21_10 = (rec->normalized_coverage >= 128 && b21_6) || (rec->normalized_coverage < 128 && b21_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b21_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b21_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b22_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b22_1 = rec->geometry_percent < 15 || b22_0;
                      const gboolean b22_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b22_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b22_1);
                      const gboolean b22_3 = rec->secondary_geometry_count < 12 || b22_2;
                      const gboolean b22_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b22_2) || (rec->normalized_coverage < 75 && t0 < 425 && b22_3);
                      const gboolean b22_5 = rec->secondary_geometry_count < 16 || b22_4;
                      const gboolean b22_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b22_4) || (rec->normalized_coverage < 90 && t0 < 405 && b22_5);
                      const gboolean b22_7 = rec->secondary_geometry_count < 21 || b22_6;
                      const gboolean b22_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b22_6) || (rec->normalized_coverage < 110 && t0 < 390 && b22_7);
                      const gboolean b22_9 = rec->secondary_geometry_count < 14 || b22_8;
                      const gboolean b22_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b22_8) || (rec->normalized_coverage < 130 && t0 < 415 && b22_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b22_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b22_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b23_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b23_1 = rec->secondary_geometry_count < 15 || b23_0;
                      const gboolean b23_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b23_0) || (rec->normalized_coverage < 90 && t0 < 430 && b23_1);
                      const gboolean b23_3 = rec->secondary_geometry_count < 20 || b23_2;
                      const gboolean b23_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b23_2) || (rec->normalized_coverage < 95 && t0 < 405 && b23_3);
                      const gboolean b23_5 = rec->secondary_geometry_count < 22 || b23_4;
                      const gboolean b23_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b23_4) || (rec->normalized_coverage < 100 && t0 < 400 && b23_5);
                      const gboolean b23_7 = rec->secondary_geometry_count < 7 || b23_6;
                      const gboolean b23_8 = (t0 >= 435 && b23_6) || (t0 < 435 && b23_7);
                      const gboolean b23_9 = rec->secondary_geometry_count < 10 || b23_8;
                      const gboolean b23_10 = (t0 >= 430 && b23_8) || (t0 < 430 && b23_9);
                      const gboolean b23_11 = (t0 >= 420 && b23_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b23_10));
                      const gboolean b23_12 = (rec->normalized_coverage >= 120 && b23_6) || (rec->normalized_coverage < 120 && b23_11);
                      const gboolean b23_13 = rec->secondary_geometry_count < 12 || b23_12;
                      const gboolean b23_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b23_12) || (rec->normalized_coverage < 140 && t0 < 430 && b23_13);
                      if (t3 >= 159 && ((t0 >= 380 && b23_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b23_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b24_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b24_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b24_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b24_0));
                          const gboolean b24_2 = t3 > 190 && b24_1;
                          const gboolean b24_3 = rec->geometry_percent < 40 || b24_2;
                          const gboolean b24_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b24_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b24_3);
                          const gboolean b24_5 = rec->geometry_percent < 30 || b24_4;
                          const gboolean b24_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b24_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b24_5);
                          const gboolean b24_7 = rec->geometry_percent < 30 || b24_6;
                          const gboolean b24_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b24_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b24_7);
                          const gboolean b24_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b24_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b24_8));
                          const gboolean b24_10 = (t3 <= 185 && b24_2) || (t3 > 185 && b24_9);
                          const gboolean b24_11 = rec->geometry_percent < 50 || b24_10;
                          const gboolean b24_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b24_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b24_11);
                          if ((t3 <= 180 && b24_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b24_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b24_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
        }
    }
  else
    {
      if (rec->normalized_coverage < 105 && (((rec->geometry_count > 4 || rec->secondary_geometry_count > 8) && rec->geometry_count <= 10 && rec->secondary_geometry_count <= 17 && t0 < 376) || (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 8 && (t0 < 415 || (rec->geometry_count <= 10 && rec->secondary_geometry_count <= 17 && t0 < 376)))))
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 14 && t1 <= 200 && rec->matched_percent <= 60 && t0 <= 380)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 11 && t1 <= 196 && rec->matched_percent <= 56 && t0 <= 385 && rec->geometry_percent < 25)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 10 && rec->secondary_geometry_count <= 15 && t1 <= 210 && rec->matched_percent <= 60 && t0 <= 390)
        {
          *status = 0;
          *conf = 0;
        }
      else if (((rec->geometry_count > 9 || rec->secondary_geometry_count > 12 || t1 > 210 || rec->matched_percent > 50 || rec->geometry_percent > 15) && rec->normalized_coverage < 36 && rec->geometry_count < rec->secondary_geometry_count && rec->secondary_geometry_count <= 9 && t0 < 426) || (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 12 && t1 <= 210 && rec->matched_percent <= 50 && rec->geometry_percent <= 15 && (t0 <= 399 || (rec->normalized_coverage < 36 && rec->geometry_count < rec->secondary_geometry_count && rec->secondary_geometry_count <= 9 && t0 < 426))))
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->normalized_coverage < 45 && rec->geometry_count < rec->secondary_geometry_count && rec->secondary_geometry_count <= 10 && t0 < 416)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->normalized_coverage < 52 && rec->geometry_count < rec->secondary_geometry_count && rec->secondary_geometry_count <= 9 && t0 < 414)
        {
          *status = 0;
          *conf = 0;
        }
      const gboolean b25_0 = rec->secondary_geometry_count <= 16 && t1 <= 200 && t2 <= 170;
      const gboolean b25_1 = rec->matched_percent <= 50 || b25_0;
      const gboolean b25_2 = ((rec->geometry_count > 15 || rec->secondary_geometry_count > 18 || rec->geometry_percent >= 15) && b25_0) || (rec->geometry_count <= 15 && rec->secondary_geometry_count <= 18 && rec->geometry_percent < 15 && b25_1);
      const gboolean b25_3 = t1 <= 215 || b25_2;
      const gboolean b25_4 = ((rec->geometry_count > 12 || rec->secondary_geometry_count > 15 || rec->geometry_percent >= 20) && b25_2) || (rec->geometry_count <= 12 && rec->secondary_geometry_count <= 15 && rec->geometry_percent < 20 && b25_3);
      const gboolean b25_5 = t2 <= 190 || b25_4;
      const gboolean b25_6 = ((rec->geometry_count > 9 || rec->secondary_geometry_count > 13 || rec->geometry_percent >= 15) && b25_4) || (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 13 && rec->geometry_percent < 15 && b25_5);
      const gboolean b25_7 = rec->geometry_percent < 10 || b25_6;
      const gboolean b25_8 = ((rec->geometry_count > 11 || rec->secondary_geometry_count > 12) && b25_6) || (rec->geometry_count <= 11 && rec->secondary_geometry_count <= 12 && b25_7);
      const gboolean b25_9 = rec->geometry_percent <= 10 || b25_8;
      const gboolean b25_10 = ((rec->geometry_count > 12 || t2 > 165) && b25_8) || (rec->geometry_count <= 12 && t2 <= 165 && b25_9);
      if (*status != 0 && rec->selector <= 128 && (((rec->geometry_count >= 20 || t1 >= 200) && b25_10) || (rec->geometry_count < 20 && t1 < 200 && (t2 < 150 || b25_10))))
        {
          *status = 0;
          *conf = 0;
        }
      if (t1 <= 205 && (((rec->geometry_count > 9 || rec->secondary_geometry_count > 15) && rec->geometry_count <= 12 && rec->secondary_geometry_count <= 16 && rec->geometry_percent <= 25 && t0 <= 381) || (rec->geometry_count <= 9 && rec->secondary_geometry_count <= 15 && (rec->geometry_percent <= 20 || (rec->geometry_count <= 12 && rec->secondary_geometry_count <= 16 && rec->geometry_percent <= 25 && t0 <= 381)))))
        {
          *status = 0;
          *conf = 0;
        }
      else if (t1 <= 200 && rec->geometry_count <= 13 && rec->secondary_geometry_count <= 16 && t2 <= 200 && rec->geometry_percent <= 25)
        {
          *status = 0;
          *conf = 0;
        }
      if (*status != 0)
        {
          if (rec->normalized_coverage <= 40 && rec->geometry_count <= 5 && rec->secondary_geometry_count <= 7 && rec->selector <= 222 && t1 <= 227)
            {
              *status = 0;
              *conf = 0;
            }
          else if (((rec->normalized_coverage > 66 || rec->geometry_count > 7 || rec->secondary_geometry_count > 12 || rec->selector > 226 || t1 > 220) && rec->normalized_coverage <= 80 && (((rec->geometry_count > 5 || rec->secondary_geometry_count > 13) && (((rec->geometry_count > 4 || rec->secondary_geometry_count > 10) && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210) || (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 10 && (t1 <= 218 || (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210))))) || (rec->geometry_count <= 5 && rec->secondary_geometry_count <= 13 && (t1 <= 215 || ((rec->geometry_count > 4 || rec->secondary_geometry_count > 10) && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210) || (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 10 && (t1 <= 218 || (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210))))))) || (rec->normalized_coverage <= 66 && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 12 && rec->selector <= 226 && t1 <= 220 && (rec->geometry_percent <= 25 || (rec->normalized_coverage <= 80 && (((rec->geometry_count > 5 || rec->secondary_geometry_count > 13) && (((rec->geometry_count > 4 || rec->secondary_geometry_count > 10) && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210) || (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 10 && (t1 <= 218 || (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210))))) || (rec->geometry_count <= 5 && rec->secondary_geometry_count <= 13 && (t1 <= 215 || ((rec->geometry_count > 4 || rec->secondary_geometry_count > 10) && rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210) || (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 10 && (t1 <= 218 || (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && t1 <= 210))))))))))
            {
              *status = 0;
              *conf = 0;
            }
          if (*status != 0 && rec->probe_coverage < 25 && rec->probe_quality <= 15)
            {
              *status = 0;
              *conf = 0;
            }
        }
      if (rec->geometry_count < rec->secondary_geometry_count)
        {
          if (t0 <= 388 && t1 <= 201 && ((rec->secondary_geometry_count > 16 && rec->geometry_count <= 11 && (((rec->normalized_coverage > 100 || rec->geometry_percent > 35) && rec->matched_percent <= 40 && rec->geometry_percent <= 17) || (rec->normalized_coverage <= 100 && rec->geometry_percent <= 35 && (rec->selector <= 217 || (rec->matched_percent <= 40 && rec->geometry_percent <= 17))))) || (rec->secondary_geometry_count <= 16 && (rec->geometry_percent <= 15 || (rec->geometry_count <= 11 && (((rec->normalized_coverage > 100 || rec->geometry_percent > 35) && rec->matched_percent <= 40 && rec->geometry_percent <= 17) || (rec->normalized_coverage <= 100 && rec->geometry_percent <= 35 && (rec->selector <= 217 || (rec->matched_percent <= 40 && rec->geometry_percent <= 17)))))))))
            {
              *status = 0;
              *conf = 0;
            }
          if (rec->secondary_geometry_count <= 12 && rec->geometry_count <= 5 && t1 <= 212 && rec->matched_percent <= 60)
            {
              *status = 0;
              *conf = 0;
            }
          else if (rec->geometry_count <= 9 && t0 <= 376 && rec->selector <= 232 && t2 <= 172 && rec->normalized_coverage <= 132 && rec->geometry_percent <= 25)
            {
              *status = 0;
              *conf = 0;
            }
          else if (rec->geometry_count <= 8 && t0 <= 400 && rec->selector <= 228 && t2 <= 197 && rec->normalized_coverage <= 115 && rec->geometry_percent <= 27)
            {
              *status = 0;
              *conf = 0;
            }
          else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 14 && t1 <= 205 && t0 <= 395 && rec->matched_percent <= 60)
            {
              *status = 0;
              *conf = 0;
            }
        }
      if (rec->geometry_count <= 3 && rec->secondary_geometry_count <= 9 && rec->normalized_coverage <= 83 && rec->matched_percent <= 50)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 4 && rec->secondary_geometry_count <= 14 && t1 <= 210 && rec->normalized_coverage <= 110 && rec->geometry_percent <= 30 && t0 <= 410)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 7 && t1 <= 215 && rec->normalized_coverage <= 90 && rec->geometry_percent <= 18 && t0 <= 421)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 8 && rec->secondary_geometry_count <= 10 && rec->geometry_percent <= 14 && rec->matched_percent <= 50 && t1 <= 215)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 7 && rec->secondary_geometry_count <= 9 && rec->geometry_percent <= 18 && rec->matched_percent <= 38 && t1 <= 218)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 5 && rec->secondary_geometry_count <= 16 && rec->geometry_percent <= 15 && t1 <= 208 && rec->selector <= 234)
        {
          *status = 0;
          *conf = 0;
        }
      else if (rec->geometry_count <= 6 && (((rec->secondary_geometry_count > 14 || rec->geometry_percent > 22 || t1 > 208) && rec->secondary_geometry_count <= 8 && rec->normalized_coverage <= 50 && rec->matched_percent <= 45 && t4 <= 664) || (rec->secondary_geometry_count <= 14 && rec->geometry_percent <= 22 && t1 <= 208 && (rec->selector <= 224 || (rec->secondary_geometry_count <= 8 && rec->normalized_coverage <= 50 && rec->matched_percent <= 45 && t4 <= 664)))))
        {
          *status = 0;
          *conf = 0;
        }
      if ((rec->penalty_flag_b == 1 && ((FALSE /* never: t0 >= 390 && t0 < 370 */ && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count) || (t0 < 390 && (rec->secondary_geometry_count < 15 || (t0 < 370 && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count))))) || (rec->penalty_flag_b != 1 && rec->geometry_count <= 6 && ((FALSE /* never: t0 >= 390 && t0 < 370 */ && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count) || (t0 < 390 && (rec->secondary_geometry_count < 15 || (t0 < 370 && rec->secondary_geometry_count < 19 && rec->geometry_count < rec->secondary_geometry_count))))))
        {
          *status = 0;
          *conf = 0;
        }
      if (rec->selector != 128)
        {
          if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
            {
              *status = 0;
              *conf = 0;
            }
          if (*status != 0)
            {
              if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
            }
          if (*status == 0)
            {
              *status = 0;
            }
          else
            {
              if (rec->normalized_coverage < 35)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->geometry_percent < 8)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                {
                  const gboolean b26_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                  const gboolean b26_1 = rec->geometry_percent < 40 || b26_0;
                  const gboolean b26_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b26_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b26_1);
                  const gboolean b26_3 = rec->geometry_percent < 45 || b26_2;
                  const gboolean b26_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b26_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b26_3);
                  const gboolean b26_5 = rec->geometry_percent < 25 || b26_4;
                  const gboolean b26_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b26_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b26_5);
                  const gboolean b26_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b26_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b26_6));
                  const gboolean b26_8 = (rec->normalized_coverage >= 90 && b26_4) || (rec->normalized_coverage < 90 && b26_7);
                  const gboolean b26_9 = rec->geometry_percent < 35 || b26_8;
                  const gboolean b26_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b26_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b26_9);
                  if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b26_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b26_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b27_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                  const gboolean b27_1 = rec->geometry_percent < 60 || b27_0;
                  const gboolean b27_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b27_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b27_1);
                  const gboolean b27_3 = rec->geometry_percent < 35 || b27_2;
                  const gboolean b27_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b27_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b27_3);
                  const gboolean b27_5 = rec->geometry_percent < 22 || b27_4;
                  const gboolean b27_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b27_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b27_5);
                  const gboolean b27_7 = rec->geometry_percent < 15 || b27_6;
                  const gboolean b27_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b27_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b27_7);
                  const gboolean b27_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b27_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b27_8));
                  const gboolean b27_10 = (rec->normalized_coverage >= 95 && b27_6) || (rec->normalized_coverage < 95 && b27_9);
                  const gboolean b27_11 = rec->geometry_percent < 10 || b27_10;
                  const gboolean b27_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b27_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b27_11);
                  const gboolean b27_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b27_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b27_12));
                  const gboolean b27_14 = (rec->normalized_coverage >= 120 && b27_10) || (rec->normalized_coverage < 120 && b27_13);
                  if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b27_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b27_14))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b28_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                  const gboolean b28_1 = rec->geometry_percent < 28 || b28_0;
                  const gboolean b28_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b28_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b28_1);
                  const gboolean b28_3 = rec->geometry_percent < 50 || b28_2;
                  const gboolean b28_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b28_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b28_3);
                  const gboolean b28_5 = rec->geometry_percent < 30 || b28_4;
                  const gboolean b28_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b28_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b28_5);
                  const gboolean b28_7 = rec->geometry_percent < 25 || b28_6;
                  const gboolean b28_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b28_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b28_7);
                  const gboolean b28_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b28_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b28_8));
                  const gboolean b28_10 = (rec->normalized_coverage >= 128 && b28_6) || (rec->normalized_coverage < 128 && b28_9);
                  if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b28_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b28_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b29_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                  const gboolean b29_1 = rec->geometry_percent < 15 || b29_0;
                  const gboolean b29_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b29_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b29_1);
                  const gboolean b29_3 = rec->secondary_geometry_count < 12 || b29_2;
                  const gboolean b29_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b29_2) || (rec->normalized_coverage < 75 && t0 < 425 && b29_3);
                  const gboolean b29_5 = rec->secondary_geometry_count < 16 || b29_4;
                  const gboolean b29_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b29_4) || (rec->normalized_coverage < 90 && t0 < 405 && b29_5);
                  const gboolean b29_7 = rec->secondary_geometry_count < 21 || b29_6;
                  const gboolean b29_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b29_6) || (rec->normalized_coverage < 110 && t0 < 390 && b29_7);
                  const gboolean b29_9 = rec->secondary_geometry_count < 14 || b29_8;
                  const gboolean b29_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b29_8) || (rec->normalized_coverage < 130 && t0 < 415 && b29_9);
                  if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b29_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b29_10))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  const gboolean b30_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                  const gboolean b30_1 = rec->secondary_geometry_count < 15 || b30_0;
                  const gboolean b30_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b30_0) || (rec->normalized_coverage < 90 && t0 < 430 && b30_1);
                  const gboolean b30_3 = rec->secondary_geometry_count < 20 || b30_2;
                  const gboolean b30_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b30_2) || (rec->normalized_coverage < 95 && t0 < 405 && b30_3);
                  const gboolean b30_5 = rec->secondary_geometry_count < 22 || b30_4;
                  const gboolean b30_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b30_4) || (rec->normalized_coverage < 100 && t0 < 400 && b30_5);
                  const gboolean b30_7 = rec->secondary_geometry_count < 7 || b30_6;
                  const gboolean b30_8 = (t0 >= 435 && b30_6) || (t0 < 435 && b30_7);
                  const gboolean b30_9 = rec->secondary_geometry_count < 10 || b30_8;
                  const gboolean b30_10 = (t0 >= 430 && b30_8) || (t0 < 430 && b30_9);
                  const gboolean b30_11 = (t0 >= 420 && b30_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b30_10));
                  const gboolean b30_12 = (rec->normalized_coverage >= 120 && b30_6) || (rec->normalized_coverage < 120 && b30_11);
                  const gboolean b30_13 = rec->secondary_geometry_count < 12 || b30_12;
                  const gboolean b30_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b30_12) || (rec->normalized_coverage < 140 && t0 < 430 && b30_13);
                  if (t3 >= 159 && ((t0 >= 380 && b30_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b30_14))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else
                    {
                      const gboolean b31_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                      const gboolean b31_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b31_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b31_0));
                      const gboolean b31_2 = t3 > 190 && b31_1;
                      const gboolean b31_3 = rec->geometry_percent < 40 || b31_2;
                      const gboolean b31_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b31_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b31_3);
                      const gboolean b31_5 = rec->geometry_percent < 30 || b31_4;
                      const gboolean b31_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b31_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b31_5);
                      const gboolean b31_7 = rec->geometry_percent < 30 || b31_6;
                      const gboolean b31_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b31_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b31_7);
                      const gboolean b31_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b31_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b31_8));
                      const gboolean b31_10 = (t3 <= 185 && b31_2) || (t3 > 185 && b31_9);
                      const gboolean b31_11 = rec->geometry_percent < 50 || b31_10;
                      const gboolean b31_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b31_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b31_11);
                      if ((t3 <= 180 && b31_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b31_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b31_12)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                    }
                }
            }
        }
      else
        {
          if (probe_resolution != 0)
            {
              *status = 0;
              *conf = 0;
            }
          if (rec->selector != 128)
            {
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b32_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b32_1 = rec->geometry_percent < 40 || b32_0;
                      const gboolean b32_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b32_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b32_1);
                      const gboolean b32_3 = rec->geometry_percent < 45 || b32_2;
                      const gboolean b32_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b32_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b32_3);
                      const gboolean b32_5 = rec->geometry_percent < 25 || b32_4;
                      const gboolean b32_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b32_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b32_5);
                      const gboolean b32_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b32_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b32_6));
                      const gboolean b32_8 = (rec->normalized_coverage >= 90 && b32_4) || (rec->normalized_coverage < 90 && b32_7);
                      const gboolean b32_9 = rec->geometry_percent < 35 || b32_8;
                      const gboolean b32_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b32_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b32_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b32_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b32_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b33_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b33_1 = rec->geometry_percent < 60 || b33_0;
                      const gboolean b33_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b33_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b33_1);
                      const gboolean b33_3 = rec->geometry_percent < 35 || b33_2;
                      const gboolean b33_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b33_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b33_3);
                      const gboolean b33_5 = rec->geometry_percent < 22 || b33_4;
                      const gboolean b33_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b33_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b33_5);
                      const gboolean b33_7 = rec->geometry_percent < 15 || b33_6;
                      const gboolean b33_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b33_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b33_7);
                      const gboolean b33_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b33_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b33_8));
                      const gboolean b33_10 = (rec->normalized_coverage >= 95 && b33_6) || (rec->normalized_coverage < 95 && b33_9);
                      const gboolean b33_11 = rec->geometry_percent < 10 || b33_10;
                      const gboolean b33_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b33_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b33_11);
                      const gboolean b33_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b33_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b33_12));
                      const gboolean b33_14 = (rec->normalized_coverage >= 120 && b33_10) || (rec->normalized_coverage < 120 && b33_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b33_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b33_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b34_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b34_1 = rec->geometry_percent < 28 || b34_0;
                      const gboolean b34_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b34_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b34_1);
                      const gboolean b34_3 = rec->geometry_percent < 50 || b34_2;
                      const gboolean b34_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b34_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b34_3);
                      const gboolean b34_5 = rec->geometry_percent < 30 || b34_4;
                      const gboolean b34_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b34_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b34_5);
                      const gboolean b34_7 = rec->geometry_percent < 25 || b34_6;
                      const gboolean b34_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b34_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b34_7);
                      const gboolean b34_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b34_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b34_8));
                      const gboolean b34_10 = (rec->normalized_coverage >= 128 && b34_6) || (rec->normalized_coverage < 128 && b34_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b34_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b34_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b35_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b35_1 = rec->geometry_percent < 15 || b35_0;
                      const gboolean b35_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b35_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b35_1);
                      const gboolean b35_3 = rec->secondary_geometry_count < 12 || b35_2;
                      const gboolean b35_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b35_2) || (rec->normalized_coverage < 75 && t0 < 425 && b35_3);
                      const gboolean b35_5 = rec->secondary_geometry_count < 16 || b35_4;
                      const gboolean b35_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b35_4) || (rec->normalized_coverage < 90 && t0 < 405 && b35_5);
                      const gboolean b35_7 = rec->secondary_geometry_count < 21 || b35_6;
                      const gboolean b35_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b35_6) || (rec->normalized_coverage < 110 && t0 < 390 && b35_7);
                      const gboolean b35_9 = rec->secondary_geometry_count < 14 || b35_8;
                      const gboolean b35_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b35_8) || (rec->normalized_coverage < 130 && t0 < 415 && b35_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b35_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b35_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b36_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b36_1 = rec->secondary_geometry_count < 15 || b36_0;
                      const gboolean b36_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b36_0) || (rec->normalized_coverage < 90 && t0 < 430 && b36_1);
                      const gboolean b36_3 = rec->secondary_geometry_count < 20 || b36_2;
                      const gboolean b36_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b36_2) || (rec->normalized_coverage < 95 && t0 < 405 && b36_3);
                      const gboolean b36_5 = rec->secondary_geometry_count < 22 || b36_4;
                      const gboolean b36_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b36_4) || (rec->normalized_coverage < 100 && t0 < 400 && b36_5);
                      const gboolean b36_7 = rec->secondary_geometry_count < 7 || b36_6;
                      const gboolean b36_8 = (t0 >= 435 && b36_6) || (t0 < 435 && b36_7);
                      const gboolean b36_9 = rec->secondary_geometry_count < 10 || b36_8;
                      const gboolean b36_10 = (t0 >= 430 && b36_8) || (t0 < 430 && b36_9);
                      const gboolean b36_11 = (t0 >= 420 && b36_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b36_10));
                      const gboolean b36_12 = (rec->normalized_coverage >= 120 && b36_6) || (rec->normalized_coverage < 120 && b36_11);
                      const gboolean b36_13 = rec->secondary_geometry_count < 12 || b36_12;
                      const gboolean b36_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b36_12) || (rec->normalized_coverage < 140 && t0 < 430 && b36_13);
                      if (t3 >= 159 && ((t0 >= 380 && b36_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b36_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b37_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b37_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b37_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b37_0));
                          const gboolean b37_2 = t3 > 190 && b37_1;
                          const gboolean b37_3 = rec->geometry_percent < 40 || b37_2;
                          const gboolean b37_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b37_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b37_3);
                          const gboolean b37_5 = rec->geometry_percent < 30 || b37_4;
                          const gboolean b37_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b37_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b37_5);
                          const gboolean b37_7 = rec->geometry_percent < 30 || b37_6;
                          const gboolean b37_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b37_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b37_7);
                          const gboolean b37_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b37_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b37_8));
                          const gboolean b37_10 = (t3 <= 185 && b37_2) || (t3 > 185 && b37_9);
                          const gboolean b37_11 = rec->geometry_percent < 50 || b37_10;
                          const gboolean b37_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b37_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b37_11);
                          if ((t3 <= 180 && b37_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b37_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b37_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
          else if (rec->study_metric_20 >= 175)
            {
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b38_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b38_1 = rec->geometry_percent < 40 || b38_0;
                      const gboolean b38_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b38_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b38_1);
                      const gboolean b38_3 = rec->geometry_percent < 45 || b38_2;
                      const gboolean b38_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b38_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b38_3);
                      const gboolean b38_5 = rec->geometry_percent < 25 || b38_4;
                      const gboolean b38_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b38_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b38_5);
                      const gboolean b38_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b38_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b38_6));
                      const gboolean b38_8 = (rec->normalized_coverage >= 90 && b38_4) || (rec->normalized_coverage < 90 && b38_7);
                      const gboolean b38_9 = rec->geometry_percent < 35 || b38_8;
                      const gboolean b38_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b38_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b38_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b38_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b38_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b39_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b39_1 = rec->geometry_percent < 60 || b39_0;
                      const gboolean b39_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b39_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b39_1);
                      const gboolean b39_3 = rec->geometry_percent < 35 || b39_2;
                      const gboolean b39_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b39_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b39_3);
                      const gboolean b39_5 = rec->geometry_percent < 22 || b39_4;
                      const gboolean b39_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b39_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b39_5);
                      const gboolean b39_7 = rec->geometry_percent < 15 || b39_6;
                      const gboolean b39_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b39_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b39_7);
                      const gboolean b39_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b39_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b39_8));
                      const gboolean b39_10 = (rec->normalized_coverage >= 95 && b39_6) || (rec->normalized_coverage < 95 && b39_9);
                      const gboolean b39_11 = rec->geometry_percent < 10 || b39_10;
                      const gboolean b39_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b39_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b39_11);
                      const gboolean b39_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b39_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b39_12));
                      const gboolean b39_14 = (rec->normalized_coverage >= 120 && b39_10) || (rec->normalized_coverage < 120 && b39_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b39_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b39_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b40_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b40_1 = rec->geometry_percent < 28 || b40_0;
                      const gboolean b40_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b40_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b40_1);
                      const gboolean b40_3 = rec->geometry_percent < 50 || b40_2;
                      const gboolean b40_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b40_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b40_3);
                      const gboolean b40_5 = rec->geometry_percent < 30 || b40_4;
                      const gboolean b40_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b40_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b40_5);
                      const gboolean b40_7 = rec->geometry_percent < 25 || b40_6;
                      const gboolean b40_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b40_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b40_7);
                      const gboolean b40_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b40_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b40_8));
                      const gboolean b40_10 = (rec->normalized_coverage >= 128 && b40_6) || (rec->normalized_coverage < 128 && b40_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b40_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b40_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b41_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b41_1 = rec->geometry_percent < 15 || b41_0;
                      const gboolean b41_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b41_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b41_1);
                      const gboolean b41_3 = rec->secondary_geometry_count < 12 || b41_2;
                      const gboolean b41_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b41_2) || (rec->normalized_coverage < 75 && t0 < 425 && b41_3);
                      const gboolean b41_5 = rec->secondary_geometry_count < 16 || b41_4;
                      const gboolean b41_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b41_4) || (rec->normalized_coverage < 90 && t0 < 405 && b41_5);
                      const gboolean b41_7 = rec->secondary_geometry_count < 21 || b41_6;
                      const gboolean b41_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b41_6) || (rec->normalized_coverage < 110 && t0 < 390 && b41_7);
                      const gboolean b41_9 = rec->secondary_geometry_count < 14 || b41_8;
                      const gboolean b41_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b41_8) || (rec->normalized_coverage < 130 && t0 < 415 && b41_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b41_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b41_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b42_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b42_1 = rec->secondary_geometry_count < 15 || b42_0;
                      const gboolean b42_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b42_0) || (rec->normalized_coverage < 90 && t0 < 430 && b42_1);
                      const gboolean b42_3 = rec->secondary_geometry_count < 20 || b42_2;
                      const gboolean b42_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b42_2) || (rec->normalized_coverage < 95 && t0 < 405 && b42_3);
                      const gboolean b42_5 = rec->secondary_geometry_count < 22 || b42_4;
                      const gboolean b42_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b42_4) || (rec->normalized_coverage < 100 && t0 < 400 && b42_5);
                      const gboolean b42_7 = rec->secondary_geometry_count < 7 || b42_6;
                      const gboolean b42_8 = (t0 >= 435 && b42_6) || (t0 < 435 && b42_7);
                      const gboolean b42_9 = rec->secondary_geometry_count < 10 || b42_8;
                      const gboolean b42_10 = (t0 >= 430 && b42_8) || (t0 < 430 && b42_9);
                      const gboolean b42_11 = (t0 >= 420 && b42_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b42_10));
                      const gboolean b42_12 = (rec->normalized_coverage >= 120 && b42_6) || (rec->normalized_coverage < 120 && b42_11);
                      const gboolean b42_13 = rec->secondary_geometry_count < 12 || b42_12;
                      const gboolean b42_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b42_12) || (rec->normalized_coverage < 140 && t0 < 430 && b42_13);
                      if (t3 >= 159 && ((t0 >= 380 && b42_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b42_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b43_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b43_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b43_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b43_0));
                          const gboolean b43_2 = t3 > 190 && b43_1;
                          const gboolean b43_3 = rec->geometry_percent < 40 || b43_2;
                          const gboolean b43_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b43_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b43_3);
                          const gboolean b43_5 = rec->geometry_percent < 30 || b43_4;
                          const gboolean b43_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b43_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b43_5);
                          const gboolean b43_7 = rec->geometry_percent < 30 || b43_6;
                          const gboolean b43_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b43_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b43_7);
                          const gboolean b43_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b43_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b43_8));
                          const gboolean b43_10 = (t3 <= 185 && b43_2) || (t3 > 185 && b43_9);
                          const gboolean b43_11 = rec->geometry_percent < 50 || b43_10;
                          const gboolean b43_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b43_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b43_11);
                          if ((t3 <= 180 && b43_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b43_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b43_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
          else
            {
              if (rec->geometry_count < 6)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->penalty_flag_b != 0)
                {
                  *status = 0;
                  *conf = 0;
                }
              else if (rec->geometry_count < 13 && rec->normalized_coverage < 180)
                {
                  *status = 0;
                  *conf = 0;
                }
              if (rec->geometry_count < rec->secondary_geometry_count && (((rec->geometry_count > 5 || t1 > 202) && (((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203))))) || (rec->geometry_count <= 5 && t1 <= 202 && (t2 <= 201 || ((rec->normalized_coverage > 85 || rec->geometry_count > 5 || t1 > 212) && rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203) || (rec->normalized_coverage <= 85 && rec->geometry_count <= 5 && t1 <= 212 && (t2 <= 201 || (rec->normalized_coverage <= 40 && rec->geometry_count <= 6 && t1 <= 218 && t2 <= 203)))))))
                {
                  *status = 0;
                  *conf = 0;
                }
              if (*status != 0)
                {
                  if (rec->normalized_coverage < 128 && rec->geometry_count <= 6 && t0 <= 400)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 50 && rec->secondary_geometry_count <= 7 && t0 < 440)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 75 && rec->secondary_geometry_count <= 7 && t0 < 430)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 95 && rec->secondary_geometry_count <= 6 && t0 < 416)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 96 && rec->secondary_geometry_count <= 12 && t0 < 411 && t1 < 218)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->normalized_coverage < 105 && rec->secondary_geometry_count <= 15 && t0 < 396)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (*status != 0 && (((rec->secondary_geometry_count > 8 || t0 > 390) && (((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205))))) || (rec->secondary_geometry_count <= 8 && t0 <= 390 && (t1 < 216 || ((rec->secondary_geometry_count > 11 || t0 > 385) && rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205) || (rec->secondary_geometry_count <= 11 && t0 <= 385 && (t1 < 210 || (rec->secondary_geometry_count <= 15 && t0 <= 380 && t1 < 205)))))))
                    {
                      *status = 0;
                      *conf = 0;
                    }
                }
              if (*status == 0)
                {
                  *status = 0;
                }
              else
                {
                  if (rec->normalized_coverage < 35)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  else if (rec->geometry_percent < 8)
                    {
                      *status = 0;
                      *conf = 0;
                    }
                  if (rec->gallery_quality > 60 || rec->probe_quality > 60)
                    {
                      const gboolean b44_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 50;
                      const gboolean b44_1 = rec->geometry_percent < 40 || b44_0;
                      const gboolean b44_2 = ((rec->normalized_coverage >= 60 || t0 >= 420 || rec->secondary_geometry_count >= 18) && b44_0) || (rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 18 && b44_1);
                      const gboolean b44_3 = rec->geometry_percent < 45 || b44_2;
                      const gboolean b44_4 = ((rec->normalized_coverage >= 70 || t0 >= 420 || rec->secondary_geometry_count >= 12) && b44_2) || (rec->normalized_coverage < 70 && t0 < 420 && rec->secondary_geometry_count < 12 && b44_3);
                      const gboolean b44_5 = rec->geometry_percent < 25 || b44_4;
                      const gboolean b44_6 = ((t0 >= 410 || rec->secondary_geometry_count >= 16) && b44_4) || (t0 < 410 && rec->secondary_geometry_count < 16 && b44_5);
                      const gboolean b44_7 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b44_6) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 32 || b44_6));
                      const gboolean b44_8 = (rec->normalized_coverage >= 90 && b44_4) || (rec->normalized_coverage < 90 && b44_7);
                      const gboolean b44_9 = rec->geometry_percent < 35 || b44_8;
                      const gboolean b44_10 = ((rec->normalized_coverage >= 128 || t0 >= 380 || rec->secondary_geometry_count >= 17) && b44_8) || (rec->normalized_coverage < 128 && t0 < 380 && rec->secondary_geometry_count < 17 && b44_9);
                      if (t3 > 100 && (((rec->normalized_coverage >= 135 || t0 >= 395 || rec->secondary_geometry_count >= 15) && b44_10) || (rec->normalized_coverage < 135 && t0 < 395 && rec->secondary_geometry_count < 15 && (rec->geometry_percent < 15 || b44_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b45_0 = rec->normalized_coverage < 50 && t0 < 420 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 55;
                      const gboolean b45_1 = rec->geometry_percent < 60 || b45_0;
                      const gboolean b45_2 = ((rec->normalized_coverage >= 60 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b45_0) || (rec->normalized_coverage < 60 && t0 < 405 && rec->secondary_geometry_count < 20 && b45_1);
                      const gboolean b45_3 = rec->geometry_percent < 35 || b45_2;
                      const gboolean b45_4 = ((rec->normalized_coverage >= 70 || t0 >= 412 || rec->secondary_geometry_count >= 14) && b45_2) || (rec->normalized_coverage < 70 && t0 < 412 && rec->secondary_geometry_count < 14 && b45_3);
                      const gboolean b45_5 = rec->geometry_percent < 22 || b45_4;
                      const gboolean b45_6 = ((rec->normalized_coverage >= 80 || t0 >= 412 || rec->secondary_geometry_count >= 13) && b45_4) || (rec->normalized_coverage < 80 && t0 < 412 && rec->secondary_geometry_count < 13 && b45_5);
                      const gboolean b45_7 = rec->geometry_percent < 15 || b45_6;
                      const gboolean b45_8 = ((t0 >= 415 || rec->secondary_geometry_count >= 11) && b45_6) || (t0 < 415 && rec->secondary_geometry_count < 11 && b45_7);
                      const gboolean b45_9 = ((t0 >= 390 || rec->secondary_geometry_count >= 17) && b45_8) || (t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 25 || b45_8));
                      const gboolean b45_10 = (rec->normalized_coverage >= 95 && b45_6) || (rec->normalized_coverage < 95 && b45_9);
                      const gboolean b45_11 = rec->geometry_percent < 10 || b45_10;
                      const gboolean b45_12 = ((t0 >= 395 || rec->secondary_geometry_count >= 11) && b45_10) || (t0 < 395 && rec->secondary_geometry_count < 11 && b45_11);
                      const gboolean b45_13 = ((t0 >= 390 || rec->secondary_geometry_count >= 19) && b45_12) || (t0 < 390 && rec->secondary_geometry_count < 19 && (rec->geometry_percent < 20 || b45_12));
                      const gboolean b45_14 = (rec->normalized_coverage >= 120 && b45_10) || (rec->normalized_coverage < 120 && b45_13);
                      if (t3 > 115 && (((rec->normalized_coverage >= 130 || t0 >= 390 || rec->secondary_geometry_count >= 17) && b45_14) || (rec->normalized_coverage < 130 && t0 < 390 && rec->secondary_geometry_count < 17 && (rec->geometry_percent < 18 || b45_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b46_0 = rec->normalized_coverage < 60 && t0 < 420 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 55;
                      const gboolean b46_1 = rec->geometry_percent < 28 || b46_0;
                      const gboolean b46_2 = ((rec->normalized_coverage >= 80 || t0 >= 416 || rec->secondary_geometry_count >= 10) && b46_0) || (rec->normalized_coverage < 80 && t0 < 416 && rec->secondary_geometry_count < 10 && b46_1);
                      const gboolean b46_3 = rec->geometry_percent < 50 || b46_2;
                      const gboolean b46_4 = ((rec->normalized_coverage >= 90 || t0 >= 405 || rec->secondary_geometry_count >= 20) && b46_2) || (rec->normalized_coverage < 90 && t0 < 405 && rec->secondary_geometry_count < 20 && b46_3);
                      const gboolean b46_5 = rec->geometry_percent < 30 || b46_4;
                      const gboolean b46_6 = ((rec->normalized_coverage >= 115 || t0 >= 380 || rec->secondary_geometry_count >= 18) && b46_4) || (rec->normalized_coverage < 115 && t0 < 380 && rec->secondary_geometry_count < 18 && b46_5);
                      const gboolean b46_7 = rec->geometry_percent < 25 || b46_6;
                      const gboolean b46_8 = ((t0 >= 406 || rec->secondary_geometry_count >= 19) && b46_6) || (t0 < 406 && rec->secondary_geometry_count < 19 && b46_7);
                      const gboolean b46_9 = ((t0 >= 420 || rec->secondary_geometry_count >= 13) && b46_8) || (t0 < 420 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 25 || b46_8));
                      const gboolean b46_10 = (rec->normalized_coverage >= 128 && b46_6) || (rec->normalized_coverage < 128 && b46_9);
                      if (t3 > 130 && (((rec->normalized_coverage >= 135 || t0 >= 400 || rec->secondary_geometry_count >= 13) && b46_10) || (rec->normalized_coverage < 135 && t0 < 400 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 15 || b46_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b47_0 = rec->normalized_coverage < 60 && t0 < 430 && rec->secondary_geometry_count < 12 && rec->geometry_percent < 40;
                      const gboolean b47_1 = rec->geometry_percent < 15 || b47_0;
                      const gboolean b47_2 = ((rec->normalized_coverage >= 90 || t0 >= 425 || rec->secondary_geometry_count >= 7) && b47_0) || (rec->normalized_coverage < 90 && t0 < 425 && rec->secondary_geometry_count < 7 && b47_1);
                      const gboolean b47_3 = rec->secondary_geometry_count < 12 || b47_2;
                      const gboolean b47_4 = ((rec->normalized_coverage >= 75 || t0 >= 425) && b47_2) || (rec->normalized_coverage < 75 && t0 < 425 && b47_3);
                      const gboolean b47_5 = rec->secondary_geometry_count < 16 || b47_4;
                      const gboolean b47_6 = ((rec->normalized_coverage >= 90 || t0 >= 405) && b47_4) || (rec->normalized_coverage < 90 && t0 < 405 && b47_5);
                      const gboolean b47_7 = rec->secondary_geometry_count < 21 || b47_6;
                      const gboolean b47_8 = ((rec->normalized_coverage >= 110 || t0 >= 390) && b47_6) || (rec->normalized_coverage < 110 && t0 < 390 && b47_7);
                      const gboolean b47_9 = rec->secondary_geometry_count < 14 || b47_8;
                      const gboolean b47_10 = ((rec->normalized_coverage >= 130 || t0 >= 415) && b47_8) || (rec->normalized_coverage < 130 && t0 < 415 && b47_9);
                      if (t3 > 145 && (((rec->normalized_coverage >= 150 || t0 >= 395) && b47_10) || (rec->normalized_coverage < 150 && t0 < 395 && (rec->secondary_geometry_count < 18 || b47_10))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      const gboolean b48_0 = rec->normalized_coverage < 60 && t0 < 440 && rec->secondary_geometry_count < 12;
                      const gboolean b48_1 = rec->secondary_geometry_count < 15 || b48_0;
                      const gboolean b48_2 = ((rec->normalized_coverage >= 90 || t0 >= 430) && b48_0) || (rec->normalized_coverage < 90 && t0 < 430 && b48_1);
                      const gboolean b48_3 = rec->secondary_geometry_count < 20 || b48_2;
                      const gboolean b48_4 = ((rec->normalized_coverage >= 95 || t0 >= 405) && b48_2) || (rec->normalized_coverage < 95 && t0 < 405 && b48_3);
                      const gboolean b48_5 = rec->secondary_geometry_count < 22 || b48_4;
                      const gboolean b48_6 = ((rec->normalized_coverage >= 100 || t0 >= 400) && b48_4) || (rec->normalized_coverage < 100 && t0 < 400 && b48_5);
                      const gboolean b48_7 = rec->secondary_geometry_count < 7 || b48_6;
                      const gboolean b48_8 = (t0 >= 435 && b48_6) || (t0 < 435 && b48_7);
                      const gboolean b48_9 = rec->secondary_geometry_count < 10 || b48_8;
                      const gboolean b48_10 = (t0 >= 430 && b48_8) || (t0 < 430 && b48_9);
                      const gboolean b48_11 = (t0 >= 420 && b48_10) || (t0 < 420 && (rec->secondary_geometry_count < 15 || b48_10));
                      const gboolean b48_12 = (rec->normalized_coverage >= 120 && b48_6) || (rec->normalized_coverage < 120 && b48_11);
                      const gboolean b48_13 = rec->secondary_geometry_count < 12 || b48_12;
                      const gboolean b48_14 = ((rec->normalized_coverage >= 140 || t0 >= 430) && b48_12) || (rec->normalized_coverage < 140 && t0 < 430 && b48_13);
                      if (t3 >= 159 && ((t0 >= 380 && b48_14) || (t0 < 380 && (rec->secondary_geometry_count < 20 || b48_14))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      if (t3 > 150 && (((rec->normalized_coverage >= 128 || t0 >= 390 || rec->secondary_geometry_count >= 22) && rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50) || (rec->normalized_coverage < 128 && t0 < 390 && rec->secondary_geometry_count < 22 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 60 && t0 < 435 && rec->secondary_geometry_count < 11 && rec->geometry_percent < 50)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 160 && (((rec->normalized_coverage >= 128 || t0 >= 400 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40) || (rec->normalized_coverage < 128 && t0 < 400 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 21 && rec->geometry_percent < 40)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else if (t3 > 170 && (((rec->normalized_coverage >= 140 || t0 >= 410 || rec->secondary_geometry_count >= 20) && rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30) || (rec->normalized_coverage < 140 && t0 < 410 && rec->secondary_geometry_count < 20 && (rec->geometry_percent < 40 || (rec->normalized_coverage < 80 && t0 < 415 && rec->secondary_geometry_count < 17 && rec->geometry_percent < 30)))))
                        {
                          *status = 0;
                          *conf = 0;
                        }
                      else
                        {
                          const gboolean b49_0 = rec->normalized_coverage < 90 && t0 < 416 && rec->secondary_geometry_count < 20 && rec->geometry_percent < 45;
                          const gboolean b49_1 = ((rec->normalized_coverage >= 115 || t0 >= 445 || rec->secondary_geometry_count >= 10) && b49_0) || (rec->normalized_coverage < 115 && t0 < 445 && rec->secondary_geometry_count < 10 && (rec->geometry_percent < 35 || b49_0));
                          const gboolean b49_2 = t3 > 190 && b49_1;
                          const gboolean b49_3 = rec->geometry_percent < 40 || b49_2;
                          const gboolean b49_4 = ((rec->normalized_coverage >= 55 || t0 >= 456 || rec->secondary_geometry_count >= 9) && b49_2) || (rec->normalized_coverage < 55 && t0 < 456 && rec->secondary_geometry_count < 9 && b49_3);
                          const gboolean b49_5 = rec->geometry_percent < 30 || b49_4;
                          const gboolean b49_6 = ((rec->normalized_coverage >= 70 || t0 >= 450 || rec->secondary_geometry_count >= 10) && b49_4) || (rec->normalized_coverage < 70 && t0 < 450 && rec->secondary_geometry_count < 10 && b49_5);
                          const gboolean b49_7 = rec->geometry_percent < 30 || b49_6;
                          const gboolean b49_8 = ((rec->normalized_coverage >= 100 || t0 >= 440 || rec->secondary_geometry_count >= 16) && b49_6) || (rec->normalized_coverage < 100 && t0 < 440 && rec->secondary_geometry_count < 16 && b49_7);
                          const gboolean b49_9 = ((rec->normalized_coverage >= 115 || t0 >= 435 || rec->secondary_geometry_count >= 13) && b49_8) || (rec->normalized_coverage < 115 && t0 < 435 && rec->secondary_geometry_count < 13 && (rec->geometry_percent < 35 || b49_8));
                          const gboolean b49_10 = (t3 <= 185 && b49_2) || (t3 > 185 && b49_9);
                          const gboolean b49_11 = rec->geometry_percent < 50 || b49_10;
                          const gboolean b49_12 = ((rec->normalized_coverage >= 85 || t0 >= 415 || rec->secondary_geometry_count >= 19) && b49_10) || (rec->normalized_coverage < 85 && t0 < 415 && rec->secondary_geometry_count < 19 && b49_11);
                          if ((t3 <= 180 && b49_10) || (t3 > 180 && (((rec->normalized_coverage >= 128 || t0 >= 425 || rec->secondary_geometry_count >= 18) && b49_12) || (rec->normalized_coverage < 128 && t0 < 425 && rec->secondary_geometry_count < 18 && (rec->geometry_percent < 35 || b49_12)))))
                            {
                              *status = 0;
                              *conf = 0;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* Status rescue (type 24, study flag set): a record without status may get status 1 (and conf 1)
* when its penalized agreement/study metric and coverage clear thresholds indexed by the geometry
* counts; otherwise conf is cleared.  *status_out receives the first test, then |= the second. */
static void
match_reeval_status_rescue_type24 (const GoodixChicagoMatchScoreRecord *rec,
                                   gint32                              *conf,
                                   gint32                              *status_out)
{
  static const gint32 study_min[10] = { 0x0fffffff, 207, 205, 202, 201, 195, 185, 185, 185, 185 };
  static const gint32 agreement_min[10] = { 0x0fffffff, 203, 203, 203, 203, 202, 201, 200, 192, 184 };
  static const gint32 coverage_min[10] = { 0x0fffffff, 95, 88, 82, 75, 65, 55, 55, 55, 55 };
  const gint32 primary = rec->geometry_count;
  const gint32 secondary = rec->secondary_geometry_count;
  const gint32 coverage = rec->normalized_coverage;
  const gint32 penalty = (secondary <= 10 || primary < 4) ?
                         5 * ((primary < 4) + rec->penalty_flag_b) : 0;
  const gint32 study = rec->study_metric_20 - penalty;
  const gint32 agreement = rec->agreement - penalty;
  const gint32 k = CLAMP (secondary - 4, 0, 9);   /* no threshold shifts */
  const gint32 coverage_limit = MIN (coverage_min[k] + 10, 100);

  *status_out = study >= study_min[k] && agreement >= agreement_min[k] && coverage >= coverage_min[k];
  if ((primary >= 6 || secondary >= 16 ||
       (primary >= 5 && rec->matched_percent > 55 && coverage >= 125)) &&
      study >= study_min[k] + 2 && agreement >= 206 && coverage >= coverage_limit)
    {
      *conf = 1;
      *status_out |= 1;
    }
  else
    {
      *conf = 0;
    }
}

/* For type 24: status = 0 or conf = 0 -> evidence without the coverage
 * penalty, OR-ed in; still no status -> status rescue; then the conf rules if conf is
 * set and the status rules if status is set. */
static void
match_reevaluate_flags_type24 (const GoodixChicagoMatchScoreRecord *rec,
                               gint32                               probe_quality,
                               gint32                               probe_coverage,
                               guint32                              probe_resolution,
                               gint32                              *conf,
                               gint32                              *status)
{
  if (*status == 0 || *conf == 0)
    {
      GoodixChicagoMatchSchedulerEvidence evidence;

      goodix_chicago_match_scheduler_evidence_type24 (rec, probe_quality, probe_coverage, FALSE,
                                                      &evidence);
      *status |= evidence.status;
      *conf |= evidence.confidence;
    }
  if (*status == 0)
    {
      gint32 rescued = 0;

      match_reeval_status_rescue_type24 (rec, conf, &rescued);
      *status |= rescued;
    }
  if (*conf != 0)
    match_reeval_conf_tree_type24 (rec, conf);
  if (*status != 0)
    match_reeval_status_tree_type24 (rec, (gint32) probe_resolution, conf, status);
}

/* Decision logic of the identify scheduler, with flag = 0 and study flag = 1
 * ("continue" = 1).
 *
 * - A gallery record that passes the admission gate (status 1, or
 *   selector > 207 && agreement > 195) adds (geometry*256 + 15)/31 to the Q8
 *   average, but the signed ratio is written only for status == 1.
 *   Admission with status 0 alone never makes a positive score inside the
 *   loop.
 * - The loop stops after a status-1 record only when the continue flag is 0:
 *   ((probe packed_resolution >> 8) & 7) >= 3, or
 *   after the rejection-count raise.  It also stops when more
 *   than 10 late rejections were counted.
 * - After the loop: no continue flag -> "strong"
 *   flag cleared; live_auxiliary[0] >= 6 without strong -> ratio 0;
 *   ratio 0 and (aux >= 4 or live_auxiliary[0] >= 2) -> 0; otherwise, with
 *   admitted records and packed_resolution == 0, the best admitted record is
 *   re-checked; otherwise the fallback. */
static gint32
match_best_record_check_type24 (const GoodixChicagoMatchScoreRecord *record)
{
  /* Best admitted record check, type 24. */
  const gint32 primary = record->geometry_count;
  const gint32 secondary = record->secondary_geometry_count;
  const gint32 geometry_percent = record->geometry_percent;
  const gint32 penalty = 3 * ((primary < 5) + record->penalty_flag_a +
                              record->penalty_flag_b + record->reserved38[0]);
  const gint32 sum = (record->agreement - penalty) +
                     (record->study_metric_20 - penalty);

  if (geometry_percent < 10 ||
      record->gallery_quality + record->probe_quality > 170)
    return 0;
  if ((primary >= 5 && secondary >= 8 && sum >= 425 && geometry_percent >= 42) ||
      (primary >= 3 && record->selector >= 235 &&
       record->normalized_coverage >= 120 && sum >= 400) ||
      (primary >= 6 && record->normalized_coverage > 62 && sum >= 415 &&
       geometry_percent >= 41))
    return (((secondary << 8) + 15) / 31 * 100) >> 8;
  return 0;
}

static gboolean
match_best_record_replace_type24 (const GoodixChicagoMatchScoreRecord *best,
                                  const GoodixChicagoMatchScoreRecord *record)
{
  /* Agreement, then secondary geometry, then normalized coverage. */
  if (record->agreement != best->agreement)
    return record->agreement > best->agreement;
  if (record->secondary_geometry_count != best->secondary_geometry_count)
    return record->secondary_geometry_count > best->secondary_geometry_count;
  return record->normalized_coverage > best->normalized_coverage;
}

/* Fallback for type 24 (study flag set).  Over the records enabled, in
 * template order: the fallback correspondences, geometry with > 5 pairs and
 * >= 4 inliers; with > 4 inliers the record counts as evaluated and gets the
 * full metric (inlier-gated selector); it is admitted with selector > threshold and
 * agreement > 195, and its agreement is then penalised by 4 per scale/shear flag.
 * The best admitted record (agreement, inliers, coverage; first wins ties) would be
 * re-checked, but its geometry_percent is never set here, so
 * the check returns 0 and the result is always the negative reason code:
 * (inlier sum < 6) << 2 | (evaluated && max admitted agreement < 208) << 1 |
 * (evaluated && best coverage < 128).  The transform admissibility test for
 * probe resolution classes 1..5 is not implemented. */
static gint32
match_fallback_type24 (const GoodixChicagoEnrollment      *gallery_template,
                       guint                               gallery_count,
                       const GoodixChicagoSubtemplateView *probe,
                       const gboolean                     *enabled,
                       gint32                              selector_threshold)
{
  gboolean evaluated = FALSE;
  gint32 inlier_sum = 0, max_agreement = 0;
  gint32 best_agreement = 0, best_inliers = 0, best_coverage = 0;

  for (guint index = 0; index < gallery_count; index++)
    {
      GoodixChicagoSubtemplateView gallery;
      GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
      GoodixChicagoMatchGeometry geometry;
      gint32 selector, agreement, coverage, flag_a, flag_b, flag_c, angle;
      guint valid = 0;
      gint32 inliers;

      if (!enabled[index] ||
          !goodix_chicago_enrollment_get_subtemplate (gallery_template, index, &gallery))
        continue;
      goodix_chicago_match_fallback_correspondences (
        gallery.records, gallery.record_count, gallery.active_count,
        probe->records, probe->record_count, probe->active_count, pairs);
      for (guint k = 0; k < GOODIX_CHICAGO_MATCH_PAIR_LIMIT; k++)
        valid += pairs[k].old_index >= 0 && pairs[k].new_index >= 0 &&
                 (guint) pairs[k].old_index < gallery.record_count &&
                 (guint) pairs[k].new_index < probe->record_count;
      if (valid <= 5)
        continue;
      goodix_chicago_match_geometry_consensus_records (
        gallery.records, gallery.record_count, probe->records, probe->record_count,
        pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 4, &geometry);
      inliers = geometry.inlier_count;
      if (inliers <= 4 || !gallery.has_metric_data || !probe->has_metric_data)
        continue;
      evaluated = TRUE;
      match_record_metrics_type24 (&gallery, probe, geometry.transform, inliers,
                                   &selector, &agreement, &coverage,
                                   &flag_a, &flag_b, &flag_c, &angle);
      if (selector <= selector_threshold || agreement <= 195)
        continue;
      agreement -= 4 * (flag_a + flag_b);
      if (agreement > best_agreement ||
          (agreement == best_agreement &&
           (inliers > best_inliers ||
            (inliers == best_inliers && coverage > best_coverage))))
        {
          best_agreement = agreement;
          best_inliers = inliers;
          best_coverage = coverage;
        }
      inlier_sum += inliers;
      max_agreement = MAX (max_agreement, agreement);
    }
  return -(((inlier_sum < 6) << 2) |
           ((evaluated && max_agreement < 208) << 1) |
           (evaluated && best_coverage < 128));
}


/* First evidence of the type-24 scheduler.  Its status/conf only decide whether the transform refinement runs. */
static void
match_first_evidence_type24 (const GoodixChicagoMatchScoreRecord *rec,
                             gint32 quality, gint32 coverage, gint32 *conf, gint32 *status)
{
  static const gint32 t1[22] = { 0x0fffffff, 0x0fffffff, 0x0fffffff, 226, 224, 224, 221, 215, 212, 208, 204,
                                 200, 195, 190, 190, 190, 180, 180, 180, 180, 180, 180 };
  static const gint32 t2[22] = { 0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff, 226, 222, 215,
                                 213, 210, 205, 203, 200, 198, 197, 195, 193, 191, 184, 182, 182, 181 };
  const gint32 b = rec->normalized_coverage;
  const gint32 c = rec->matched_percent;
  gint32 g0 = rec->geometry_count, g1 = rec->secondary_geometry_count;
  gint32 e = rec->agreement, r = rec->agreement, i0, i1, st, cf;

  if (b >= 80 && c >= 70 && g0 > 4)
    {
      g0 += 1 + (c - 70) / 5;
      g1 += 1 + (c - 70) / 5;
    }
  if (g1 <= 11)
    e = r = rec->agreement - 4 * (rec->penalty_flag_a + rec->penalty_flag_b + rec->reserved38[0]);
  if (b > 100)
    r += 2 * ((b - 100) / 30) + 2;
  i0 = CLAMP (g0, 0, 21);
  i1 = CLAMP (g1, 0, 21);
  st = b >= 25 && (r > t1[i0] || r > t1[i1] + 10 || g0 >= 14);
  cf = st && quality > 15 && coverage >= 65 && b >= 40 && (e > t2[i0] || e > t2[i1] + 10);
  if (cf == 1 && rec->geometry_count > 7 && rec->secondary_geometry_count > 11 &&
      rec->geometry_percent > 37 && b > 95)
    cf = 2;
  *status = st;
  *conf = cf;
}

/* Index map of the probe records (radius 2, later records win). */
static void
match_render_record_map (gint16 *map, const GoodixChicagoFeatureRecord *recs, guint count)
{
  const gint w = GOODIX_CHICAGO_FEATURE_WIDTH, h = GOODIX_CHICAGO_FEATURE_HEIGHT, rad = 2;

  for (gint k = 0; k < w * h; k++)
    map[k] = -1;
  for (guint k = 0; k < count; k++)
    {
      const gint x = ((gint) (guint16) recs[k].refined_x + 128) >> 8;
      const gint y = ((gint) (guint16) recs[k].refined_y + 128) >> 8;
      const gint x0 = MAX (x - rad, 0), x1 = MIN (x + rad, w - 1);
      const gint y0 = MAX (y - rad, 0), y1 = MIN (y + rad, h - 1);

      for (gint yy = y0; yy <= y1; yy++)
        for (gint xx = x0; xx <= x1; xx++)
          map[yy * w + xx] = (gint16) k;
    }
}

/* Gallery records mapped into the probe by the inverse transform; a hit
 * in the probe map gives the pair {gallery, probe}. */
static guint
match_map_pairs (const GoodixChicagoFeatureRecord *recs, guint count, const gint32 transform[6],
                 const gint16 *map, gint32 (*pairs)[2])
{
  const gint w = GOODIX_CHICAGO_FEATURE_WIDTH, h = GOODIX_CHICAGO_FEATURE_HEIGHT;
  gint32 t[6];
  guint n = 0;

  goodix_chicago_match_invert_transform_q8_type24 (transform, t);
  for (guint k = 0; k < count; k++)
    {
      const gint64 x = (guint16) recs[k].refined_x, y = (guint16) recs[k].refined_y;
      const gint32 X = (gint32) (t[2] + 128 + (gint32) (((gint64) t[0] * x + 128 + (gint64) t[1] * y) >> 8)) >> 8;
      gint32 Y;

      if (X < 0 || X >= w)
        continue;
      Y = (gint32) (t[5] + 128 + (gint32) (((gint64) t[3] * x + 128 + (gint64) t[4] * y) >> 8)) >> 8;
      if (Y < 0 || Y >= h || map[Y * w + X] < 0)
        continue;
      pairs[n][0] = k;
      pairs[n][1] = map[Y * w + X];
      n++;
    }
  return n;
}

/* Similarity transform (probe -> gallery) fitted to the pairs, Q8. */
static void
match_fit_similarity (const GoodixChicagoFeatureRecord *grecs, const GoodixChicagoFeatureRecord *precs,
                      gint32 (*pairs)[2], gint64 n, gint32 out[6])
{
  gint32 spx = 0, spy = 0, sgx = 0, sgy = 0;
  gint64 spp = 0, s1 = 0, s2 = 0, mxp, myp, mxg, myg, r10, d, inv, r11, a, b;

  for (gint64 i = 0; i < n; i++)
    {
      const gint64 px = (guint16) precs[pairs[i][1]].refined_x, py = (guint16) precs[pairs[i][1]].refined_y;
      const gint64 gx = (guint16) grecs[pairs[i][0]].refined_x, gy = (guint16) grecs[pairs[i][0]].refined_y;

      spx += (gint32) px;
      spy += (gint32) py;
      sgx += (gint32) gx;
      sgy += (gint32) gy;
      spp += px * px + py * py;
      s1 += gx * px + gy * py;
      s2 += gy * px - gx * py;
    }
  mxp = (gint64) (gint32) (spx + 0x80) >> 8;
  myp = (gint64) (gint32) (spy + 0x80) >> 8;
  mxg = (gint64) (gint32) (sgx + 0x80) >> 8;
  myg = (gint64) (gint32) (sgy + 0x80) >> 8;
  s2 = (s2 + 0x8000) >> 16;
  spp = (spp + 0x8000) >> 16;
  s1 = (s1 + 0x8000) >> 16;
  r10 = myp * myp + mxp * mxp;
  d = spp * n - r10;
  if (d == 0)
    return;
  inv = ((d >> 1) + 0x800000000LL) / d;
  r11 = (inv * r10 + 0x800000000LL) / n;
  a = (inv * (n * s1 - myg * myp - mxg * mxp)) >> 27;
  b = (inv * (n * s2 - myg * mxp + mxg * myp)) >> 27;
  out[0] = out[4] = (gint32) a;
  out[3] = (gint32) b;
  out[1] = (gint32) - b;
  out[2] = (gint32) ((-inv * mxp * s1 + inv * myp * s2 + r11 * mxg) >> 27);
  out[5] = (gint32) ((-inv * mxp * s2 - inv * myp * s1 + r11 * myg) >> 27);
}

/* Transform refinement (type 24) from the scheduler: the pairs of the current
 * transform within the class maps give a refitted similarity; when its metrics
 * have a higher agreement, selector/agreement/coverage, the overlap percentages
 * and the transform are replaced. */
static void
match_refine_transform_type24 (const GoodixChicagoSubtemplateView *gallery,
                               const GoodixChicagoSubtemplateView *probe,
                               GoodixChicagoMatchScoreRecord      *rec,
                               gint32                              transform[6],
                               gint16                            **maps)
{
  gint32 pairs[512][2];
  gint32 refined[6];
  guint n1, n2;
  gint32 sel, agr, cov, d0, d1, d2, d3;
  GoodixChicagoMatchFeatureOverlap overlap;
  const gsize px = GOODIX_CHICAGO_FEATURE_WIDTH * GOODIX_CHICAGO_FEATURE_HEIGHT;

  if (*maps == NULL)
    {
      *maps = g_new (gint16, 2 * px);
      match_render_record_map (*maps, probe->records, probe->active_count);
      match_render_record_map (*maps + px, probe->records + probe->active_count,
                               probe->record_count - probe->active_count);
    }
  n1 = match_map_pairs (gallery->records, gallery->active_count, transform, *maps, pairs);
  n2 = match_map_pairs (gallery->records + gallery->active_count,
                        gallery->record_count - gallery->active_count, transform, *maps + px, pairs + n1);
  for (guint k = n1; k < n1 + n2; k++)
    {
      pairs[k][0] += gallery->active_count;
      pairs[k][1] += probe->active_count;
    }
  memcpy (refined, transform, sizeof (refined));
  if (n1 + n2 > 2)
    match_fit_similarity (gallery->records, probe->records, pairs, n1 + n2, refined);
  match_record_metrics_type24 (gallery, probe, refined, rec->secondary_geometry_count,
                               &sel, &agr, &cov, &d0, &d1, &d2, &d3);
  if (sel == 0x80 || !(rec->selector == 0x80 || agr > rec->agreement))
    return;
  rec->selector = sel;
  rec->agreement = agr;
  rec->normalized_coverage = cov;
  goodix_chicago_match_feature_overlap_type24 (
    gallery->records, gallery->record_count, probe->records, probe->record_count, 80, 64,
    refined, rec->secondary_geometry_count, &overlap);
  rec->matched_percent = overlap.matched_percent;
  rec->geometry_percent = overlap.geometry_percent;
  memcpy (transform, refined, sizeof (refined));
}

void
goodix_chicago_match_template_type24 (
  const GoodixChicagoEnrollment      *gallery_template,
  const GoodixChicagoSubtemplateView *probe,
  gint32                              selector_threshold,
  GoodixChicagoMatchTemplateResult   *result)
{
  GoodixChicagoMatchScoreRecord records[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  GoodixChicagoMatchGeometry geometries[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  gint32 auxiliary_counts[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { 0, };
  gboolean auxiliary_counts_known[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { FALSE, };
  gboolean fallback_enabled[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { FALSE, };
  gboolean admitted[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { FALSE, };
  g_autofree gint16 *refine_maps = NULL;
  GoodixChicagoMatchSchedulerAuxiliary scheduler_auxiliary;
  GoodixChicagoMatchScoreRecord best_record;
  gboolean continue_after_match;
  gboolean any_status = FALSE;
  gboolean any_strong = FALSE;
  gboolean group_hit = FALSE;    /* a status record of a grouped subtemplate */
  gint32 admitted_count = 0;
  gint32 admitted_q8_sum = 0;
  gint32 best_geometry = 0;
  gint32 candidate_metric;
  gint32 rejection_count = 0;
  guint32 probe_resolution;
  guint gallery_count;

  g_return_if_fail (gallery_template != NULL);
  g_return_if_fail (probe != NULL);
  g_return_if_fail (result != NULL);
  *result = (GoodixChicagoMatchTemplateResult){
    .selected_index = -1,
  };
  gallery_count = goodix_chicago_enrollment_get_count (gallery_template);
  g_return_if_fail (gallery_count <= G_N_ELEMENTS (records));
  result->study_relation_count = gallery_count;
  memset (records, 0, sizeof (records));
  memset (geometries, 0, sizeof (geometries));
  memset (&best_record, 0, sizeof (best_record));
  candidate_metric = probe->live_auxiliary.values[0];
  probe_resolution = probe->metric_data->packed_resolution;
  continue_after_match = ((probe_resolution >> 8) & 7) < 3;
  goodix_chicago_match_scheduler_auxiliary_init_type24 (
    probe_resolution, &scheduler_auxiliary);
  for (guint schedule_index = 0; schedule_index < gallery_count;
       schedule_index++)
    {
      GoodixChicagoSubtemplateView gallery;
      GoodixChicagoMatchSchedulerEvidence evidence;
      GoodixChicagoMatchStudyAdmissionInput study_input;
      gint32 combined_auxiliary;
      gint32 status_flag;
      gint32 study_eligible;
      gint32 count_zero = 0;
      gint32 count_mixed = 0;
      gint32 count_one = 0;
      gint32 aggregate_metric;
      gboolean was_raised;
      gboolean prefiltered;
      guint index;

      if (!goodix_chicago_enrollment_get_match_order_index (
            gallery_template, schedule_index, &index) ||
          !goodix_chicago_enrollment_get_subtemplate (
            gallery_template, index, &gallery))
        continue;
      /* Once a grouped subtemplate has given a status record, the
       * other grouped ones are skipped (with a full template, or after a confident one). */
      if ((gallery_count == GOODIX_CHICAGO_ENROLLMENT_CAPACITY || any_strong) &&
          group_hit && gallery.group_state == 1)
        continue;
      was_raised = scheduler_auxiliary.raised;
      combined_auxiliary =
        goodix_chicago_match_scheduler_auxiliary_consume_type24 (
          &scheduler_auxiliary, gallery.metric_data->packed_resolution,
          rejection_count);
      if (!was_raised && scheduler_auxiliary.raised)
        continue_after_match = FALSE;
      else if (rejection_count > 10)
        break;
      auxiliary_counts[index] = scheduler_auxiliary.auxiliary_count;
      auxiliary_counts_known[index] = TRUE;
      goodix_chicago_match_score_subtemplate_type24 (
        &gallery, probe, &records[index], &geometries[index]);
      result->study_relations[index].inlier_count = -2;
      /* Fewer than five inliers or matched_percent < 30 skip
       * the candidate; the fallback may still use it. */
      if (records[index].secondary_geometry_count <= 4 ||
          records[index].matched_percent < 30)
        {
          if (!any_status && records[index].secondary_geometry_count >= 3)
            fallback_enabled[index] = TRUE;
          continue;
        }
      /* The prefilter runs before the study metrics
       * (study_metric_18..20) are written, so they are still zero. */
      {
        GoodixChicagoMatchScoreRecord early = records[index];

        early.study_metric_18 = early.study_metric_1c = early.study_metric_20 = 0;
        prefiltered = goodix_chicago_match_candidate_prefilter_type24 (
          &early, scheduler_auxiliary.auxiliary_count, candidate_metric);
      }
      if (prefiltered)
        continue;
      {
        /* First evidence; selector < 207 or
         * status 0 or conf 0 -> transform refinement. */
        gint32 first_conf, first_status;

        match_first_evidence_type24 (&records[index], probe->quality, probe->coverage,
                                     &first_conf, &first_status);
        if (records[index].selector < 207 || first_status == 0 || first_conf == 0)
          match_refine_transform_type24 (&gallery, probe, &records[index],
                                         geometries[index].transform, &refine_maps);
      }
      goodix_chicago_match_scheduler_evidence_type24 (
        &records[index], probe->quality, probe->coverage, TRUE, &evidence);
      status_flag = evidence.status;
      study_eligible = evidence.confidence;
      /* Unless the evidence is already confident (conf 2) with a low auxiliary
       * count, good agreement (or a poor probe) and coverage >= 105, the study metrics are
       * computed, the flags re-evaluated and "special" evidence
       * with study_metric_20 < 175 clears both.  On the skip path the study
       * metrics are not computed: the record keeps zeros. */
      if (study_eligible == 2 && scheduler_auxiliary.auxiliary_count <= 1 &&
          (records[index].agreement >= 205 || probe->quality <= 90) &&
          records[index].normalized_coverage >= 105)
        {
          records[index].study_metric_18 = records[index].study_metric_1c =
            records[index].study_metric_20 = 0;
        }
      else
        {
          match_reevaluate_flags_type24 (&records[index], probe->quality, probe->coverage,
                                         probe_resolution, &study_eligible, &status_flag);
          if (evidence.special && records[index].study_metric_20 < 175)
            status_flag = study_eligible = 0;
        }
      if (goodix_chicago_match_late_rejection_type24 (
            80, 64, probe->quality, &records[index],
            geometries[index].transform,
            scheduler_auxiliary.auxiliary_count, candidate_metric,
            combined_auxiliary, &rejection_count, &status_flag))
        continue;
      if (candidate_metric >= 3)
        goodix_chicago_enrollment_calculate_live_auxiliary_counts_type24 (
          probe->live_auxiliary.values, geometries[index].transform,
          &count_zero, &count_mixed, &count_one);
      aggregate_metric =
        goodix_chicago_match_study_aggregate_q8_type24 (
          count_zero, count_mixed, count_one);
      study_input = (GoodixChicagoMatchStudyAdmissionInput){
        .auxiliary_count = scheduler_auxiliary.auxiliary_count,
        .candidate_metric = candidate_metric,
        .probe_quality = probe->quality,
        .probe_auxiliary = probe->density_positive_percent,
        .aggregate_metric = aggregate_metric,
      };
      goodix_chicago_match_filter_study_admission_type24 (
        &records[index], &study_input, &status_flag, &study_eligible);
      if (status_flag == 1)
        {
          /* Status-1 geometry for template study. */
          result->study_relations[index].inlier_count =
            geometries[index].inlier_count;
          memcpy (result->study_relations[index].transform,
                  geometries[index].transform,
                  sizeof (result->study_relations[index].transform));
        }
      if (goodix_chicago_match_accept_type24 (
            records[index].selector, records[index].agreement, status_flag,
            selector_threshold))
        {
          gboolean replace;

          admitted[index] = TRUE;
          admitted_count++;
          admitted_q8_sum += match_divide_nearest (
            records[index].secondary_geometry_count * 256, 31);
          if (any_strong)
            {
              replace = study_eligible != 0 &&
                        match_best_record_replace_type24 (&best_record,
                                                          &records[index]);
            }
          else if (any_status)
            {
              replace = study_eligible != 0 ||
                        (status_flag != 0 &&
                         match_best_record_replace_type24 (&best_record,
                                                           &records[index]));
            }
          else
            {
              replace = study_eligible != 0 || status_flag != 0 ||
                        match_best_record_replace_type24 (&best_record,
                                                          &records[index]);
            }
          if (replace)
            best_record = records[index];
        }
      any_status |= status_flag != 0;
      any_strong |= study_eligible != 0;
      group_hit |= status_flag != 0 && gallery.group_state != 0;
      if (status_flag == 0)
        {
          if (!any_status && records[index].secondary_geometry_count >= 3)
            fallback_enabled[index] = TRUE;
          continue;
        }
      if (records[index].secondary_geometry_count > best_geometry)
        {
          best_geometry = records[index].secondary_geometry_count;
          result->selected_index = index;
          result->study_auxiliary_count = auxiliary_counts[index];
          result->study_auxiliary_count_known = auxiliary_counts_known[index];
          result->study_candidate_metric = candidate_metric;
        }
      result->hit_increments[index]++;
      result->score = ((admitted_q8_sum * 100) / admitted_count) >> 8;
      if (!continue_after_match)
        break;
    }
  if (!continue_after_match)
    any_strong = FALSE;
  if (candidate_metric >= 6 && !any_strong)
    result->score = 0;
  if (result->score != 0 ||
      scheduler_auxiliary.auxiliary_count >= 4 || candidate_metric >= 2)
    goto done;
  if (admitted_count > 0 && probe_resolution == 0 &&
      match_best_record_check_type24 (&best_record) > 0)
    {
      /* The average of all admitted records. */
      for (guint index = 0; index < gallery_count; index++)
        if (admitted[index])
          result->hit_increments[index]++;
      result->score = ((admitted_q8_sum * 100) / admitted_count) >> 8;
      goto done;
    }

  /* The fallback. */
  result->score = match_fallback_type24 (gallery_template, gallery_count, probe,
                                         fallback_enabled, selector_threshold);
done:
  if (result->score > 0)
    {
      result->selected_index_known = result->selected_index >= 0;
      /* Template study tests the strong flag, which clears when the
       * loop stopped at the first match. */
      result->study_eligible = any_strong;
      result->study_eligibility_known = TRUE;
    }
}

guint
goodix_chicago_match_fallback_correspondences (
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  guint                             gallery_split,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  guint                             probe_split,
  GoodixChicagoMatchPair            pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT])
{
  g_autofree GoodixChicagoMatchCandidate *row_candidates = NULL;
  g_autofree GoodixChicagoMatchCandidate *column_candidates = NULL;
  g_autofree GoodixChicagoMatchPair *probe_gallery_pairs = NULL;
  g_autofree guint8 *distance_matrix = NULL;
  g_autofree guint8 *direction_matrix = NULL;
  GoodixChicagoMatchCandidateConfig config;
  gsize matrix_size;
  guint selected_count;

  g_return_val_if_fail (gallery_count == 0 || gallery_records != NULL, 0);
  g_return_val_if_fail (probe_count == 0 || probe_records != NULL, 0);
  g_return_val_if_fail (gallery_count <= GOODIX_CHICAGO_MATCH_MATRIX_STRIDE,
                        0);
  g_return_val_if_fail (probe_count <= GOODIX_CHICAGO_MATCH_MATRIX_STRIDE,
                        0);
  g_return_val_if_fail (gallery_split <= gallery_count, 0);
  g_return_val_if_fail (probe_split <= probe_count, 0);
  g_return_val_if_fail (pairs != NULL, 0);

  matrix_size = gallery_count * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE;
  row_candidates = g_new (GoodixChicagoMatchCandidate, gallery_count);
  column_candidates = g_new (GoodixChicagoMatchCandidate, probe_count);
  probe_gallery_pairs = g_new (GoodixChicagoMatchPair,
                               GOODIX_CHICAGO_MATCH_PAIR_LIMIT);
  distance_matrix = g_malloc (MAX (matrix_size, (gsize) 1));
  direction_matrix = g_malloc0 (MAX (matrix_size, (gsize) 1));
  memset (distance_matrix, 0xff, MAX (matrix_size, (gsize) 1));
  goodix_chicago_match_init_candidates (row_candidates, gallery_count);
  config = (GoodixChicagoMatchCandidateConfig){
    0, gallery_split, 0, probe_split, 23, 47,
  };
  goodix_chicago_match_update_candidates (
    gallery_records, probe_records, &config, row_candidates,
    distance_matrix, direction_matrix);
  config = (GoodixChicagoMatchCandidateConfig){
    gallery_split, gallery_count, probe_split, probe_count, 23, 47,
  };
  goodix_chicago_match_update_candidates (
    gallery_records, probe_records, &config, row_candidates,
    distance_matrix, direction_matrix);

  goodix_chicago_match_init_candidates (column_candidates, probe_count);
  for (guint gallery_index = 0; gallery_index < gallery_split; gallery_index++)
    for (guint probe_index = 0; probe_index < probe_split; probe_index++)
      update_column_candidate (
        &column_candidates[probe_index], gallery_index,
        distance_matrix[gallery_index * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE +
                        probe_index]);
  for (guint gallery_index = gallery_split;
       gallery_index < gallery_count;
       gallery_index++)
    for (guint probe_index = probe_split;
         probe_index < probe_count;
         probe_index++)
      update_column_candidate (
        &column_candidates[probe_index], gallery_index,
        distance_matrix[gallery_index * GOODIX_CHICAGO_MATCH_MATRIX_STRIDE +
                        probe_index]);

  selected_count = goodix_chicago_match_select_candidates (
    gallery_records, column_candidates, probe_count,
    GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 40, 38, probe_gallery_pairs);
  for (guint index = 0; index < GOODIX_CHICAGO_MATCH_PAIR_LIMIT; index++)
    pairs[index] = (GoodixChicagoMatchPair){
      probe_gallery_pairs[index].new_index,
      probe_gallery_pairs[index].old_index,
    };
  return selected_count;
}
