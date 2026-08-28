// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Shared AlgoChicago late-rejection policy.  The production helper at
 * AlgoChicago+0x2ce50 serves algorithm template types 7, 10, and 23..26.
 * Keep the legacy translation available for modes that do not yet have a
 * named, independently reviewed specialization. */

#include "goodix-chicago-late-rejection-private.h"

typedef struct
{
  gint32                               pixel_count;
  gint32                               overlap_area_x100;
  gint32                               probe_quality;
  const GoodixChicagoMatchScoreRecord *record;
  gint32                               agreement;
  gint32                               study_metric;
  gint32                               metric_sum;
  gint32                               geometry_count;
  gint32                               record_probe_quality;
  gint32                               selector;
  gint32                               candidate_auxiliary;
  gint32                               max_auxiliary;
  gint32                               rejection_count;
} Type24LateRejectionContext;

static gboolean
type24_overlap_above (const Type24LateRejectionContext *ctx,
                      gint32                            percent)
{
  return ctx->pixel_count * percent < ctx->overlap_area_x100;
}

static gboolean
type24_overlap_at_least (const Type24LateRejectionContext *ctx,
                         gint32                            percent)
{
  return ctx->pixel_count * percent <= ctx->overlap_area_x100;
}

static gboolean
type24_reject_high_auxiliary_pair (const Type24LateRejectionContext *ctx)
{
  gboolean weak_relation;
  gboolean passes_overlap_policy;

  if (ctx->max_auxiliary <= 4)
    return FALSE;

  if (ctx->candidate_auxiliary < 2)
    {
      const gboolean passes_low_candidate_policy =
        (!type24_overlap_above (ctx, 92) ||
         ctx->agreement > 209 || ctx->study_metric > 209) &&
        (!type24_overlap_above (ctx, 95) ||
         ctx->agreement > 209 || ctx->study_metric > 219) &&
        (!type24_overlap_at_least (ctx, 95) ||
         ctx->agreement > 217 || ctx->study_metric > 199) &&
        (!type24_overlap_above (ctx, 95) ||
         ((ctx->agreement > 211 || ctx->study_metric > 214) &&
          (!type24_overlap_above (ctx, 95) ||
           ctx->agreement > 199 || ctx->study_metric > 229)));

      return !passes_low_candidate_policy;
    }

  weak_relation =
    ctx->agreement < 225 && ctx->study_metric < 210 &&
    (type24_overlap_above (ctx, 90) || ctx->geometry_count < 7);
  passes_overlap_policy =
    (!type24_overlap_above (ctx, 95) || ctx->candidate_auxiliary < 4) &&
    (!type24_overlap_above (ctx, 85) || ctx->candidate_auxiliary < 5) &&
    (!type24_overlap_above (ctx, 95) ||
     ctx->agreement > 240 || ctx->study_metric > 240) &&
    (!type24_overlap_above (ctx, 90) ||
     ctx->agreement > 235 || ctx->study_metric > 230);

  return weak_relation || !passes_overlap_policy;
}

static gboolean
type24_reject_auxiliary_level_four (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;

  if (ctx->max_auxiliary <= 3)
    return FALSE;

  if (ctx->candidate_auxiliary >= 2)
    {
      return
        (type24_overlap_above (ctx, 85) &&
         ctx->agreement < 220 && ctx->study_metric < 211) ||
        (ctx->agreement < 210 && ctx->study_metric < 206 &&
         ctx->rejection_count > 5) ||
        (type24_overlap_above (ctx, 95) &&
         ctx->agreement < 231 && ctx->study_metric < 231);
    }

  if (ctx->record_probe_quality >= 90)
    return FALSE;

  if (ctx->geometry_count < 24)
    {
      const gboolean weak_small_geometry =
        (type24_overlap_above (ctx, 95) && ctx->metric_sum < 415) ||
        (type24_overlap_above (ctx, 90) && ctx->metric_sum < 410) ||
        (type24_overlap_above (ctx, 85) && ctx->metric_sum < 405) ||
        (type24_overlap_above (ctx, 90) &&
         ((ctx->metric_sum < 420 && ctx->record_probe_quality < 50) ||
          (ctx->record_probe_quality < 70 && record->geometry_percent < 45))) ||
        (ctx->geometry_count < 12 && type24_overlap_above (ctx, 80) &&
         ctx->metric_sum < 405) ||
        (ctx->geometry_count < 16 && type24_overlap_above (ctx, 80) &&
         ctx->metric_sum < 375 && ctx->record_probe_quality < 51 &&
         record->geometry_percent < 31);
      const gboolean repeated_rejection =
        ctx->rejection_count > 4 &&
        (type24_overlap_above (ctx, 98) ||
         (type24_overlap_above (ctx, 90) && ctx->study_metric < 210 &&
          record->geometry_percent < 35));

      return weak_small_geometry || repeated_rejection;
    }

  return
    (type24_overlap_above (ctx, 95) && ctx->metric_sum < 405) ||
    (type24_overlap_above (ctx, 99) &&
     ctx->agreement < 208 && ctx->study_metric < 207 &&
     record->geometry_percent < 49 && ctx->probe_quality < 71 &&
     ctx->selector < 226);
}

