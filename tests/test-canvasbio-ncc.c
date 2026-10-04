/*
 * CanvasBio CB2000 NCC Unit Tests
 * Copyright (C) 2026 Roberto Focosi <rfocosi@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#define FP_COMPONENT "test-canvasbio-ncc"

#include <glib.h>
#include <math.h>
#include <string.h>

#include "drivers/canvasbio-ncc.h"

static void
test_ncc_decision_labels (void)
{
  g_assert_cmpstr (cb2000_ncc_decision_label (CB2000_NCC_DECISION_MATCH), ==, "MATCH");
  g_assert_cmpstr (cb2000_ncc_decision_label (CB2000_NCC_DECISION_NO_MATCH), ==, "NO_MATCH");
  g_assert_cmpstr (cb2000_ncc_decision_label (CB2000_NCC_DECISION_RETRY), ==, "RETRY");
  g_assert_cmpstr (cb2000_ncc_decision_label (CB2000_NCC_DECISION_ERROR), ==, "ERROR");
  g_assert_cmpstr (cb2000_ncc_decision_label ((Cb2000NccDecision) 999), ==, "UNKNOWN");
}

static void
test_ncc_env_helpers (void)
{
  gdouble d_val;
  guint u_val;

  /* 1. Unset env vars return fallback */
  g_unsetenv ("CB2000_NCC_TEST_D");
  g_unsetenv ("CB2000_NCC_TEST_U");
  d_val = cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0);
  g_assert_cmpfloat_with_epsilon (d_val, 0.42, 1e-6);
  u_val = cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20);
  g_assert_cmpuint (u_val, ==, 10);

  /* 2. Empty env var returns fallback */
  g_setenv ("CB2000_NCC_TEST_D", "", TRUE);
  g_setenv ("CB2000_NCC_TEST_U", "", TRUE);
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0), 0.42, 1e-6);
  g_assert_cmpuint (cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20), ==, 10);

  /* 3. Valid env vars within [min, max] */
  g_setenv ("CB2000_NCC_TEST_D", "0.75", TRUE);
  g_setenv ("CB2000_NCC_TEST_U", "15", TRUE);
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0), 0.75, 1e-6);
  g_assert_cmpuint (cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20), ==, 15);

  /* 4. Out of bounds values return fallback */
  g_setenv ("CB2000_NCC_TEST_D", "1.50", TRUE);
  g_setenv ("CB2000_NCC_TEST_U", "30", TRUE);
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0), 0.42, 1e-6);
  g_assert_cmpuint (cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20), ==, 10);

  g_setenv ("CB2000_NCC_TEST_D", "-0.10", TRUE);
  g_setenv ("CB2000_NCC_TEST_U", "0", TRUE);
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0), 0.42, 1e-6);
  g_assert_cmpuint (cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20), ==, 10);

  /* 5. Non-numeric invalid strings return fallback */
  g_setenv ("CB2000_NCC_TEST_D", "invalid", TRUE);
  g_setenv ("CB2000_NCC_TEST_U", "not_a_number", TRUE);
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_env_double ("CB2000_NCC_TEST_D", 0.42, 0.0, 1.0), 0.42, 1e-6);
  g_assert_cmpuint (cb2000_ncc_env_uint ("CB2000_NCC_TEST_U", 10, 1, 20), ==, 10);

  g_unsetenv ("CB2000_NCC_TEST_D");
  g_unsetenv ("CB2000_NCC_TEST_U");
}

static void
test_ncc_foreground_mask (void)
{
  guint8 img[CB2000_IMG_SIZE];
  guint8 mask[CB2000_IMG_SIZE];
  guint active;
  gint x, y;

  /* Flat image below threshold */
  memset (img, 10, sizeof (img));
  active = cb2000_build_foreground_mask (img, mask, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, 24);
  g_assert_cmpuint (active, ==, 0);

  /* Central rectangle above threshold */
  memset (img, 0, sizeof (img));
  for (y = 10; y < 30; y++)
    for (x = 10; x < 40; x++)
      img[y * CB2000_IMG_WIDTH + x] = 120;

  active = cb2000_build_foreground_mask (img, mask, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, 24);
  g_assert_cmpuint (active, ==, 20 * 30);
  g_assert_cmpuint (mask[15 * CB2000_IMG_WIDTH + 15], ==, 1);
  g_assert_cmpuint (mask[0], ==, 0);
}

