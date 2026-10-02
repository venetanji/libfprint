/*
 * CanvasBio CB2000 Driver - NCC Matching Engine Implementation
 *
 * Copyright (C) 2026 Roberto Focosi <rfocosi@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#define FP_COMPONENT "canvasbio-ncc"

#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fpi-log.h"
#include "canvasbio-ncc.h"

const char *
cb2000_ncc_decision_label (Cb2000NccDecision decision)
{
  switch (decision)
    {
    case CB2000_NCC_DECISION_MATCH:
      return "MATCH";

    case CB2000_NCC_DECISION_NO_MATCH:
      return "NO_MATCH";

    case CB2000_NCC_DECISION_RETRY:
      return "RETRY";

    case CB2000_NCC_DECISION_ERROR:
      return "ERROR";

    default:
      return "UNKNOWN";
    }
}

gdouble
cb2000_ncc_env_double (const char *name,
                       gdouble     fallback,
                       gdouble     min,
                       gdouble     max)
{
  const char *env = g_getenv (name);
  gdouble val;

  if (!env || !*env)
    return fallback;

  val = g_ascii_strtod (env, NULL);
  if (val < min || val > max)
    return fallback;
  return val;
}

guint
cb2000_ncc_env_uint (const char *name, guint fallback, guint min, guint max)
{
  const char *env = g_getenv (name);
  char *end = NULL;
  unsigned long val;

  if (!env || !*env)
    return fallback;

  val = strtoul (env, &end, 10);
  if (end == env || *end != '\0')
    return fallback;
  if (val < min || val > max)
    return fallback;
  return (guint) val;
}

guint
cb2000_build_foreground_mask (const guint8 *img,
                              guint8       *mask,
                              gint          width,
                              gint          height,
                              guint8        threshold)
{
  guint active = 0;
  gint x, y;

  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
        {
          gint idx = y * width + x;
          guint8 on = img[idx] >= threshold ? 1 : 0;
          mask[idx] = on;
          active += on;
        }
    }
  return active;
}

void
cb2000_compute_gradient_magnitude (const guint8 *img,
                                   gdouble      *grad,
                                   gint          width,
                                   gint          height)
{
  gint x, y;

  memset (grad, 0, sizeof (*grad) * width * height);
  for (y = 1; y < height - 1; y++)
    {
      for (x = 1; x < width - 1; x++)
        {
          gint idx = y * width + x;
          gint gx =
            -img[(y - 1) * width + (x - 1)] + img[(y - 1) * width + (x + 1)] +
            -2 * img[y * width + (x - 1)]     + 2 * img[y * width + (x + 1)] +
            -img[(y + 1) * width + (x - 1)] + img[(y + 1) * width + (x + 1)];
          gint gy =
            -img[(y - 1) * width + (x - 1)] - 2 * img[(y - 1) * width + x] - img[(y - 1) * width + (x + 1)] +
            img[(y + 1) * width + (x - 1)] + 2 * img[(y + 1) * width + x] + img[(y + 1) * width + (x + 1)];
          grad[idx] = sqrt ((gdouble) gx * gx + (gdouble) gy * gy);
        }
    }
}

gdouble
cb2000_ncc_match_u8_masked (const guint8 *a,
                            const guint8 *b,
                            const guint8 *mask_a,
                            const guint8 *mask_b,
                            gint          width,
                            gint          height,
                            gint          max_shift,
                            guint         min_samples)
{
  gdouble best = -2.0;
  gint dx, dy, x, y;

  for (dy = -max_shift; dy <= max_shift; dy++)
    {
      gint y0 = MAX (0, -dy);
      gint y1 = MIN (height, height - dy);
      if (y0 >= y1)
        continue;

      for (dx = -max_shift; dx <= max_shift; dx++)
        {
          gint x0 = MAX (0, -dx);
          gint x1 = MIN (width, width - dx);
          guint n = 0;
          gdouble sum_a = 0.0, sum_b = 0.0;
          gdouble mean_a, mean_b;
          gdouble cov = 0.0, var_a = 0.0, var_b = 0.0;
          gdouble score;

          if (x0 >= x1)
            continue;

          for (y = y0; y < y1; y++)
            {
              for (x = x0; x < x1; x++)
                {
                  gint ia = y * width + x;
                  gint ib = (y + dy) * width + (x + dx);
                  if (!mask_a[ia] || !mask_b[ib])
                    continue;
                  sum_a += a[ia];
                  sum_b += b[ib];
                  n++;
                }
            }

          if (n < min_samples)
            continue;

          mean_a = sum_a / n;
          mean_b = sum_b / n;

          for (y = y0; y < y1; y++)
            {
              for (x = x0; x < x1; x++)
                {
                  gint ia = y * width + x;
                  gint ib = (y + dy) * width + (x + dx);
                  gdouble da, db;
                  if (!mask_a[ia] || !mask_b[ib])
                    continue;
                  da = a[ia] - mean_a;
                  db = b[ib] - mean_b;
                  cov += da * db;
                  var_a += da * da;
                  var_b += db * db;
                }
            }

          score = (var_a < 1.0 || var_b < 1.0) ? 0.0 : cov / sqrt (var_a * var_b);
          if (score > best)
            best = score;
        }
    }

  if (best < -1.0)
    return 0.0;
  return best;
}

gdouble
cb2000_ncc_match_f64_masked (const gdouble *a,
                             const gdouble *b,
                             const guint8  *mask_a,
                             const guint8  *mask_b,
                             gint           width,
                             gint           height,
                             gint           max_shift,
                             guint          min_samples)
{
  gdouble best = -2.0;
  gint dx, dy, x, y;

  for (dy = -max_shift; dy <= max_shift; dy++)
    {
      gint y0 = MAX (0, -dy);
      gint y1 = MIN (height, height - dy);
      if (y0 >= y1)
        continue;

      for (dx = -max_shift; dx <= max_shift; dx++)
        {
          gint x0 = MAX (0, -dx);
          gint x1 = MIN (width, width - dx);
          guint n = 0;
          gdouble sum_a = 0.0, sum_b = 0.0;
          gdouble mean_a, mean_b;
          gdouble cov = 0.0, var_a = 0.0, var_b = 0.0;
          gdouble score;

          if (x0 >= x1)
            continue;

          for (y = y0; y < y1; y++)
            {
              for (x = x0; x < x1; x++)
                {
                  gint ia = y * width + x;
                  gint ib = (y + dy) * width + (x + dx);
                  if (!mask_a[ia] || !mask_b[ib])
                    continue;
                  sum_a += a[ia];
                  sum_b += b[ib];
                  n++;
                }
            }

          if (n < min_samples)
            continue;

          mean_a = sum_a / n;
          mean_b = sum_b / n;

          for (y = y0; y < y1; y++)
            {
              for (x = x0; x < x1; x++)
                {
                  gint ia = y * width + x;
                  gint ib = (y + dy) * width + (x + dx);
                  gdouble da, db;
                  if (!mask_a[ia] || !mask_b[ib])
                    continue;
                  da = a[ia] - mean_a;
                  db = b[ib] - mean_b;
                  cov += da * db;
                  var_a += da * da;
                  var_b += db * db;
                }
            }

          score = (var_a < 1.0 || var_b < 1.0) ? 0.0 : cov / sqrt (var_a * var_b);
          if (score > best)
            best = score;
        }
    }

  if (best < -1.0)
    return 0.0;
  return best;
}

gdouble
cb2000_ncc_compute_adaptive_relax (const Cb2000NccTelemetry *tel)
{
  guint adaptive_enabled = cb2000_ncc_env_uint ("CB2000_NCC_ADAPTIVE",
                                                CB2000_NCC_ADAPTIVE_ENABLED_DEFAULT,
                                                0, 1);
  gdouble max_relax = cb2000_ncc_env_double ("CB2000_NCC_ADAPTIVE_MAX_RELAX",
                                             CB2000_NCC_ADAPTIVE_MAX_RELAX_DEFAULT,
                                             0.0, 0.20);
  gdouble target_mean = cb2000_ncc_env_double ("CB2000_NCC_ADAPTIVE_TARGET_MEAN",
                                               CB2000_NCC_ADAPTIVE_TARGET_MEAN_DEFAULT,
                                               0.0, 1.0);
  gdouble target_min = cb2000_ncc_env_double ("CB2000_NCC_ADAPTIVE_TARGET_MIN",
                                              CB2000_NCC_ADAPTIVE_TARGET_MIN_DEFAULT,
                                              0.0, 1.0);
  gdouble deficit_mean;
  gdouble deficit_min;
  gdouble relax;

  if (!adaptive_enabled || tel->enroll_pair_count == 0)
    return 0.0;

  deficit_mean = MAX (0.0, target_mean - tel->enroll_pair_mean_intensity);
  deficit_min = MAX (0.0, target_min - tel->enroll_pair_min_intensity);

  /* Weighted deficits: broad cohesion (mean) + worst-case spread (min). */
  relax = deficit_mean * 0.60 + deficit_min * 0.40;
  if (relax > max_relax)
    relax = max_relax;
  if (relax < 0.0)
    relax = 0.0;
  return relax;
}