static gboolean
type24_reject_auxiliary_level_three (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;

  if (ctx->max_auxiliary <= 2)
    return FALSE;

  if (ctx->candidate_auxiliary >= 2)
    return type24_overlap_above (ctx, 92) &&
           ctx->agreement < 220 && ctx->study_metric < 206;

  if (ctx->record_probe_quality >= 90 || ctx->selector >= 241)
    return FALSE;

  if (ctx->geometry_count < 25)
    {
      return
        (type24_overlap_above (ctx, 90) && ctx->metric_sum < 405) ||
        (type24_overlap_above (ctx, 95) &&
         (ctx->metric_sum < 412 ||
          (ctx->metric_sum < 421 && ctx->record_probe_quality < 70))) ||
        (type24_overlap_above (ctx, 97) && ctx->metric_sum < 400 &&
         record->geometry_percent < 36);
    }

  return type24_overlap_above (ctx, 95) && ctx->metric_sum < 400 &&
         record->geometry_percent < 21;
}

static gboolean
type24_reject_auxiliary_level_two (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;

  if (ctx->max_auxiliary <= 1)
    return FALSE;

  if (ctx->candidate_auxiliary >= 2)
    {
      return
        (type24_overlap_above (ctx, 94) &&
         ctx->agreement < 220 && ctx->study_metric < 216 &&
         record->geometry_percent < 35) ||
        (type24_overlap_above (ctx, 85) &&
         ctx->agreement < 215 && ctx->study_metric < 211 &&
         record->geometry_percent < 35 && ctx->geometry_count < 15);
    }

  if (ctx->record_probe_quality >= 90 || ctx->selector >= 240)
    return FALSE;

  if (ctx->geometry_count < 24)
    {
      return
        (type24_overlap_above (ctx, 90) &&
         ((ctx->metric_sum < 406 &&
           (record->geometry_percent < 36 || ctx->geometry_count < 18)) ||
          (ctx->study_metric < 210 && record->geometry_percent < 36 &&
           ctx->record_probe_quality < 60))) ||
        (type24_overlap_above (ctx, 95) && ctx->geometry_count < 23 &&
         record->geometry_percent < 36 && ctx->record_probe_quality < 60) ||
        (type24_overlap_above (ctx, 98) && ctx->geometry_count < 23 &&
         record->geometry_percent < 41 && ctx->record_probe_quality < 65);
    }

  return
    (type24_overlap_above (ctx, 95) &&
     ctx->agreement < 211 && ctx->study_metric < 211 &&
     record->geometry_percent < 40 && ctx->probe_quality < 61) ||
    (type24_overlap_above (ctx, 98) && ctx->agreement < 211 &&
     record->geometry_percent < 41 && ctx->probe_quality < 73 &&
     ctx->selector < 230);
}