static void
test_ncc_foreground_mask_edges (void)
{
  guint8 img[CB2000_IMG_SIZE];
  guint8 mask[CB2000_IMG_SIZE];
  guint active;

  /* All pixels at 255 with threshold 24 -> all pixels active */
  memset (img, 255, sizeof (img));
  active = cb2000_build_foreground_mask (img, mask, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, 24);
  g_assert_cmpuint (active, ==, CB2000_IMG_SIZE);
  g_assert_cmpuint (mask[0], ==, 1);
  g_assert_cmpuint (mask[CB2000_IMG_SIZE - 1], ==, 1);

  /* All pixels at threshold boundary (e.g. exactly 24 with threshold 24) */
  memset (img, 24, sizeof (img));
  active = cb2000_build_foreground_mask (img, mask, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, 24);
  g_assert_cmpuint (active, ==, CB2000_IMG_SIZE);

  /* All pixels at threshold - 1 (23 with threshold 24) -> 0 active */
  memset (img, 23, sizeof (img));
  active = cb2000_build_foreground_mask (img, mask, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, 24);
  g_assert_cmpuint (active, ==, 0);
}

static void
test_ncc_gradient_magnitude (void)
{
  guint8 img[CB2000_IMG_SIZE];
  gdouble grad[CB2000_IMG_SIZE];
  gint x, y;

  /* Uniform image has zero gradient everywhere */
  memset (img, 128, sizeof (img));
  cb2000_compute_gradient_magnitude (img, grad, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    for (x = 0; x < CB2000_IMG_WIDTH; x++)
      g_assert_cmpfloat_with_epsilon (grad[y * CB2000_IMG_WIDTH + x], 0.0, 1e-6);

  /* Vertical step edge at x = 40 */
  memset (img, 0, sizeof (img));
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    for (x = 40; x < CB2000_IMG_WIDTH; x++)
      img[y * CB2000_IMG_WIDTH + x] = 200;

  cb2000_compute_gradient_magnitude (img, grad, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
  /* Boundary at x=40 should have strong gradient */
  g_assert_cmpfloat (grad[20 * CB2000_IMG_WIDTH + 40], >, 100.0);
  /* Interior region at x=60 should have zero gradient */
  g_assert_cmpfloat_with_epsilon (grad[20 * CB2000_IMG_WIDTH + 60], 0.0, 1e-6);
}

static void
test_ncc_gradient_magnitude_edges (void)
{
  guint8 img[CB2000_IMG_SIZE];
  gdouble grad[CB2000_IMG_SIZE];
  gint x, y;

  /* Horizontal step edge at y = 32 */
  memset (img, 0, sizeof (img));
  for (y = 32; y < CB2000_IMG_HEIGHT; y++)
    for (x = 0; x < CB2000_IMG_WIDTH; x++)
      img[y * CB2000_IMG_WIDTH + x] = 200;

  cb2000_compute_gradient_magnitude (img, grad, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);

  /* Boundary at y = 32 must have strong gradient */
  g_assert_cmpfloat (grad[32 * CB2000_IMG_WIDTH + 20], >, 100.0);

  /* Boundaries (borders x=0, y=0, x=width-1, y=height-1) must be 0.0 */
  g_assert_cmpfloat_with_epsilon (grad[0], 0.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (grad[CB2000_IMG_SIZE - 1], 0.0, 1e-6);
  for (x = 0; x < CB2000_IMG_WIDTH; x++)
    {
      g_assert_cmpfloat_with_epsilon (grad[x], 0.0, 1e-6);
      g_assert_cmpfloat_with_epsilon (grad[(CB2000_IMG_HEIGHT - 1) * CB2000_IMG_WIDTH + x], 0.0, 1e-6);
    }
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    {
      g_assert_cmpfloat_with_epsilon (grad[y * CB2000_IMG_WIDTH], 0.0, 1e-6);
      g_assert_cmpfloat_with_epsilon (grad[y * CB2000_IMG_WIDTH + (CB2000_IMG_WIDTH - 1)], 0.0, 1e-6);
    }
}

static void
test_ncc_masked_shifts (void)
{
  guint8 a[CB2000_IMG_SIZE];
  guint8 b[CB2000_IMG_SIZE];
  guint8 mask_a[CB2000_IMG_SIZE];
  guint8 mask_b[CB2000_IMG_SIZE];
  gdouble score;
  gint x, y;

  /* Generate a synthetic 2D ridge pattern */
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    {
      for (x = 0; x < CB2000_IMG_WIDTH; x++)
        {
          gdouble val = 128.0 + 50.0 * sin (x * 0.4) + 40.0 * cos (y * 0.3);
          a[y * CB2000_IMG_WIDTH + x] = (guint8) CLAMP (val, 0.0, 255.0);
          mask_a[y * CB2000_IMG_WIDTH + x] = 1;
        }
    }

  /* Self-match: score should be 1.0 */
  score = cb2000_ncc_match_u8_masked (a, a, mask_a, mask_a,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      6, 700);
  g_assert_cmpfloat_with_epsilon (score, 1.0, 1e-4);

  /* Shifted pattern by dx = +4 */
  memset (b, 0, sizeof (b));
  memset (mask_b, 0, sizeof (mask_b));
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    {
      for (x = 4; x < CB2000_IMG_WIDTH; x++)
        {
          b[y * CB2000_IMG_WIDTH + x] = a[y * CB2000_IMG_WIDTH + (x - 4)];
          mask_b[y * CB2000_IMG_WIDTH + x] = 1;
        }
    }

  /* With max_shift = 6, it should find dx=4 and score near 1.0 */
  score = cb2000_ncc_match_u8_masked (a, b, mask_a, mask_b,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      6, 700);
  g_assert_cmpfloat (score, >, 0.95);

  /* With max_shift = 1 (cannot cover shift of 4), score drops significantly */
  score = cb2000_ncc_match_u8_masked (a, b, mask_a, mask_b,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      1, 700);
  g_assert_cmpfloat (score, <, 0.70);

  /* Test cb2000_ncc_match_u8_masked_ext coordinate output */
  gint best_dx = 0, best_dy = 0;
  score = cb2000_ncc_match_u8_masked_ext (a, b, mask_a, mask_b,
                                          CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                          6, 700, &best_dx, &best_dy);
  g_assert_cmpfloat (score, >, 0.95);
  g_assert_cmpint (best_dx, ==, 4);
  g_assert_cmpint (best_dy, ==, 0);

  /* Test cb2000_ncc_match_f64_masked_window around best shift */
  gdouble a_f[CB2000_IMG_SIZE], b_f[CB2000_IMG_SIZE];
  for (y = 0; y < CB2000_IMG_SIZE; y++)
    {
      a_f[y] = (gdouble) a[y];
      b_f[y] = (gdouble) b[y];
    }
  score = cb2000_ncc_match_f64_masked_window (a_f, b_f, mask_a, mask_b,
                                              CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                              best_dx, best_dy, 1, 700);
  g_assert_cmpfloat (score, >, 0.95);
}

static void
test_ncc_masked_shifts_edge_cases (void)
{
  guint8 a[CB2000_IMG_SIZE];
  guint8 b[CB2000_IMG_SIZE];
  guint8 mask_a[CB2000_IMG_SIZE];
  guint8 mask_b[CB2000_IMG_SIZE];
  gdouble a_f[CB2000_IMG_SIZE];
  gdouble b_f[CB2000_IMG_SIZE];
  gdouble score;
  gint i;

  /* 1. Flat images: variance is zero (var < 1.0) -> returns 0.0 */
  memset (a, 128, sizeof (a));
  memset (b, 128, sizeof (b));
  memset (mask_a, 1, sizeof (mask_a));
  memset (mask_b, 1, sizeof (mask_b));
  score = cb2000_ncc_match_u8_masked (a, b, mask_a, mask_b,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      8, 700);
  g_assert_cmpfloat_with_epsilon (score, 0.0, 1e-6);

  for (i = 0; i < CB2000_IMG_SIZE; i++)
    {
      a_f[i] = 128.0;
      b_f[i] = 128.0;
    }
  score = cb2000_ncc_match_f64_masked (a_f, b_f, mask_a, mask_b,
                                       CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                       8, 700);
  g_assert_cmpfloat_with_epsilon (score, 0.0, 1e-6);

  /* 2. Insufficient overlap (n < min_samples): only 50 pixels active while min_samples = 700 */
  memset (mask_a, 0, sizeof (mask_a));
  memset (mask_b, 0, sizeof (mask_b));
  for (i = 0; i < 50; i++)
    {
      mask_a[i] = 1;
      mask_b[i] = 1;
      a[i] = (guint8) (i * 5);
      b[i] = (guint8) (i * 5);
      a_f[i] = (gdouble) (i * 5);
      b_f[i] = (gdouble) (i * 5);
    }
  score = cb2000_ncc_match_u8_masked (a, b, mask_a, mask_b,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      8, 700);
  g_assert_cmpfloat_with_epsilon (score, 0.0, 1e-6);

  score = cb2000_ncc_match_f64_masked (a_f, b_f, mask_a, mask_b,
                                       CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                       8, 700);
  g_assert_cmpfloat_with_epsilon (score, 0.0, 1e-6);

  /* 3. Inverted image (perfect negative correlation) */
  memset (mask_a, 1, sizeof (mask_a));
  memset (mask_b, 1, sizeof (mask_b));
  for (i = 0; i < CB2000_IMG_SIZE; i++)
    {
      a[i] = (guint8) (i % 256);
      b[i] = (guint8) (255 - (i % 256));
      a_f[i] = (gdouble) a[i];
      b_f[i] = (gdouble) b[i];
    }
  score = cb2000_ncc_match_u8_masked (a, b, mask_a, mask_b,
                                      CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                      0, 700);
  g_assert_cmpfloat (score, <, -0.95);

  score = cb2000_ncc_match_f64_masked (a_f, b_f, mask_a, mask_b,
                                       CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                       0, 700);
  g_assert_cmpfloat (score, <, -0.95);
}

static void
test_ncc_adaptive_relaxation (void)
{
  Cb2000NccTelemetry tel = { 0 };

  /* Zero pairs: relax must be 0.0 */
  tel.enroll_pair_count = 0;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel), 0.0, 1e-6);

  /* High cohesion: mean = 0.40, min = 0.32 (both above targets 0.34 and 0.24) -> relax = 0.0 */
  tel.enroll_pair_count = 10;
  tel.enroll_pair_mean_intensity = 0.40;
  tel.enroll_pair_min_intensity = 0.32;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel), 0.0, 1e-6);

  /* Moderate cohesion: mean = 0.30 (deficit 0.04), min = 0.20 (deficit 0.04) */
  /* relax = 0.04 * 0.60 + 0.04 * 0.40 = 0.024 + 0.016 = 0.040 -> clamped to max_relax 0.020 */
  tel.enroll_pair_mean_intensity = 0.30;
  tel.enroll_pair_min_intensity = 0.20;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel),
                                  CB2000_NCC_ADAPTIVE_MAX_RELAX_DEFAULT,
                                  1e-6);
}

