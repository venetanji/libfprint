// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Native state for the recovered Chicago preprocessing boundary. */

#pragma once

#include <glib.h>

#include "goodix-chicago-calibration.h"

G_BEGIN_DECLS

typedef struct _GoodixChicagoPreprocessor GoodixChicagoPreprocessor;

typedef enum {
  GOODIX_CHICAGO_PREPROCESS_STATUS_OK = 0,
  /* Exact mode-24 raw-input validation failure. */
  GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT = 0x29aa,
  /* Exact AlgoChicago preprocessor poor-capture return. */
  GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE = 0x7531,
} GoodixChicagoPreprocessStatus;

typedef struct
{
  gint secondary_hard;
  gint primary_high;
  gint primary_mid;
  gint secondary_mid;
  gint primary_low;
  gint promotion;
} GoodixChicagoResolutionThresholds;

typedef struct
{
  gint primary_center_a;
  gint primary_center_b;
  gint primary_high_sample;
  gint primary_mid_sample;
  gint branch_state;
  gint secondary_base;
  gint secondary_mid_sample;
} GoodixChicagoResolutionStatistics;

typedef struct
{
  gint upper_cutoff;
  gint upper_state;
  gint lower_cutoff;
  gint lower_state;
  gint upper_mid_sample;
  gint balance_center;
  gint upper_outer_mean;
  gint upper_inner_mean;
  gint lower_outer_mean;
  gint lower_inner_mean;
  gint peak_state;
  gint peak_value;
} GoodixChicagoResolutionSecondaryAnalysis;

/* Creates the per-sensor state corresponding to the official
 * preprocessor_init(ImageBase) boundary. ImageBase is retained exactly as
 * captured; calibration map CRCs are checked before the state is usable. */
GoodixChicagoPreprocessor *goodix_chicago_preprocessor_new (GBytes       *calibration,
                                                            const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                                            GError      **error);

void goodix_chicago_preprocessor_free (GoodixChicagoPreprocessor *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoPreprocessor,
                               goodix_chicago_preprocessor_free)

/* Exact mode-0x18 raw-input routine: clamp transport words to 12 bits, then
 * replace columns 1..78 of the first/last rows from their adjacent rows. The
 * four corner samples remain unchanged. */
void goodix_chicago_preprocessor_prepare_raw (GoodixChicagoPreprocessor *self,
                                              const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                              guint16                    prepared[GOODIX_CHICAGO_PIXELS]);

const guint16 *goodix_chicago_preprocessor_get_image_base (const GoodixChicagoPreprocessor *self);

gboolean goodix_chicago_preprocessor_get_corrections (const GoodixChicagoPreprocessor *self,
                                                      guint                            pixel,
                                                      guint16                         *gain,
                                                      guint16                         *offset);

/* Exact first calibration stage recovered from AlgoChicago+0x4d810. It
 * scales the CRC-validated gain map to a mean of 0x2000 with the vendor's
 * rounded integer arithmetic. A zero-mean map has no usable vendor output. */
gboolean goodix_chicago_preprocessor_get_normalized_gain (const GoodixChicagoPreprocessor *self,
                                                          guint                            pixel,
                                                          guint16                         *normalized_gain);

/* Exact +0x4b300 source-plane result for already prepared current/ImageBase
 * inputs. This applies the normalized calibration gain with vendor rounding.
 * Inputs and output must not alias. */
void goodix_chicago_preprocessor_build_source_plane (GoodixChicagoPreprocessor *self,
                                                     const guint16              current[GOODIX_CHICAGO_PIXELS],
                                                     const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                                     guint16                    source[GOODIX_CHICAGO_PIXELS]);

/* Exact mode-0x18 mask built before +0x43a00. The returned threshold is
 * derived from positive ImageBase-current samples above 120. A mask byte is
 * 0xff when that difference reaches the threshold and current is not the
 * saturated 0x0fff value; otherwise it is zero. */
guint16 goodix_chicago_preprocessor_build_mask (GoodixChicagoPreprocessor *self,
                                                const guint16              current[GOODIX_CHICAGO_PIXELS],
                                                const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                                guint8                     mask[GOODIX_CHICAGO_PIXELS]);

/* Exact mode-0x18 local centering at AlgoChicago+0x4bee0. For each enabled
 * pixel, subtract the rounded mean of enabled pixels in the clipped 11x11
 * window and add 3000. Disabled pixels are set to 3000. Inputs and output
 * must not alias. */