static gboolean
type24_passes_low_geometry_overlap_gate (
  const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;
  const gboolean pass_98 =
    !type24_overlap_above (ctx, 98) ||
    ((ctx->agreement > 229 || ctx->study_metric > 215 ||
      record->geometry_percent > 25 || ctx->probe_quality > 70 ||
      ctx->selector > 238) &&
     (!type24_overlap_above (ctx, 98) || ctx->agreement > 234 ||
      ctx->study_metric > 215 || record->geometry_percent > 30 ||
      ctx->probe_quality > 31 || ctx->selector > 238));
  const gboolean pass_97 =
    !type24_overlap_above (ctx, 97) ||
    ((ctx->agreement > 224 || ctx->study_metric > 210 ||
      record->geometry_percent > 25 || ctx->probe_quality > 70 ||
      ctx->selector > 234) &&
     (!type24_overlap_above (ctx, 97) ||
      ((ctx->agreement > 231 || record->geometry_percent > 38 ||
        ctx->probe_quality > 32 || ctx->selector > 237) &&
       (!type24_overlap_above (ctx, 97) || ctx->agreement > 227 ||
        ctx->study_metric > 210 || record->geometry_percent > 28 ||
        ctx->probe_quality > 40 || ctx->selector > 234))));
  const gboolean pass_96 =
    !type24_overlap_above (ctx, 96) || ctx->agreement > 224 ||
    ctx->study_metric > 210 || record->geometry_percent > 26 ||
    ctx->probe_quality > 68 || ctx->selector > 234;
  const gboolean pass_95 =
    !type24_overlap_above (ctx, 95) || ctx->agreement > 220 ||
    ctx->study_metric > 205 || record->geometry_percent > 31 ||
    ctx->probe_quality > 32 || ctx->selector > 234;
  const gboolean pass_94 =
    !type24_overlap_above (ctx, 94) || ctx->agreement > 220 ||
    ctx->study_metric > 195 || record->geometry_percent > 31 ||
    ctx->probe_quality > 52 || ctx->selector > 234;

  return pass_98 && pass_97 && pass_96 && pass_95 && pass_94;
}

static gboolean
type24_reject_low_geometry (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;
  gboolean initial_rejection;
  gboolean immediate_rejection;
  gboolean enters_history_exception;
  gboolean weak_high_overlap;
  gboolean allow_candidate;

  if (ctx->geometry_count >= 25 || ctx->record_probe_quality >= 70 ||
      ctx->selector >= 242)
    return FALSE;

  initial_rejection =
    (type24_overlap_above (ctx, 95) && ctx->agreement < 210 &&
     ctx->study_metric < 210 && ctx->probe_quality < 81) ||
    (type24_overlap_above (ctx, 90) &&
     ((ctx->agreement < 205 && ctx->study_metric < 205) ||
      (ctx->geometry_count < 12 && ctx->agreement < 210 &&
       ctx->study_metric < 210 && record->geometry_percent < 26))) ||
    (type24_overlap_above (ctx, 95) && ctx->agreement < 220 &&
     ctx->study_metric < 210 && ctx->probe_quality < 16);

  immediate_rejection =
    type24_overlap_above (ctx, 91) && ctx->agreement < 210 &&
    ctx->study_metric < 199 && record->geometry_percent < 22 &&
    ctx->probe_quality < 51 && ctx->selector < 230 &&
    ctx->geometry_count < 16;
  enters_history_exception =
    !type24_overlap_above (ctx, 87) || ctx->agreement > 215 ||
    ctx->study_metric > 185 || record->geometry_percent > 21 ||
    ctx->probe_quality > 42 || ctx->geometry_count > 15;
  weak_high_overlap =
    type24_overlap_above (ctx, 85) &&
    ((ctx->agreement < 200 && ctx->study_metric < 195 &&
      ctx->probe_quality < 71) ||
     (ctx->agreement < 210 && ctx->study_metric < 205 &&
      ctx->geometry_count < 14));
  allow_candidate =
    type24_passes_low_geometry_overlap_gate (ctx) &&
    !immediate_rejection && enters_history_exception &&
    !weak_high_overlap && ctx->rejection_count < 7;

  return initial_rejection || !allow_candidate;
}