static void
test_ncc_enroll_cohesion (void)
{
  guint8 images[CB2000_NR_ENROLL_STAGES * CB2000_IMG_SIZE] = { 0 };
  Cb2000NccTelemetry tel = { 0 };
  gint i, x, y;

  /* 1. Less than 2 images: early return with 0 pairs */
  cb2000_ncc_compute_enroll_cohesion (images, 1, 24, 700, 8, &tel);
  g_assert_cmpuint (tel.enroll_pair_count, ==, 0);
  g_assert_cmpfloat_with_epsilon (tel.enroll_pair_mean_intensity, 0.0, 1e-6);

  /* 2. 5 identical synthetic images with clear ridge pattern */
  for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
    {
      guint8 *stage = images + (i * CB2000_IMG_SIZE);
      for (y = 0; y < CB2000_IMG_HEIGHT; y++)
        {
          for (x = 0; x < CB2000_IMG_WIDTH; x++)
            {
              gdouble val = 128.0 + 80.0 * sin (x * 0.4) * cos (y * 0.3);
              stage[y * CB2000_IMG_WIDTH + x] = (guint8) CLAMP (val, 0.0, 255.0);
            }
        }
    }

  cb2000_ncc_compute_enroll_cohesion (images, CB2000_NR_ENROLL_STAGES, 24, 700, 8, &tel);
  /* 5 images -> 5 * 4 / 2 = 10 pairs */
  g_assert_cmpuint (tel.enroll_pair_count, ==, 10);
  g_assert_cmpfloat_with_epsilon (tel.enroll_pair_mean_intensity, 1.0, 1e-3);
  g_assert_cmpfloat_with_epsilon (tel.enroll_pair_min_intensity, 1.0, 1e-3);
  g_assert_cmpfloat_with_epsilon (tel.enroll_pair_mean_gradient, 1.0, 1e-3);

  /* 3. 5 images with incremental shift dx = 1 per stage (total dx = 4 between stages 0 and 4) */
  for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
    {
      guint8 *stage = images + (i * CB2000_IMG_SIZE);
      memset (stage, 0, CB2000_IMG_SIZE);
      for (y = 0; y < CB2000_IMG_HEIGHT; y++)
        {
          for (x = i; x < CB2000_IMG_WIDTH; x++)
            {
              gdouble val = 128.0 + 80.0 * sin ((x - i) * 0.4);
              stage[y * CB2000_IMG_WIDTH + x] = (guint8) CLAMP (val, 0.0, 255.0);
            }
        }
    }

  cb2000_ncc_compute_enroll_cohesion (images, CB2000_NR_ENROLL_STAGES, 24, 700, 8, &tel);
  g_assert_cmpuint (tel.enroll_pair_count, ==, 10);
  /* With max_shift = 8, all shifts (dx <= 4) are found with high correlation */
  g_assert_cmpfloat (tel.enroll_pair_mean_intensity, >, 0.90);
  g_assert_cmpfloat (tel.enroll_pair_min_intensity, >, 0.85);
}