void
cb2000_ncc_compute_enroll_cohesion (const guint8       *images_data,
                                    gint                n_images,
                                    guint8              mask_threshold,
                                    guint               min_samples,
                                    gint                max_shift,
                                    Cb2000NccTelemetry *tel)
{
  guint8 masks[CB2000_NR_ENROLL_STAGES][CB2000_IMG_SIZE];
  gdouble grads[CB2000_NR_ENROLL_STAGES][CB2000_IMG_SIZE];
  const guint8 *images[CB2000_NR_ENROLL_STAGES];
  gdouble sum_int = 0.0;
  gdouble sum_grad = 0.0;
  gdouble min_int = 1.0;
  guint pairs = 0;
  gint i, j;

  tel->enroll_pair_mean_intensity = 0.0;
  tel->enroll_pair_min_intensity = 0.0;
  tel->enroll_pair_mean_gradient = 0.0;
  tel->enroll_pair_count = 0;

  if (n_images < 2)
    return;

  n_images = MIN (n_images, CB2000_NR_ENROLL_STAGES);

  for (i = 0; i < n_images; i++)
    {
      images[i] = images_data + (i * CB2000_IMG_SIZE);
      cb2000_build_foreground_mask (images[i], masks[i],
                                    CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                    mask_threshold);
      cb2000_compute_gradient_magnitude (images[i], grads[i],
                                         CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
    }

  for (i = 0; i < n_images; i++)
    {
      for (j = i + 1; j < n_images; j++)
        {
          gdouble int_pair = cb2000_ncc_match_u8_masked (images[i], images[j],
                                                         masks[i], masks[j],
                                                         CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                                         max_shift, min_samples);
          gdouble grad_pair = cb2000_ncc_match_f64_masked (grads[i], grads[j],
                                                           masks[i], masks[j],
                                                           CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                                           max_shift, min_samples);
          sum_int += int_pair;
          sum_grad += grad_pair;
          min_int = MIN (min_int, int_pair);
          pairs++;
          fp_dbg ("[ NCC ] cohesion pair=%d-%d int=%.4f grad=%.4f",
                  i, j, int_pair, grad_pair);
        }
    }

  if (pairs == 0)
    return;

  tel->enroll_pair_count = pairs;
  tel->enroll_pair_mean_intensity = sum_int / pairs;
  tel->enroll_pair_min_intensity = min_int;
  tel->enroll_pair_mean_gradient = sum_grad / pairs;
}

Cb2000NccDecision
cb2000_ncc_classify (const Cb2000NccTelemetry *tel)
{
  gdouble match_top1 = cb2000_ncc_env_double ("CB2000_NCC_MATCH_TOP1",
                                              CB2000_NCC_MATCH_TOP1_DEFAULT,
                                              0.0, 1.0);
  gdouble match_top2 = cb2000_ncc_env_double ("CB2000_NCC_MATCH_TOP2",
                                              CB2000_NCC_MATCH_TOP2_DEFAULT,
                                              0.0, 1.0);
  gdouble match_mean3 = cb2000_ncc_env_double ("CB2000_NCC_MATCH_MEAN3",
                                               CB2000_NCC_MATCH_MEAN3_DEFAULT,
                                               0.0, 1.0);
  gdouble match_grad1 = cb2000_ncc_env_double ("CB2000_NCC_MATCH_GRAD1",
                                               CB2000_NCC_MATCH_GRAD1_DEFAULT,
                                               0.0, 1.0);
  guint support_min = cb2000_ncc_env_uint ("CB2000_NCC_SUPPORT_MIN",
                                           CB2000_NCC_SUPPORT_MIN_DEFAULT,
                                           1, CB2000_NR_ENROLL_STAGES);
  gdouble medium_top1 = cb2000_ncc_env_double ("CB2000_NCC_MEDIUM_TOP1",
                                               CB2000_NCC_MEDIUM_TOP1_DEFAULT,
                                               0.0, 1.0);
  gdouble medium_top2 = cb2000_ncc_env_double ("CB2000_NCC_MEDIUM_TOP2",
                                               CB2000_NCC_MEDIUM_TOP2_DEFAULT,
                                               0.0, 1.0);
  gdouble medium_mean3 = cb2000_ncc_env_double ("CB2000_NCC_MEDIUM_MEAN3",
                                                CB2000_NCC_MEDIUM_MEAN3_DEFAULT,
                                                0.0, 1.0);
  gdouble medium_grad1 = cb2000_ncc_env_double ("CB2000_NCC_MEDIUM_GRAD1",
                                                CB2000_NCC_MEDIUM_GRAD1_DEFAULT,
                                                0.0, 1.0);
  guint medium_support_min = cb2000_ncc_env_uint ("CB2000_NCC_MEDIUM_SUPPORT_MIN",
                                                  CB2000_NCC_MEDIUM_SUPPORT_MIN_DEFAULT,
                                                  1, CB2000_NR_ENROLL_STAGES);
  gdouble dominant_top1 = cb2000_ncc_env_double ("CB2000_NCC_DOMINANT_TOP1",
                                                 CB2000_NCC_DOMINANT_TOP1_DEFAULT,
                                                 0.0, 1.0);
  gdouble dominant_top2 = cb2000_ncc_env_double ("CB2000_NCC_DOMINANT_TOP2",
                                                 CB2000_NCC_DOMINANT_TOP2_DEFAULT,
                                                 0.0, 1.0);
  gdouble dominant_mean3 = cb2000_ncc_env_double ("CB2000_NCC_DOMINANT_MEAN3",
                                                  CB2000_NCC_DOMINANT_MEAN3_DEFAULT,
                                                  0.0, 1.0);
  gdouble dominant_grad1 = cb2000_ncc_env_double ("CB2000_NCC_DOMINANT_GRAD1",
                                                  CB2000_NCC_DOMINANT_GRAD1_DEFAULT,
                                                  0.0, 1.0);
  guint dominant_support_min = cb2000_ncc_env_uint ("CB2000_NCC_DOMINANT_SUPPORT_MIN",
                                                    CB2000_NCC_DOMINANT_SUPPORT_MIN_DEFAULT,
                                                    1, CB2000_NR_ENROLL_STAGES);
  gdouble consistent_top1 = cb2000_ncc_env_double ("CB2000_NCC_CONSISTENT_TOP1",
                                                   CB2000_NCC_CONSISTENT_TOP1_DEFAULT,
                                                   0.0, 1.0);
  gdouble consistent_mean3 = cb2000_ncc_env_double ("CB2000_NCC_CONSISTENT_MEAN3",
                                                    CB2000_NCC_CONSISTENT_MEAN3_DEFAULT,
                                                    0.0, 1.0);
  gdouble consistent_grad1 = cb2000_ncc_env_double ("CB2000_NCC_CONSISTENT_GRAD1",
                                                    CB2000_NCC_CONSISTENT_GRAD1_DEFAULT,
                                                    0.0, 1.0);
  guint consistent_support_min = cb2000_ncc_env_uint ("CB2000_NCC_CONSISTENT_SUPPORT_MIN",
                                                      CB2000_NCC_CONSISTENT_SUPPORT_MIN_DEFAULT,
                                                      1, CB2000_NR_ENROLL_STAGES);
  gdouble nomatch_top1 = cb2000_ncc_env_double ("CB2000_NCC_NOMATCH_TOP1",
                                                CB2000_NCC_NOMATCH_TOP1_DEFAULT,
                                                0.0, 1.0);
  gdouble clear_nomatch_top1 = cb2000_ncc_env_double ("CB2000_NCC_CLEAR_NOMATCH_TOP1",
                                                      CB2000_NCC_CLEAR_NOMATCH_TOP1_DEFAULT,
                                                      0.0, 1.0);
  gdouble clear_nomatch_mean3 = cb2000_ncc_env_double ("CB2000_NCC_CLEAR_NOMATCH_MEAN3",
                                                       CB2000_NCC_CLEAR_NOMATCH_MEAN3_DEFAULT,
                                                       0.0, 1.0);
  guint adaptive_relax_support = cb2000_ncc_env_uint ("CB2000_NCC_ADAPTIVE_RELAX_SUPPORT",
                                                      CB2000_NCC_ADAPTIVE_RELAX_SUPPORT_DEFAULT,
                                                      0, 1);
  gdouble relax = cb2000_ncc_compute_adaptive_relax (tel);

  if (relax > 0.0)
    {
      match_top1 = MAX (0.0, match_top1 - relax);
      match_top2 = MAX (0.0, match_top2 - (relax * 0.75));
      match_mean3 = MAX (0.0, match_mean3 - (relax * 0.75));
      match_grad1 = MAX (0.0, match_grad1 - (relax * 0.50));

      medium_top1 = MAX (0.0, medium_top1 - relax);
      medium_top2 = MAX (0.0, medium_top2 - (relax * 0.80));
      medium_mean3 = MAX (0.0, medium_mean3 - (relax * 0.80));
      medium_grad1 = MAX (0.0, medium_grad1 - (relax * 0.50));

      dominant_top1 = MAX (0.0, dominant_top1 - (relax * 0.40));
      dominant_top2 = MAX (0.0, dominant_top2 - (relax * 0.25));
      dominant_mean3 = MAX (0.0, dominant_mean3 - (relax * 0.50));
      dominant_grad1 = MAX (0.0, dominant_grad1 - (relax * 0.40));

      consistent_top1 = MAX (0.0, consistent_top1 - (relax * 0.60));
      consistent_mean3 = MAX (0.0, consistent_mean3 - (relax * 0.50));
      consistent_grad1 = MAX (0.0, consistent_grad1 - (relax * 0.40));

      if (adaptive_relax_support)
        {
          if (support_min > 1 && relax >= CB2000_NCC_ADAPTIVE_SUPPORT_RELAX_AT)
            support_min--;
          if (medium_support_min > 1 && relax >= CB2000_NCC_ADAPTIVE_SUPPORT_RELAX_AT)
            medium_support_min--;
          if (consistent_support_min > 1 && relax >= CB2000_NCC_ADAPTIVE_SUPPORT_RELAX_AT)
            consistent_support_min--;
        }
    }

  g_message ("[ NCC ] classify thresholds: strong(top1>=%.3f top2>=%.3f mean3>=%.3f grad1>=%.3f support>=%u) "
             "medium(top1>=%.3f top2>=%.3f mean3>=%.3f grad1>=%.3f support>=%u) "
             "dominant(top1>=%.3f top2>=%.3f mean3>=%.3f grad1>=%.3f support>=%u) "
             "consistent(top1>=%.3f mean3>=%.3f grad1>=%.3f support>=%u) "
             "clear_nomatch(support==0 && top1<%.3f && mean3<%.3f) "
             "nomatch(top1<%.3f) "
             "adaptive(relax=%.3f relax_support=%u cohesion_mean=%.3f cohesion_min=%.3f pairs=%u)",
             match_top1, match_top2, match_mean3, match_grad1, support_min,
             medium_top1, medium_top2, medium_mean3, medium_grad1, medium_support_min,
             dominant_top1, dominant_top2, dominant_mean3, dominant_grad1, dominant_support_min,
             consistent_top1, consistent_mean3, consistent_grad1, consistent_support_min,
             clear_nomatch_top1, clear_nomatch_mean3,
             nomatch_top1,
             relax,
             adaptive_relax_support,
             tel->enroll_pair_mean_intensity,
             tel->enroll_pair_min_intensity,
             tel->enroll_pair_count);

  if (tel->compared_images == 0)
    return CB2000_NCC_DECISION_ERROR;

  if (tel->support_count >= support_min &&
      tel->best_intensity >= match_top1 &&
      tel->second_intensity >= match_top2 &&
      tel->mean_top3_intensity >= match_mean3 &&
      tel->grad_for_best_intensity >= match_grad1)
    return CB2000_NCC_DECISION_MATCH;

  if (tel->support_count >= medium_support_min &&
      tel->best_intensity >= medium_top1 &&
      tel->second_intensity >= medium_top2 &&
      tel->mean_top3_intensity >= medium_mean3 &&
      tel->grad_for_best_intensity >= medium_grad1)
    return CB2000_NCC_DECISION_MATCH;

  if (tel->support_count >= dominant_support_min &&
      tel->best_intensity >= dominant_top1 &&
      tel->second_intensity >= dominant_top2 &&
      tel->mean_top3_intensity >= dominant_mean3 &&
      tel->grad_for_best_intensity >= dominant_grad1)
    return CB2000_NCC_DECISION_MATCH;

  if (tel->support_count >= consistent_support_min &&
      tel->best_intensity >= consistent_top1 &&
      tel->mean_top3_intensity >= consistent_mean3 &&
      tel->grad_for_best_intensity >= consistent_grad1)
    return CB2000_NCC_DECISION_MATCH;

  if (tel->support_count == 0 &&
      tel->best_intensity < clear_nomatch_top1 &&
      tel->mean_top3_intensity < clear_nomatch_mean3)
    return CB2000_NCC_DECISION_NO_MATCH;

  if (tel->best_intensity < nomatch_top1)
    return CB2000_NCC_DECISION_NO_MATCH;

  return CB2000_NCC_DECISION_RETRY;
}
