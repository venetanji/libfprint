// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

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

void
goodix_chicago_match_aggregation_add_geometry (
  GoodixChicagoMatchAggregation *aggregation,
  gint32                         geometry_count,
  gint32                         geometry_limit)
{
  g_return_if_fail (aggregation != NULL);
  g_return_if_fail (geometry_count >= 0);
  g_return_if_fail (geometry_limit > 0);
  aggregation->accepted_count++;
  aggregation->accepted_geometry_q8_sum += match_divide_nearest (
    geometry_count * 256, geometry_limit);
}

gint32
goodix_chicago_match_aggregation_score (
  const GoodixChicagoMatchAggregation *aggregation)
{
  gint32 reason;

  g_return_val_if_fail (aggregation != NULL, 0);
  if (aggregation->accepted_count > 0)
    return ((aggregation->accepted_geometry_q8_sum * 100) /
            aggregation->accepted_count) >> 8;
  if (aggregation->fallback_score > 0)
    return MIN (aggregation->fallback_score, 100);

  reason = (aggregation->fallback_geometry_sum < 6) << 2;
  if (aggregation->fallback_evaluated_count == 1)
    {
      reason |= (aggregation->fallback_max_metric < 208) << 1;
      reason |= aggregation->fallback_best_coverage < 128;
    }
  return -reason;
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

gboolean
goodix_chicago_match_aggregation_consume_type24 (
  GoodixChicagoMatchAggregation       *aggregation,
  const GoodixChicagoMatchScoreRecord *record,
  gint32                               quality_a,
  gint32                               quality_b,
  gint32                               selector_threshold)
{
  GoodixChicagoMatchSchedulerEvidence evidence;

  g_return_val_if_fail (aggregation != NULL, FALSE);
  g_return_val_if_fail (record != NULL, FALSE);
  goodix_chicago_match_scheduler_evidence_type24 (
    record, quality_a, quality_b, TRUE, &evidence);
  if (!goodix_chicago_match_accept_type24 (
        record->selector, record->agreement, evidence.status,
        selector_threshold))
    return FALSE;
  goodix_chicago_match_aggregation_add_geometry (
    aggregation, record->secondary_geometry_count, 31);
  return TRUE;
}

gboolean
goodix_chicago_match_aggregation_consume_fallback_type24 (
  GoodixChicagoMatchAggregation *aggregation,
  gint32                         geometry_count,
  gint32                         selector,
  gint32                         agreement,
  gint32                         coverage,
  gint32                         selector_threshold)
{
  g_return_val_if_fail (aggregation != NULL, FALSE);
  if (geometry_count <= 4)
    return FALSE;
  aggregation->fallback_evaluated_count = 1;
  if (selector <= selector_threshold || agreement <= 195)
    return FALSE;

  aggregation->fallback_geometry_sum += geometry_count;
  aggregation->fallback_max_metric =
    MAX (aggregation->fallback_max_metric, agreement);
  if (agreement > aggregation->fallback_best_metric ||
      (agreement == aggregation->fallback_best_metric &&
       geometry_count > aggregation->fallback_best_geometry) ||
      (agreement == aggregation->fallback_best_metric &&
       geometry_count == aggregation->fallback_best_geometry &&
       coverage > aggregation->fallback_best_coverage))
    {
      aggregation->fallback_best_metric = agreement;
      aggregation->fallback_best_geometry = geometry_count;
      aggregation->fallback_best_coverage = coverage;
    }
  return TRUE;
}

gboolean
goodix_chicago_match_aggregation_consume_fallback_records_type24 (
  GoodixChicagoMatchAggregation    *aggregation,
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  guint                             gallery_split,
  const GoodixChicagoMetricData    *gallery_metric_data,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  guint                             probe_split,
  const GoodixChicagoMetricData    *probe_metric_data,
  gint32                            selector_threshold,
  GoodixChicagoMatchFallbackResult *result)
{
  GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
  GoodixChicagoMatchFallbackResult local = { 0, };

  g_return_val_if_fail (aggregation != NULL, FALSE);
  g_return_val_if_fail (gallery_count == 0 || gallery_records != NULL, FALSE);
  g_return_val_if_fail (probe_count == 0 || probe_records != NULL, FALSE);
  local.correspondence_count =
    goodix_chicago_match_fallback_correspondences (
      gallery_records, gallery_count, gallery_split,
      probe_records, probe_count, probe_split, pairs);
  goodix_chicago_match_geometry_consensus_records (
    gallery_records, gallery_count, probe_records, probe_count,
    pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 4, &local.geometry);
  if (local.geometry.inlier_count > 4)
    {
      g_return_val_if_fail (gallery_metric_data != NULL, FALSE);
      g_return_val_if_fail (probe_metric_data != NULL, FALSE);
      local.metric_evaluated = TRUE;
      local.selector =
        goodix_chicago_enrollment_calculate_config1_metrics (
          probe_metric_data, gallery_metric_data, local.geometry.transform,
          &local.agreement, &local.coverage);
    }
  local.accepted =
    goodix_chicago_match_aggregation_consume_fallback_type24 (
      aggregation, local.geometry.inlier_count, local.selector,
      local.agreement, local.coverage, selector_threshold);
  if (result != NULL)
    *result = local;
  return local.accepted;
}

gboolean
goodix_chicago_match_fallback_gallery_enabled_type24 (
  gint32 primary_geometry_count,
  gint32 scratch_state,
  gint32 gallery_state)
{
  return scratch_state == 0 && gallery_state == 1 &&
         primary_geometry_count >= 3;
}

void
goodix_chicago_match_init_score_config_type24 (
  GoodixChicagoMatchScoreConfig *config)
{
  static const gint32 values[22] = {
    0, 0, 5, 218, 0, 0, 23, 47, 40, 38, -1, 16,
    137, 1, 0, 24, 1, 0, 0, 0, 0, 0,
  };

  g_return_if_fail (config != NULL);
  memcpy (config->values, values, sizeof (values));
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
          numerator += cofactor[row][coefficient] * cross[row][axis];
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
  const gint64 a = transform[0];
  const gint64 b = transform[1];
  const gint64 c = transform[3];
  const gint64 d = transform[4];
  const gint64 m00 = a * a + b * b;
  const gint64 m01 = a * c + b * d;
  const gint64 m11 = c * c + d * d;
  const gint64 trace = m00 + m11;
  const gint64 discriminant = trace * trace +
                              4 * (m01 * m01 - m11 * m00);
  const gint64 upper_delta = ((gint64) 0xa3 << 9) - trace;
  const gint64 lower_delta = ((gint64) 0x191 << 9) - trace;

  return ABS (transform[0] - transform[4]) < 0x32 &&
         ABS (transform[3] + transform[1]) < 0x32 &&
         ABS (transform[0]) < 0x12c && ABS (transform[1]) < 0x12c &&
         ABS (transform[3]) < 0x12c && ABS (transform[4]) < 0x12c &&
         discriminant >= 0 && trace >= 0 && upper_delta <= 0 &&
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
                  const gint32 squared = dx * dx + dy * dy;

                  if (ABS (dx) <= 0x280 && ABS (dy) <= 0x280 &&
                      squared < 0x64000)
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

guint
goodix_chicago_match_ordinary_correspondences (
  const GoodixChicagoFeatureRecord *gallery_records,
  guint                             gallery_count,
  guint                             gallery_split,
  const GoodixChicagoFeatureRecord *probe_records,
  guint                             probe_count,
  guint                             probe_split,
  GoodixChicagoMatchPair            pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT])
{
  return ordinary_correspondences_with_gates (
    gallery_records, gallery_count, gallery_split,
    probe_records, probe_count, probe_split, 23, 47, pairs);
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

void
goodix_chicago_match_score_subtemplate_type24 (
  const GoodixChicagoSubtemplateView *gallery,
  const GoodixChicagoSubtemplateView *probe,
  GoodixChicagoMatchScoreRecord      *record,
  GoodixChicagoMatchGeometry         *geometry)
{
  GoodixChicagoMatchPair pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT];
  GoodixChicagoMatchFeatureOverlap overlap;
  gint agreement;
  gint coverage;

  g_return_if_fail (gallery != NULL);
  g_return_if_fail (probe != NULL);
  g_return_if_fail (record != NULL);
  g_return_if_fail (geometry != NULL);
  memset (record, 0, sizeof (*record));
  record->probe_quality = probe->quality;
  record->probe_coverage = probe->coverage;
  record->gallery_quality = gallery->quality;
  record->gallery_coverage = gallery->coverage;
  goodix_chicago_match_ordinary_correspondences (
    gallery->records, gallery->record_count, gallery->active_count,
    probe->records, probe->record_count, probe->active_count, pairs);
  goodix_chicago_match_geometry_consensus_records (
    gallery->records, gallery->record_count,
    probe->records, probe->record_count,
    pairs, GOODIX_CHICAGO_MATCH_PAIR_LIMIT, 3, geometry);
  record->geometry_count = geometry->inlier_count;
  record->secondary_geometry_count = geometry->inlier_count;
  if (geometry->inlier_count < 3 || !gallery->has_metric_data ||
      !probe->has_metric_data)
    return;

  record->selector =
    goodix_chicago_enrollment_calculate_config1_metrics (
      probe->metric_data, gallery->metric_data, geometry->transform,
      &agreement, &coverage);
  record->agreement = agreement;
  record->normalized_coverage = (coverage * 137) >> 8;
  goodix_chicago_enrollment_calculate_study_metrics (
    probe->metric_data, gallery->metric_data, geometry->transform,
    &record->study_metric_18, &record->study_metric_1c,
    &record->study_metric_20);
  if (geometry->inlier_count <= 4)
    return;
  goodix_chicago_match_feature_overlap_type24 (
    gallery->records, gallery->record_count,
    probe->records, probe->record_count, 80, 64,
    geometry->transform, geometry->inlier_count, &overlap);
  record->matched_percent = overlap.matched_percent;
  record->geometry_percent = overlap.geometry_percent;
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

  /* AlgoChicago+0x43300 preserves Q8 coefficients by promoting the inverse
   * numerator to Q16.  C's signed division supplies the DLL's truncation
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

gint32
goodix_chicago_match_bidirectional_overlap_area_type24 (
  guint        width,
  guint        height,
  const gint32 transform[6])
{
  gint32 inverse[6];
  gint32 forward;
  gint32 reverse;

  g_return_val_if_fail (transform != NULL, 0);
  goodix_chicago_match_invert_transform_q8_type24 (transform, inverse);
  forward = goodix_chicago_match_transform_overlap_area_type24 (
    width, height, transform);
  reverse = goodix_chicago_match_transform_overlap_area_type24 (
    width, height, inverse);
  return MAX (forward, reverse);
}

void
goodix_chicago_match_decode_probe_resolution_evidence (
  guint32                               packed_resolution,
  GoodixChicagoMatchResolutionEvidence *evidence)
{
  gint32 secondary;

  g_return_if_fail (evidence != NULL);
  evidence->primary = packed_resolution & 3;
  secondary = (packed_resolution >> 8) & 7;
  evidence->secondary = secondary > 0 ? secondary + 4 : 0;
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

  /* Type 24 fixes the scheduler mode to two at +0x29a05, selecting addition
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

  /* Type 24 is one of the template types selected by the official
   * 0x07010400 bitset at +0x2552e. */
  *study_eligible = 0;
  if ((record->geometry_count < 6 &&
       record->study_metric_20 < 210 &&
       record->study_metric_1c < 135) ||
      (record->geometry_count <= 11 &&
       record->study_metric_20 < 192 &&
       record->study_metric_1c < 90))
    *status = 0;
}

void
goodix_chicago_match_template_type24 (
  const GoodixChicagoEnrollment      *gallery_template,
  const GoodixChicagoSubtemplateView *probe,
  gint32                              selector_threshold,
  GoodixChicagoMatchTemplateResult   *result)
{
  GoodixChicagoMatchAggregation aggregation = { 0, };
  GoodixChicagoMatchScoreRecord records[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  GoodixChicagoMatchGeometry geometries[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  gint32 auxiliary_counts[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { 0, };
  gboolean auxiliary_counts_known[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { FALSE, };
  gboolean late_rejected[GOODIX_CHICAGO_ENROLLMENT_CAPACITY] = { FALSE, };
  GoodixChicagoMatchSchedulerAuxiliary scheduler_auxiliary;
  gboolean study_eligible_any = FALSE;
  gint32 candidate_metric;
  gint32 rejection_count = 0;
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
  candidate_metric = probe->live_auxiliary.values[0];
  goodix_chicago_match_scheduler_auxiliary_init_type24 (
    probe->metric_data->packed_resolution, &scheduler_auxiliary);
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
      guint index;

      if (!goodix_chicago_enrollment_get_match_order_index (
            gallery_template, schedule_index, &index) ||
          !goodix_chicago_enrollment_get_subtemplate (
            gallery_template, index, &gallery))
        continue;
      combined_auxiliary =
        goodix_chicago_match_scheduler_auxiliary_consume_type24 (
          &scheduler_auxiliary, gallery.metric_data->packed_resolution,
          rejection_count);
      auxiliary_counts[index] = scheduler_auxiliary.auxiliary_count;
      auxiliary_counts_known[index] = TRUE;
      goodix_chicago_match_score_subtemplate_type24 (
        &gallery, probe, &records[index], &geometries[index]);
      if (goodix_chicago_match_candidate_prefilter_type24 (
            &records[index], scheduler_auxiliary.auxiliary_count,
            candidate_metric))
        continue;
      goodix_chicago_match_scheduler_evidence_type24 (
        &records[index], probe->quality, probe->coverage, TRUE, &evidence);
      status_flag = evidence.status;
      study_eligible = evidence.confidence;
      /* +0x2a6ac snapshots only status-1 candidate geometry into the
       * per-gallery relation state later consumed by templateStudy. */
      if (status_flag == 1)
        {
          result->study_relations[index].inlier_count =
            geometries[index].inlier_count;
          memcpy (result->study_relations[index].transform,
                  geometries[index].transform,
                  sizeof (result->study_relations[index].transform));
        }
      late_rejected[index] =
        goodix_chicago_match_late_rejection_type24 (
          80, 64, probe->quality, &records[index],
          geometries[index].transform,
          scheduler_auxiliary.auxiliary_count, candidate_metric,
          combined_auxiliary, &rejection_count, &status_flag);
      if (late_rejected[index])
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
      study_eligible_any |= study_eligible != 0;
      if (!goodix_chicago_match_accept_type24 (
            records[index].selector, records[index].agreement, status_flag,
            selector_threshold))
        continue;
      goodix_chicago_match_aggregation_add_geometry (
        &aggregation, records[index].secondary_geometry_count, 31);
      if (status_flag != 0 && records[index].secondary_geometry_count > 0)
        {
          result->selected_index = index;
          result->study_auxiliary_count = auxiliary_counts[index];
          result->study_auxiliary_count_known = auxiliary_counts_known[index];
          result->study_candidate_metric = candidate_metric;
        }
      /* The production identify call fixes the scheduler's continue flag to
       * zero. +0x2a873 therefore exits the gallery loop immediately after
       * the first admitted candidate instead of averaging later galleries. */
      break;
    }
  if (aggregation.accepted_count > 0)
    {
      result->score =
        goodix_chicago_match_aggregation_score (&aggregation);
      result->selected_index_known = result->selected_index >= 0;
      result->study_eligible = study_eligible_any;
      result->study_eligibility_known = TRUE;
      return;
    }

  for (guint schedule_index = 0; schedule_index < gallery_count;
       schedule_index++)
    {
      GoodixChicagoSubtemplateView gallery;
      guint index;

      if (!goodix_chicago_enrollment_get_match_order_index (
            gallery_template, schedule_index, &index) ||
          !goodix_chicago_enrollment_get_subtemplate (
            gallery_template, index, &gallery) ||
          late_rejected[index] ||
          !goodix_chicago_match_fallback_gallery_enabled_type24 (
            records[index].geometry_count, 0, 1))
        continue;
      goodix_chicago_match_aggregation_consume_fallback_records_type24 (
        &aggregation, gallery.records, gallery.record_count,
        gallery.active_count, gallery.metric_data,
        probe->records, probe->record_count, probe->active_count,
        probe->metric_data, selector_threshold, NULL);
    }
  result->score = goodix_chicago_match_aggregation_score (&aggregation);
}

gint32
goodix_chicago_match_score_template_type24 (
  const GoodixChicagoEnrollment      *gallery_template,
  const GoodixChicagoSubtemplateView *probe,
  gint32                              selector_threshold)
{
  GoodixChicagoMatchTemplateResult result;

  g_return_val_if_fail (gallery_template != NULL, 0);
  g_return_val_if_fail (probe != NULL, 0);
  goodix_chicago_match_template_type24 (
    gallery_template, probe, selector_threshold, &result);
  return result.score;
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
