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
test_ncc_masked_shifts (void)
{
  guint8 a[CB2000_IMG_SIZE];
  guint8 b[CB2000_IMG_SIZE];
  guint8 mask_a[CB2000_IMG_SIZE];
  guint8 mask_b[CB2000_IMG_SIZE];
  gdouble score;
  gint x, y;

  /* Generate a synthetic ridge pattern: sin wave along x */
  for (y = 0; y < CB2000_IMG_HEIGHT; y++)
    {
      for (x = 0; x < CB2000_IMG_WIDTH; x++)
        {
          gdouble val = 128.0 + 80.0 * sin (x * 0.4);
          a[y * CB2000_IMG_WIDTH + x] = (guint8) val;
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
}

static void
test_ncc_adaptive_relaxation (void)
{
  Cb2000NccTelemetry tel = { 0 };

  /* Zero pairs: relax must be 0.0 */
  tel.enroll_pair_count = 0;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel), 0.0, 1e-6);

  /* High cohesion: mean = 0.40, min = 0.32 (both above targets 0.36 and 0.28) -> relax = 0.0 */
  tel.enroll_pair_count = 10;
  tel.enroll_pair_mean_intensity = 0.40;
  tel.enroll_pair_min_intensity = 0.32;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel), 0.0, 1e-6);

  /* Moderate cohesion: mean = 0.30 (deficit 0.06), min = 0.20 (deficit 0.08) */
  /* relax = 0.06 * 0.60 + 0.08 * 0.40 = 0.036 + 0.032 = 0.068 -> clamped to max_relax 0.040 */
  tel.enroll_pair_mean_intensity = 0.30;
  tel.enroll_pair_min_intensity = 0.20;
  g_assert_cmpfloat_with_epsilon (cb2000_ncc_compute_adaptive_relax (&tel),
                                  CB2000_NCC_ADAPTIVE_MAX_RELAX_DEFAULT,
                                  1e-6);
}

static void
test_ncc_classify_matches (void)
{
  Cb2000NccTelemetry tel = { 0 };

  tel.compared_images = 5;

  /* 1. Strong match */
  tel.best_intensity = 0.40;
  tel.second_intensity = 0.28;
  tel.mean_top3_intensity = 0.30;
  tel.grad_for_best_intensity = 0.25;
  tel.support_count = 4;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 2. Medium match */
  tel.best_intensity = 0.29;
  tel.second_intensity = 0.21;
  tel.mean_top3_intensity = 0.22;
  tel.grad_for_best_intensity = 0.21;
  tel.support_count = 2;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 3. Dominant match (1 strong image, coherent mean/grad) */
  tel.best_intensity = 0.38;
  tel.second_intensity = 0.19;
  tel.mean_top3_intensity = 0.21;
  tel.grad_for_best_intensity = 0.22;
  tel.support_count = 1;
  g_assert_cmpint (cb2000_ncc_classify (&tel), ==, CB2000_NCC_DECISION_MATCH);

  /* 4. Cross-sample consistent match */
  tel.best_intensity = 0.25;
  tel.second_intensity = 0.24;
  tel.mean_top3_intensity = 0.23;
  tel.grad_for_best_intensity = 0.195;
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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/canvasbio/ncc/decision_labels", test_ncc_decision_labels);
  g_test_add_func ("/canvasbio/ncc/foreground_mask", test_ncc_foreground_mask);
  g_test_add_func ("/canvasbio/ncc/gradient_magnitude", test_ncc_gradient_magnitude);
  g_test_add_func ("/canvasbio/ncc/masked_shifts", test_ncc_masked_shifts);
  g_test_add_func ("/canvasbio/ncc/adaptive_relaxation", test_ncc_adaptive_relaxation);
  g_test_add_func ("/canvasbio/ncc/classify_matches", test_ncc_classify_matches);
  g_test_add_func ("/canvasbio/ncc/classify_rejection_and_nomatch", test_ncc_classify_rejection_and_nomatch);

  return g_test_run ();
}