static void
test_ncc_classify_matches (void)
{
  Cb2000NccTelemetry tel = { 0 };

  tel.compared_images = 5;

  /* 1. Strong match */
  tel.best_intensity = 0.46;
  tel.second_intensity = 0.35;
  tel.mean_top3_intensity = 0.35;
  tel.grad_for_best_intensity = 0.30;
  tel.support_count = 4;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 2. Medium match */
  tel.best_intensity = 0.43;
  tel.second_intensity = 0.28;
  tel.mean_top3_intensity = 0.31;
  tel.grad_for_best_intensity = 0.28;
  tel.support_count = 2;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 3. Dominant match (1 strong image, coherent mean/grad) */
  tel.best_intensity = 0.48;
  tel.second_intensity = 0.18;
  tel.mean_top3_intensity = 0.24;
  tel.grad_for_best_intensity = 0.28;
  tel.support_count = 1;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 4. Cross-sample consistent match */
  tel.best_intensity = 0.43;
  tel.second_intensity = 0.35;
  tel.mean_top3_intensity = 0.35;
  tel.grad_for_best_intensity = 0.28;
  tel.support_count = 3;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);
}

static void
test_ncc_classify_rejection_and_nomatch (void)
{
  Cb2000NccTelemetry tel = { 0 };

  tel.compared_images = 5;

  /*
   * Crucial regression test for adjacent/false accept fingers:
   * Middle finger only correlates with ONE gallery image (~0.25-0.26)
   * with zero multi-sample support (top2 < 0.15).
   * MUST NOT MATCH!
   */
  tel.best_intensity = 0.2662;
  tel.second_intensity = 0.1445;
  tel.mean_top3_intensity = 0.1772;
  tel.grad_for_best_intensity = 0.2967;
  tel.support_count = 1;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Borderline middle finger attempt (support=1, top1=0.2512) */
  tel.best_intensity = 0.2512;
  tel.second_intensity = 0.1171;
  tel.mean_top3_intensity = 0.1562;
  tel.grad_for_best_intensity = 0.2362;
  tel.support_count = 1;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Un-enrolled middle finger live telemetry #1 (13:01:28) */
  tel.best_intensity = 0.3114;
  tel.second_intensity = 0.2850;
  tel.mean_top3_intensity = 0.2836;
  tel.grad_for_best_intensity = 0.2634;
  tel.support_count = 4;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Un-enrolled middle finger live telemetry #2 (13:05:17) */
  tel.best_intensity = 0.2405;
  tel.second_intensity = 0.2114;
  tel.mean_top3_intensity = 0.2210;
  tel.grad_for_best_intensity = 0.2410;
  tel.support_count = 4;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Un-enrolled middle finger live telemetry #3 (13:15:11 - False Accept regression) */
  tel.best_intensity = 0.3944;
  tel.second_intensity = 0.3860;
  tel.mean_top3_intensity = 0.3697;
  tel.grad_for_best_intensity = 0.2630;
  tel.support_count = 4;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Clear no-match (completely different finger) */
  tel.best_intensity = 0.1009;
  tel.second_intensity = 0.0646;
  tel.mean_top3_intensity = 0.0759;
  tel.grad_for_best_intensity = 0.1695;
  tel.support_count = 0;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_NO_MATCH);

  /* Zero compared images should return ERROR */
  tel.compared_images = 0;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_ERROR);
}