void goodix_chicago_preprocessor_center_local_mean (GoodixChicagoPreprocessor *self,
                                                    const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                    const guint8               mask[GOODIX_CHICAGO_PIXELS],
                                                    guint16                    centered[GOODIX_CHICAGO_PIXELS]);

/* Exact in-place interior smoothing at AlgoChicago+0x4d2e0: a rounded 3x3
 * [1 2 1; 2 4 2; 1 2 1] / 16 kernel. Border pixels are retained unchanged. */
void goodix_chicago_preprocessor_smooth_centered (GoodixChicagoPreprocessor *self,
                                                  guint16                    centered[GOODIX_CHICAGO_PIXELS]);

/* Exact result of AlgoChicago+0x481e0 and +0x45a40 plus their combining loop:
 * clipped 11-sample horizontal and vertical extrema are combined into a
 * cross-shaped local maximum and minimum for every 80x64 pixel. */
void goodix_chicago_preprocessor_build_local_envelopes (GoodixChicagoPreprocessor *self,
                                                        const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                        guint16                    local_max[GOODIX_CHICAGO_PIXELS],
                                                        guint16                    local_min[GOODIX_CHICAGO_PIXELS]);

/* Exact diagonal-envelope refinement at AlgoChicago+0x46930. For each 80x64
 * pixel, the first output is the minimum of local_max at the center and valid
 * diagonal neighbors; the second is the corresponding maximum of local_min.
 * Inputs and outputs must not alias. */
void goodix_chicago_preprocessor_refine_envelopes (GoodixChicagoPreprocessor *self,
                                                   const guint16              local_max[GOODIX_CHICAGO_PIXELS],
                                                   const guint16              local_min[GOODIX_CHICAGO_PIXELS],
                                                   guint16                    refined_min[GOODIX_CHICAGO_PIXELS],
                                                   guint16                    refined_max[GOODIX_CHICAGO_PIXELS]);

/* Compose the recovered mode-0x18 AlgoChicago+0x43a00 image path. `source`
 * is the upstream 16-bit +0x4b300 state plane, not a transport raw frame.
 * The result is the 5,120-byte candidate later consumed by +0x47450. */
void goodix_chicago_preprocessor_build_candidate (GoodixChicagoPreprocessor *self,
                                                  const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                  const guint8               mask[GOODIX_CHICAGO_PIXELS],
                                                  guint8                     candidate[GOODIX_CHICAGO_PIXELS]);

/* Exact mode-24 branch forest at AlgoChicago+0x47450 after the primary and
 * alternate candidates have been scored. Scores are the candidate's
 * column-axis variation, correlation_q8 is their masked Pearson correlation,
 * and reference_score is 600 in the production ChicagoHS path. */
gboolean goodix_chicago_preprocessor_select_alternate_mode24 (gint primary_score,
                                                              gint alternate_score,
                                                              gint correlation_q8,
                                                              gint reference_score);

/* Compose the exact recovered preprocessing path for already prepared
 * current and ImageBase planes. */
void goodix_chicago_preprocessor_build_enhanced_prepared (GoodixChicagoPreprocessor *self,
                                                          const guint16              current[GOODIX_CHICAGO_PIXELS],
                                                          const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                                          guint8                     enhanced[GOODIX_CHICAGO_PIXELS]);

/* Complete recovered raw-frame to enhanced-image path. The retained raw
 * ImageBase and the current transport frame are prepared internally. */
void goodix_chicago_preprocessor_build_enhanced (GoodixChicagoPreprocessor *self,
                                                 const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                                 guint8                     enhanced[GOODIX_CHICAGO_PIXELS]);

/* Complete raw-frame path plus the recovered mode-24 capture-policy result.
 * The official wrapper applies this local-contrast rejection only while the
 * finalized quality is below 35; low exported quality or coverage alone is
 * not a bad capture. The enhanced image is produced for OK and POOR_CAPTURE;
 * BAD_INPUT returns before the official candidate builder and zeroes it. */
GoodixChicagoPreprocessStatus goodix_chicago_preprocessor_build_enhanced_checked (GoodixChicagoPreprocessor *self,
                                                                                  const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                                                                  guint8                     enhanced[GOODIX_CHICAGO_PIXELS]);

/* Exact fixed 80x64 classifier mask: a six-pixel disabled border surrounding
 * the 68x52 enabled interior (3,536 pixels). */
void goodix_chicago_preprocessor_build_resolution_input_mask (guint8 input_mask[GOODIX_CHICAGO_PIXELS]);

/* Exact mode-0x18 input plane produced by +0x386a0 for already prepared
 * current/ImageBase samples. Unlike the enhanced-image source, this path does
 * not apply calibration gain. */