static gboolean
type24_reject_overlap_bands (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;
  const gboolean reject_above_99 =
    type24_overlap_above (ctx, 99) &&
    ((ctx->agreement < 210 && record->geometry_percent < 37 &&
      ctx->probe_quality < 66 && ctx->selector < 231) ||
     (ctx->agreement < 210 && ctx->study_metric == 0 &&
      record->geometry_percent < 49 && ctx->probe_quality < 71 &&
      ctx->selector < 231) ||
     (ctx->agreement < 217 && ctx->study_metric < 179 &&
      record->geometry_percent < 19 && ctx->probe_quality < 81 &&
      ctx->selector < 246));
  const gboolean reject_above_98 =
    type24_overlap_above (ctx, 98) &&
    ((ctx->agreement < 220 && ctx->study_metric < 208 &&
      record->geometry_percent < 31 && ctx->probe_quality < 81 &&
      ctx->selector < 233) ||
     (ctx->agreement < 225 && ctx->study_metric < 216 &&
      record->geometry_percent < 36 && ctx->probe_quality < 68 &&
      ctx->selector < 235));
  const gboolean reject_above_97 =
    type24_overlap_above (ctx, 97) &&
    ((ctx->agreement < 220 && ctx->study_metric < 206 &&
      record->geometry_percent < 21 && ctx->probe_quality < 71 &&
      ctx->selector < 233) ||
     (ctx->agreement < 215 && record->geometry_percent < 41 &&
      ctx->probe_quality < 71 && ctx->selector < 233) ||
     (ctx->agreement < 224 && ctx->study_metric < 206 &&
      record->geometry_percent < 21 && ctx->probe_quality < 81 &&
      ctx->selector < 233 && ctx->max_auxiliary == 1) ||
     (ctx->agreement < 210 && ctx->study_metric == 0 &&
      record->geometry_percent < 48 && ctx->probe_quality < 71 &&
      ctx->selector < 233 && ctx->max_auxiliary == 1));
  const gboolean reject_above_95 =
    type24_overlap_above (ctx, 95) && ctx->agreement < 221 &&
    ctx->study_metric < 205 && record->geometry_percent < 23 &&
    ctx->probe_quality < 79 && ctx->selector < 233;
  const gboolean reject_at_least_95 =
    type24_overlap_at_least (ctx, 95) && ctx->agreement < 219 &&
    ctx->study_metric < 183 && record->geometry_percent < 14 &&
    ctx->probe_quality < 55 && ctx->selector < 246;
  const gboolean reject_above_94 =
    type24_overlap_above (ctx, 94) && ctx->agreement < 218 &&
    ctx->study_metric < 207 && record->geometry_percent < 21 &&
    ctx->probe_quality < 81 && ctx->selector < 251;
  const gboolean reject_above_93 =
    type24_overlap_above (ctx, 93) && ctx->metric_sum < 376 &&
    record->geometry_percent < 21 && ctx->probe_quality < 86 &&
    ctx->selector < 231;
  const gboolean reject_above_91 =
    type24_overlap_above (ctx, 91) &&
    ((ctx->agreement < 213 && ctx->study_metric < 205 &&
      record->geometry_percent < 23 && ctx->probe_quality < 80 &&
      ctx->selector < 229) ||
     (ctx->agreement < 216 && ctx->study_metric < 191 &&
      record->geometry_percent < 18 && ctx->probe_quality < 51 &&
      ctx->selector < 249));

  return reject_above_99 || reject_above_98 || reject_above_97 ||
         reject_above_95 || reject_at_least_95 || reject_above_94 ||
         reject_above_93 || reject_above_91;
}

static gboolean
type24_reject_candidate_auxiliary (const Type24LateRejectionContext *ctx)
{
  if (ctx->candidate_auxiliary <= 1)
    return FALSE;

  return
    (ctx->candidate_auxiliary > 2 &&
     ((ctx->agreement < 215 && ctx->study_metric < 215 &&
       ctx->geometry_count < 11 && type24_overlap_above (ctx, 80)) ||
      (ctx->agreement < 220 && ctx->study_metric < 220 &&
       type24_overlap_above (ctx, 90)))) ||
    (ctx->agreement < 215 && ctx->study_metric < 215 &&
     ctx->geometry_count < 16 && type24_overlap_above (ctx, 95));
}

static gboolean
type24_reject_baseline_weakness (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;

  return
    (type24_overlap_above (ctx, 90) && ctx->agreement < 205 &&
     ctx->study_metric < 211 && record->geometry_percent < 45 &&
     ctx->probe_quality < 51 && ctx->selector < 240) ||
    (type24_overlap_above (ctx, 95) && ctx->agreement < 216 &&
     ctx->study_metric < 216 && record->geometry_percent < 35 &&
     ctx->selector < 240);
}

static gboolean
type24_should_clear_status (const Type24LateRejectionContext *ctx)
{
  const GoodixChicagoMatchScoreRecord *record = ctx->record;

  return
    (type24_overlap_above (ctx, 92) && ctx->geometry_count < 18 &&
     record->geometry_percent < 26 && ctx->agreement < 218) ||
    (type24_overlap_above (ctx, 93) && ctx->geometry_count < 10 &&
     record->geometry_percent < 13 && ctx->agreement < 220);
}

/* Type-24 specialization of AlgoChicago+0x2ce50. Strict and inclusive
 * comparisons intentionally match the official helper. The differential
 * oracle validates the return value, rejection count, and status flag. */