static void
test_ncc_classify_single_condition_failures (void)
{
  Cb2000NccTelemetry base_tel = { 0 };
  Cb2000NccTelemetry tel;

  /* Perfect baseline that matches Strong profile (best=0.43, second=0.32, mean3=0.32, grad1=0.28, support=2) */
  base_tel.compared_images = 5;
  base_tel.best_intensity = 0.43;
  base_tel.second_intensity = 0.32;
  base_tel.mean_top3_intensity = 0.32;
  base_tel.grad_for_best_intensity = 0.28;
  base_tel.support_count = 2;
  g_assert_cmpint (cb2000_ncc_classify (&base_tel), ==, CB2000_NCC_DECISION_MATCH);

  /* Failure 1: score_sum is too low (best + grad < 0.69 Strong and < 0.67 Medium) */
  tel = base_tel;
  tel.best_intensity = 0.42;
  tel.grad_for_best_intensity = 0.23; /* sum = 0.65 < 0.67 */
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Failure 2: best_intensity is below Strong (0.42) and Medium (0.39) */
  tel = base_tel;
  tel.best_intensity = 0.38;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Failure 3: second_intensity is below Strong (0.28) and Medium (0.24) */
  tel = base_tel;
  tel.second_intensity = 0.20;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Failure 4: mean_top3_intensity below Strong (0.30) and Medium (0.28) */
  tel = base_tel;
  tel.mean_top3_intensity = 0.25;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Failure 5: grad_for_best_intensity below Strong (0.25) and Medium (0.24) */
  tel = base_tel;
  tel.grad_for_best_intensity = 0.22;
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);

  /* Failure 6: support_count insufficient for Strong/Medium (support = 1 < 2) */
  tel = base_tel;
  tel.support_count = 1; /* best=0.43 < 0.44 for Dominant, support=1 < 2 for Strong/Medium */
  g_assert_cmpint (cb2000_ncc_classify (&tel), !=, CB2000_NCC_DECISION_MATCH);
}