void goodix_chicago_preprocessor_build_resolution_base_plane (const guint16 current[GOODIX_CHICAGO_PIXELS],
                                                              const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                                              guint16       resolution_base[GOODIX_CHICAGO_PIXELS]);

/* Exact selector-10 smoothing used by +0x3f4c0 to turn the resolution base
 * into the secondary classifier plane. */
void goodix_chicago_preprocessor_build_resolution_secondary_plane (const guint16 resolution_base[GOODIX_CHICAGO_PIXELS],
                                                                   guint16       secondary[GOODIX_CHICAGO_PIXELS]);

/* Exact eight-neighbor extrema stage at +0x3b5a0. Directions use the vendor
 * order left, right, up, down, up-left, down-left, up-right, down-right;
 * 0xff means that no strictly larger difference exists. */
guint goodix_chicago_preprocessor_build_resolution_gradients (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                              const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                              guint16       falling[GOODIX_CHICAGO_PIXELS],
                                                              guint8        falling_directions[GOODIX_CHICAGO_PIXELS],
                                                              guint16       rising[GOODIX_CHICAGO_PIXELS],
                                                              guint8        rising_directions[GOODIX_CHICAGO_PIXELS]);

void goodix_chicago_preprocessor_build_resolution_filtered_gradient (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                                     const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                                     guint16       filtered_gradient[GOODIX_CHICAGO_PIXELS]);

/* Exact +0x3c860 primary classifier plane, including selector-6 smoothing of
 * max(falling, rising). The exceptional branch follows falling gradients and
 * reverses the strict secondary threshold comparison. */
void goodix_chicago_preprocessor_build_resolution_primary_plane (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                                 const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                                 gint          secondary_threshold,
                                                                 gint          gradient_threshold,
                                                                 gboolean      exceptional,
                                                                 guint16       primary[GOODIX_CHICAGO_PIXELS]);

/* Exact +0x3b890/+0x40c70 statistics used immediately before +0x3c860 and
 * by the label-threshold calculator. */