static gboolean
match_late_rejection_policy_type24 (
  gint32                               width,
  gint32                               height,
  gint32                               probe_quality,
  const GoodixChicagoMatchScoreRecord *record,
  const gint32                         transform[6],
  gint32                               current_auxiliary,
  gint32                               candidate_auxiliary,
  gint32                               combined_auxiliary,
  gint32                              *rejection_count,
  gint32                              *status_flag)
{
  Type24LateRejectionContext ctx;
  gint32 inverse[6];
  gint32 forward_overlap_area;
  gint32 inverse_overlap_area;
  gboolean rejected;

  forward_overlap_area = goodix_chicago_match_transform_overlap_area_type24 (
    width, height, transform);
  goodix_chicago_match_invert_transform_q8_type24 (transform, inverse);
  inverse_overlap_area = goodix_chicago_match_transform_overlap_area_type24 (
    width, height, inverse);

  ctx.pixel_count = width * height;
  ctx.overlap_area_x100 = MAX (forward_overlap_area, inverse_overlap_area) * 100;
  ctx.probe_quality = probe_quality;
  ctx.record = record;
  ctx.agreement = record->agreement;
  ctx.study_metric = record->study_metric_20;
  ctx.metric_sum = ctx.agreement + ctx.study_metric;
  ctx.geometry_count = record->geometry_count;
  ctx.record_probe_quality = record->probe_quality;
  ctx.selector = record->selector;
  ctx.candidate_auxiliary = candidate_auxiliary;
  ctx.max_auxiliary = MAX (combined_auxiliary, current_auxiliary);
  ctx.rejection_count = *rejection_count;

  rejected =
    type24_reject_high_auxiliary_pair (&ctx) |
    type24_reject_auxiliary_level_four (&ctx) |
    type24_reject_auxiliary_level_three (&ctx) |
    type24_reject_auxiliary_level_two (&ctx) |
    type24_reject_low_geometry (&ctx) |
    type24_reject_overlap_bands (&ctx) |
    type24_reject_candidate_auxiliary (&ctx) |
    type24_reject_baseline_weakness (&ctx);

  if (type24_should_clear_status (&ctx))
    *status_flag = 0;
  if (rejected)
    (*rejection_count)++;

  return rejected;
}

gboolean
goodix_chicago_match_late_rejection_type24 (
  guint                                width,
  guint                                height,
  gint32                               probe_quality,
  const GoodixChicagoMatchScoreRecord *record,
  const gint32                         transform[6],
  gint32                               current_auxiliary,
  gint32                               candidate_auxiliary,
  gint32                               combined_auxiliary,
  gint32                              *rejection_count,
  gint32                              *status_flag)
{
  g_return_val_if_fail (width > 0 && width <= G_MAXINT32, FALSE);
  g_return_val_if_fail (height > 0 && height <= G_MAXINT32, FALSE);
  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (transform != NULL, FALSE);
  g_return_val_if_fail (rejection_count != NULL, FALSE);
  g_return_val_if_fail (status_flag != NULL, FALSE);
  return match_late_rejection_policy_type24 (
    width, height, probe_quality, record, transform, current_auxiliary,
    candidate_auxiliary, combined_auxiliary, rejection_count, status_flag);
}

static gboolean
template_type_supported (guint template_type)
{
  return template_type == 7 || template_type == 10 ||
         (template_type >= 23 && template_type <= 26);
}

gboolean
goodix_chicago_match_late_rejection (
  guint                                template_type,
  guint                                width,
  guint                                height,
  gint32                               probe_quality,
  const GoodixChicagoMatchScoreRecord *record,
  const gint32                         transform[6],
  gint32                               current_auxiliary,
  gint32                               candidate_auxiliary,
  gint32                               combined_auxiliary,
  gint32                              *rejection_count,
  gint32                              *status_flag)
{
  g_return_val_if_fail (template_type_supported (template_type), FALSE);
  g_return_val_if_fail (width > 0 && width <= G_MAXINT32, FALSE);
  g_return_val_if_fail (height > 0 && height <= G_MAXINT32, FALSE);
  g_return_val_if_fail (record != NULL, FALSE);
  g_return_val_if_fail (transform != NULL, FALSE);
  g_return_val_if_fail (rejection_count != NULL, FALSE);
  g_return_val_if_fail (status_flag != NULL, FALSE);

  if (template_type == 24)
    {
      return goodix_chicago_match_late_rejection_type24 (
        width, height, probe_quality, record, transform,
        current_auxiliary, candidate_auxiliary, combined_auxiliary,
        rejection_count, status_flag);
    }

  return goodix_chicago_match_late_rejection_compat (
    template_type, width, height, probe_quality, record, transform,
    current_auxiliary, candidate_auxiliary, combined_auxiliary,
    rejection_count, status_flag);
}
