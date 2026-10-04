/*
 * CanvasBio CB2000 Driver - NCC Matching Engine Header
 *
 * Copyright (C) 2026 Roberto Focosi <rfocosi@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#define CB2000_IMG_WIDTH 80
#define CB2000_IMG_HEIGHT 64
#define CB2000_IMG_SIZE (CB2000_IMG_WIDTH * CB2000_IMG_HEIGHT)

/* NCC correlation matching constants & defaults */
#define CB2000_NR_ENROLL_STAGES 5
#define CB2000_NCC_MAX_SHIFT_DEFAULT 8
#define CB2000_NCC_MASK_THRESHOLD_DEFAULT 24
#define CB2000_NCC_MIN_SAMPLES_DEFAULT 700

#define CB2000_NCC_MATCH_TOP1_DEFAULT 0.44
#define CB2000_NCC_MATCH_TOP2_DEFAULT 0.30
#define CB2000_NCC_MATCH_MEAN3_DEFAULT 0.32
#define CB2000_NCC_MATCH_GRAD1_DEFAULT 0.28
#define CB2000_NCC_MATCH_SCORE_SUM_DEFAULT 0.70
#define CB2000_NCC_SUPPORT_MIN_DEFAULT 2
#define CB2000_NCC_SUPPORT_INT_DEFAULT 0.28
#define CB2000_NCC_SUPPORT_GRAD_DEFAULT 0.22

#define CB2000_NCC_MEDIUM_TOP1_DEFAULT 0.42
#define CB2000_NCC_MEDIUM_TOP2_DEFAULT 0.26
#define CB2000_NCC_MEDIUM_MEAN3_DEFAULT 0.30
#define CB2000_NCC_MEDIUM_GRAD1_DEFAULT 0.26
#define CB2000_NCC_MEDIUM_SCORE_SUM_DEFAULT 0.68
#define CB2000_NCC_MEDIUM_SUPPORT_MIN_DEFAULT 2

#define CB2000_NCC_DOMINANT_TOP1_DEFAULT 0.46
#define CB2000_NCC_DOMINANT_TOP2_DEFAULT 0.15
#define CB2000_NCC_DOMINANT_MEAN3_DEFAULT 0.22
#define CB2000_NCC_DOMINANT_GRAD1_DEFAULT 0.27
#define CB2000_NCC_DOMINANT_SCORE_SUM_DEFAULT 0.72
#define CB2000_NCC_DOMINANT_SUPPORT_MIN_DEFAULT 1

#define CB2000_NCC_CONSISTENT_TOP1_DEFAULT 0.42
#define CB2000_NCC_CONSISTENT_MEAN3_DEFAULT 0.33
#define CB2000_NCC_CONSISTENT_GRAD1_DEFAULT 0.26
#define CB2000_NCC_CONSISTENT_SCORE_SUM_DEFAULT 0.68
#define CB2000_NCC_CONSISTENT_SUPPORT_MIN_DEFAULT 3
#define CB2000_NCC_NOMATCH_TOP1_DEFAULT 0.26

#define CB2000_NCC_CLEAR_NOMATCH_TOP1_DEFAULT 0.26
#define CB2000_NCC_CLEAR_NOMATCH_MEAN3_DEFAULT 0.22

#define CB2000_NCC_ADAPTIVE_ENABLED_DEFAULT 1
#define CB2000_NCC_ADAPTIVE_MAX_RELAX_DEFAULT 0.02
#define CB2000_NCC_ADAPTIVE_TARGET_MEAN_DEFAULT 0.34
#define CB2000_NCC_ADAPTIVE_TARGET_MIN_DEFAULT 0.24
#define CB2000_NCC_ADAPTIVE_SUPPORT_RELAX_AT 0.025
#define CB2000_NCC_ADAPTIVE_RELAX_SUPPORT_DEFAULT 0

typedef enum {
  CB2000_NCC_DECISION_MATCH = 0,
  CB2000_NCC_DECISION_NO_MATCH,
  CB2000_NCC_DECISION_RETRY,
  CB2000_NCC_DECISION_ERROR,
} Cb2000NccDecision;

typedef struct
{
  gdouble best_intensity;
  gdouble second_intensity;
  gdouble mean_top3_intensity;
  gdouble grad_for_best_intensity;
  guint   support_count;
  guint   compared_images;
  gdouble enroll_pair_mean_intensity;
  gdouble enroll_pair_min_intensity;
  gdouble enroll_pair_mean_gradient;
  guint   enroll_pair_count;
  gdouble adaptive_relax;
} Cb2000NccTelemetry;

const char *cb2000_ncc_decision_label (Cb2000NccDecision decision);

gdouble cb2000_ncc_env_double (const char *name,
                               gdouble     fallback,
                               gdouble     min,
                               gdouble     max);

guint cb2000_ncc_env_uint (const char *name,
                           guint       fallback,
                           guint       min,
                           guint       max);

guint cb2000_build_foreground_mask (const guint8 *img,
                                    guint8       *mask,
                                    gint          width,
                                    gint          height,
                                    guint8        threshold);

void cb2000_compute_gradient_magnitude (const guint8 *img,
                                        gdouble      *grad,
                                        gint          width,
                                        gint          height);

gdouble cb2000_ncc_match_u8_masked (const guint8 *a,
                                    const guint8 *b,
                                    const guint8 *mask_a,
                                    const guint8 *mask_b,
                                    gint          width,
                                    gint          height,
                                    gint          max_shift,
                                    guint         min_samples);

gdouble cb2000_ncc_match_f64_masked (const gdouble *a,
                                     const gdouble *b,
                                     const guint8  *mask_a,
                                     const guint8  *mask_b,
                                     gint           width,
                                     gint           height,
                                     gint           max_shift,
                                     guint          min_samples);

gdouble cb2000_ncc_compute_adaptive_relax (const Cb2000NccTelemetry *tel);

void cb2000_ncc_compute_enroll_cohesion (const guint8       *images_data,
                                         gint                n_images,
                                         guint8              mask_threshold,
                                         guint               min_samples,
                                         gint                max_shift,
                                         Cb2000NccTelemetry *tel);

Cb2000NccDecision cb2000_ncc_classify (const Cb2000NccTelemetry *tel);