gint goodix_chicago_preprocessor_calculate_resolution_gradient_threshold (const guint16 filtered_gradient[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8  input_mask[GOODIX_CHICAGO_PIXELS]);

void goodix_chicago_preprocessor_calculate_resolution_primary_statistics (const guint16                      primary[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8                       input_mask[GOODIX_CHICAGO_PIXELS],
                                                                          GoodixChicagoResolutionStatistics *statistics);

/* Exact +0x41330/+0x39e70 secondary-plane analysis. It supplies both
 * +0x3c860 cutoffs and the availability of the exceptional pre-pass. */
void goodix_chicago_preprocessor_calculate_resolution_secondary_analysis (const guint16                             secondary[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8                              input_mask[GOODIX_CHICAGO_PIXELS],
                                                                          GoodixChicagoResolutionSecondaryAnalysis *analysis);

/* Exact +0x3f4c0 gate between secondary analysis and the exceptional/normal
 * classifier passes. The gradient statistics are calculated from the
 * selector-6 filtered maximum-gradient plane. */
void goodix_chicago_preprocessor_select_resolution_branches (const GoodixChicagoResolutionSecondaryAnalysis *analysis,
                                                             const GoodixChicagoResolutionStatistics        *gradient_statistics,
                                                             gboolean                                       *use_exceptional,
                                                             gboolean                                       *use_normal,
                                                             gint                                           *branch_state);

/* Exact mode-0x18 threshold arithmetic shared by +0x3ac50 and +0x38ae0.
 * The statistics fields correspond to the scalar outputs of the upstream
 * vendor histogram stage. */
void goodix_chicago_preprocessor_calculate_resolution_thresholds (const GoodixChicagoResolutionStatistics *statistics,
                                                                  gboolean                                 exceptional,
                                                                  GoodixChicagoResolutionThresholds       *thresholds);

/* Exact normal-branch label generator at AlgoChicago+0x3ac50, including the
 * +0x40f60 eight-connected promotion pass. `labels` may contain labels from
 * the preceding exceptional branch; those are preserved unless a hard
 * class-3 rule or connected promotion supersedes them. `promotion_mask` is
 * returned as the official 0/0xff mask. The return value is the number of
 * class-3 seeds before connected promotion. */
guint goodix_chicago_preprocessor_build_resolution_labels (const guint16                            primary[GOODIX_CHICAGO_PIXELS],
                                                           const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
                                                           const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
                                                           const GoodixChicagoResolutionThresholds *thresholds,
                                                           guint8                                   labels[GOODIX_CHICAGO_PIXELS],
                                                           guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS]);

/* Exact exceptional pre-pass at AlgoChicago+0x38ae0. This branch reverses
 * the hard secondary comparison and adds the primary-mid/secondary-mid rule
 * used by the official mode-0x18 classifier on selected captures. */
guint goodix_chicago_preprocessor_build_exceptional_resolution_labels (const guint16                            primary[GOODIX_CHICAGO_PIXELS],
                                                                       const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
                                                                       const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
                                                                       const GoodixChicagoResolutionThresholds *thresholds,
                                                                       guint8                                   labels[GOODIX_CHICAGO_PIXELS],
                                                                       guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS]);

/* Composes the recovered +0x41330, +0x3f4c0, +0x3c860, +0x38ae0 and
 * +0x3ac50 boundaries into the official pre-final resolution labels. */
void goodix_chicago_preprocessor_build_resolution_map_labels (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                              const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                              guint8        labels[GOODIX_CHICAGO_PIXELS]);

/* The same map generator, retaining the otherwise transient first byte of
 * the six-byte preprocessor context consumed by feature construction.  For
 * production mode 0x18 this is +0x41330's peak-state output. */
void goodix_chicago_preprocessor_build_resolution_map_labels_full (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                                   const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                                   guint8        labels[GOODIX_CHICAGO_PIXELS],
                                                                   guint        *peak_state_out);

/* Raw-frame entry point for the pre-final mode-0x18 resolution map. */
void goodix_chicago_preprocessor_build_resolution_map (GoodixChicagoPreprocessor *self,
                                                       const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                                       guint8                     labels[GOODIX_CHICAGO_PIXELS]);

void goodix_chicago_preprocessor_build_resolution_map_full (GoodixChicagoPreprocessor *self,
                                                            const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                                            guint8                     labels[GOODIX_CHICAGO_PIXELS],
                                                            guint                     *peak_state_out);

/* Exact AlgoChicago+0x45890 class-count boundary. Labels 1 and 2 are cleared
 * in place, as in the DLL; labels 0 and 3 are retained. The return value is
 * the number of class-3 pixels. */
guint goodix_chicago_preprocessor_classify_resolution_labels (guint     mode,
                                                              guint8    labels[GOODIX_CHICAGO_PIXELS],
                                                              guint     pixel_count,
                                                              guint     valid_pixel_count,
                                                              guint    *code_out,
                                                              gboolean *auxiliary_out);

/* Exact AlgoChicago+0x54080 discrete-code mapping, packed in the high byte
 * consumed by +0x53210. The production mode-0x18 path uses low code zero. */
guint goodix_chicago_preprocessor_pack_resolution_code (guint code);

/* Exact mode-0x18 wrapper around Chicago's core quality/coverage result.
 * `base_quality` and `coverage` are the signed integer outputs of +0x10030;
 * the returned values reproduce +0x10220's bias, low-coverage penalty, and
 * final 0..100 clamps. */
void goodix_chicago_preprocessor_finalize_metrics (gint    base_quality,
                                                   gint    coverage,
                                                   guint8 *quality_out,
                                                   guint8 *coverage_out);

/* Exact AlgoChicago+0x509b0 enhanced-image coverage path: fixed Q16 Gaussian,
 * Sobel magnitude, reflected 15x15 local mean, and threshold-120 active-area
 * ratio, converted with the official 16.16-to-percent truncation. */
gint goodix_chicago_preprocessor_compute_coverage (const guint8 enhanced[GOODIX_CHICAGO_PIXELS]);

/* Exact mode-0x18 core texture quality at +0x10650. `quality_mask` is the
 * cleaned 0/0xff mask produced by the preceding +0x10410 stage. The function
 * does not reproduce the vendor's later in-place mask annotation. */
gint goodix_chicago_preprocessor_compute_base_quality (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                                       const guint8 quality_mask[GOODIX_CHICAGO_PIXELS]);

/* Exact +0x10410/+0x52a10 cleaned mask used by the quality core. A pixel is
 * cleared only when it and all available axial neighbors at distance one and
 * two are saturated (0xff) in the enhanced image. */
void goodix_chicago_preprocessor_build_quality_mask (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                                     guint8       quality_mask[GOODIX_CHICAGO_PIXELS]);

gint goodix_chicago_preprocessor_compute_base_quality_from_enhanced (const guint8 enhanced[GOODIX_CHICAGO_PIXELS]);

G_END_DECLS