static void
test_ncc_classify_nomatch_and_retry_transitions (void)
{
  Cb2000NccTelemetry tel = { 0 };

  tel.compared_images = 5;

  /* 1. Clear NO_MATCH: support == 0, top1 < 0.24, mean3 < 0.20 */
  tel.support_count = 0;
  tel.best_intensity = 0.20;
  tel.mean_top3_intensity = 0.15;
  tel.grad_for_best_intensity = 0.10;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_NO_MATCH);

  /* 2. General NO_MATCH: best_intensity < 0.24 (nomatch_top1) */
  tel.support_count = 1;
  tel.best_intensity = 0.23;
  tel.mean_top3_intensity = 0.20;
  tel.grad_for_best_intensity = 0.20;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_NO_MATCH);

  /* 3. Borderline touch that didn't match and didn't fall below nomatch -> RETRY */
  tel.support_count = 2;
  tel.best_intensity = 0.35;
  tel.second_intensity = 0.20;
  tel.mean_top3_intensity = 0.25;
  tel.grad_for_best_intensity = 0.20;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_RETRY);

  /* 4. Compared images zero -> ERROR */
  tel.compared_images = 0;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_ERROR);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/canvasbio/ncc/decision_labels", test_ncc_decision_labels);
  g_test_add_func ("/canvasbio/ncc/env_helpers", test_ncc_env_helpers);
  g_test_add_func ("/canvasbio/ncc/foreground_mask", test_ncc_foreground_mask);
  g_test_add_func ("/canvasbio/ncc/foreground_mask_edges", test_ncc_foreground_mask_edges);
  g_test_add_func ("/canvasbio/ncc/gradient_magnitude", test_ncc_gradient_magnitude);
  g_test_add_func ("/canvasbio/ncc/gradient_magnitude_edges", test_ncc_gradient_magnitude_edges);
  g_test_add_func ("/canvasbio/ncc/masked_shifts", test_ncc_masked_shifts);
  g_test_add_func ("/canvasbio/ncc/masked_shifts_edge_cases", test_ncc_masked_shifts_edge_cases);
  g_test_add_func ("/canvasbio/ncc/adaptive_relaxation", test_ncc_adaptive_relaxation);
  g_test_add_func ("/canvasbio/ncc/enroll_cohesion", test_ncc_enroll_cohesion);
  g_test_add_func ("/canvasbio/ncc/classify_matches", test_ncc_classify_matches);
  g_test_add_func ("/canvasbio/ncc/classify_single_condition_failures", test_ncc_classify_single_condition_failures);
  g_test_add_func ("/canvasbio/ncc/classify_nomatch_and_retry_transitions", test_ncc_classify_nomatch_and_retry_transitions);
  g_test_add_func ("/canvasbio/ncc/classify_rejection_and_nomatch", test_ncc_classify_rejection_and_nomatch);

  return g_test_run ();
}
