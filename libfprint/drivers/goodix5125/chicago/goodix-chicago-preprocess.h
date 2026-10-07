// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: process()/get_state() entry points added. */

/* Native state for the Chicago preprocessing boundary. */

#pragma once

#include <glib.h>

#include "goodix-chicago-calibration.h"

G_BEGIN_DECLS

typedef struct _GoodixChicagoPreprocessor GoodixChicagoPreprocessor;

typedef enum {
  GOODIX_CHICAGO_PREPROCESS_STATUS_OK = 0,
  /* Mode-24 raw-input validation failure. */
  GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT = 0x29aa,
  /* Preprocessor poor-capture return. */
  GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE = 0x7531,
  /* Class code 9: too many class-3 resolution pixels. */
  GOODIX_CHICAGO_PREPROCESS_STATUS_CLASS_REJECT = 0xc351,
} GoodixChicagoPreprocessStatus;

/* Bytes 0..5 of the per-frame preprocessor context (cbuf):
 * { state, hold flag, counter, class code, mask share 0..100, 0 }. */
#define GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE 6

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
  gint upper_innermost_mean;   /* fifth upper mean */
} GoodixChicagoResolutionSecondaryAnalysis;

/* Creates the per-sensor state corresponding to the
 * preprocessor_init(ImageBase) boundary. ImageBase is retained exactly as
 * captured; calibration map CRCs are checked before the state is usable. */
GoodixChicagoPreprocessor *goodix_chicago_preprocessor_new (GBytes       *calibration,
                                                            const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                                            GError      **error);

void goodix_chicago_preprocessor_free (GoodixChicagoPreprocessor *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoPreprocessor,
                               goodix_chicago_preprocessor_free)

/* Mode-0x18 raw-input routine: clamp transport words to 12 bits, then
 * replace columns 1..78 of the first/last rows from their adjacent rows. The
 * four corner samples remain unchanged. */
void goodix_chicago_preprocessor_prepare_raw (GoodixChicagoPreprocessor *self,
                                              const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                              guint16                    prepared[GOODIX_CHICAGO_PIXELS]);

const guint16 *goodix_chicago_preprocessor_get_image_base (const GoodixChicagoPreprocessor *self);

/* preprocessor_init(ImageBase) on a running preprocessor (openchicago): the
 * new ImageBase and the matching rebased @calibration replace the old ones;
 * the adaptive state (kr, framenum, history, ...) is kept. */
gboolean goodix_chicago_preprocessor_rebase (GoodixChicagoPreprocessor *self,
                                             GBytes                    *calibration,
                                             const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                             GError                   **error);

/* Mode-0x18 capture mask. The returned threshold is
 * derived from positive ImageBase-current samples above 120. A mask byte is
 * 0xff when that difference reaches the threshold and current is not the
 * saturated 0x0fff value; otherwise it is zero. */
guint16 goodix_chicago_preprocessor_build_mask (GoodixChicagoPreprocessor *self,
                                                const guint16              current[GOODIX_CHICAGO_PIXELS],
                                                const guint16              image_base[GOODIX_CHICAGO_PIXELS],
                                                guint8                     mask[GOODIX_CHICAGO_PIXELS]);

/* Mode-0x18 local centering. For each enabled
 * pixel, subtract the rounded mean of enabled pixels in the clipped 11x11
 * window and add 3000. Disabled pixels are set to 3000. Inputs and output
 * must not alias. */
void goodix_chicago_preprocessor_center_local_mean (GoodixChicagoPreprocessor *self,
                                                    const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                    const guint8               mask[GOODIX_CHICAGO_PIXELS],
                                                    guint16                    centered[GOODIX_CHICAGO_PIXELS]);

/* In-place interior smoothing: a rounded 3x3
 * [1 2 1; 2 4 2; 1 2 1] / 16 kernel. Border pixels are retained unchanged. */
void goodix_chicago_preprocessor_smooth_centered (GoodixChicagoPreprocessor *self,
                                                  guint16                    centered[GOODIX_CHICAGO_PIXELS]);

/* Local envelopes: clipped 11-sample horizontal and vertical extrema are combined into a
 * cross-shaped local maximum and minimum for every 80x64 pixel. */
void goodix_chicago_preprocessor_build_local_envelopes (GoodixChicagoPreprocessor *self,
                                                        const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                        guint16                    local_max[GOODIX_CHICAGO_PIXELS],
                                                        guint16                    local_min[GOODIX_CHICAGO_PIXELS]);

/* Diagonal-envelope refinement. For each 80x64
 * pixel, the first output is the minimum of local_max at the center and valid
 * diagonal neighbors; the second is the corresponding maximum of local_min.
 * Inputs and outputs must not alias. */
void goodix_chicago_preprocessor_refine_envelopes (GoodixChicagoPreprocessor *self,
                                                   const guint16              local_max[GOODIX_CHICAGO_PIXELS],
                                                   const guint16              local_min[GOODIX_CHICAGO_PIXELS],
                                                   guint16                    refined_min[GOODIX_CHICAGO_PIXELS],
                                                   guint16                    refined_max[GOODIX_CHICAGO_PIXELS]);

/* Compose the mode-0x18 image path. `source`
 * is the normalized 16-bit state plane, not a transport raw frame.
 * The result is the 5,120-byte candidate scored by the branch forest. */
void goodix_chicago_preprocessor_build_candidate (GoodixChicagoPreprocessor *self,
                                                  const guint16              source[GOODIX_CHICAGO_PIXELS],
                                                  const guint8               mask[GOODIX_CHICAGO_PIXELS],
                                                  guint8                     candidate[GOODIX_CHICAGO_PIXELS]);

/* Mode-24 branch forest after the primary and
 * alternate candidates have been scored. Scores are the candidate's
 * column-axis variation, correlation_q8 is their masked Pearson correlation,
 * and reference_score is 600 in ChicagoHS path. */
gboolean goodix_chicago_preprocessor_select_alternate_mode24 (gint primary_score,
                                                              gint alternate_score,
                                                              gint correlation_q8,
                                                              gint reference_score);

/* Mode-0x18 input plane produced for already prepared
 * current/ImageBase samples. Unlike the enhanced-image source, this path does
 * not apply calibration gain. */
void goodix_chicago_preprocessor_build_resolution_base_plane (const guint16 current[GOODIX_CHICAGO_PIXELS],
                                                              const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                                              guint16       resolution_base[GOODIX_CHICAGO_PIXELS]);

/* Selector-10 smoothing used to turn the resolution base
 * into the secondary classifier plane. */
void goodix_chicago_preprocessor_build_resolution_secondary_plane (const guint16 resolution_base[GOODIX_CHICAGO_PIXELS],
                                                                   guint16       secondary[GOODIX_CHICAGO_PIXELS]);

/* Eight-neighbor extrema stage. Directions use the
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

/* Primary classifier plane, including selector-6 smoothing of
 * max(falling, rising). The exceptional branch follows falling gradients and
 * reverses the strict secondary threshold comparison. */
void goodix_chicago_preprocessor_build_resolution_primary_plane (const guint16 secondary[GOODIX_CHICAGO_PIXELS],
                                                                 const guint8  input_mask[GOODIX_CHICAGO_PIXELS],
                                                                 gint          secondary_threshold,
                                                                 gint          gradient_threshold,
                                                                 gboolean      exceptional,
                                                                 guint16       primary[GOODIX_CHICAGO_PIXELS]);

/* Statistics used immediately before and
 * by the label-threshold calculator. */
gint goodix_chicago_preprocessor_calculate_resolution_gradient_threshold (const guint16 filtered_gradient[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8  input_mask[GOODIX_CHICAGO_PIXELS]);

void goodix_chicago_preprocessor_calculate_resolution_primary_statistics (const guint16                      primary[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8                       input_mask[GOODIX_CHICAGO_PIXELS],
                                                                          GoodixChicagoResolutionStatistics *statistics);

/* Secondary-plane analysis. It supplies both
 * cutoffs and the availability of the exceptional pre-pass. */
void goodix_chicago_preprocessor_calculate_resolution_secondary_analysis (const guint16                             secondary[GOODIX_CHICAGO_PIXELS],
                                                                          const guint8                              input_mask[GOODIX_CHICAGO_PIXELS],
                                                                          GoodixChicagoResolutionSecondaryAnalysis *analysis);

/* Gate between secondary analysis and the exceptional/normal
 * classifier passes. The gradient statistics are calculated from the
 * selector-6 filtered maximum-gradient plane. */
void goodix_chicago_preprocessor_select_resolution_branches (const GoodixChicagoResolutionSecondaryAnalysis *analysis,
                                                             const GoodixChicagoResolutionStatistics        *gradient_statistics,
                                                             gboolean                                       *use_exceptional,
                                                             gboolean                                       *use_normal,
                                                             gint                                           *branch_state);

/* Mode-0x18 threshold arithmetic shared by the normal and exceptional
 * label generators.  The statistics fields are the scalar outputs of the
 * histogram stage. */
void goodix_chicago_preprocessor_calculate_resolution_thresholds (const GoodixChicagoResolutionStatistics *statistics,
                                                                  gboolean                                 exceptional,
                                                                  GoodixChicagoResolutionThresholds       *thresholds);

/* Normal-branch label generator, including the
 * eight-connected promotion pass. `labels` may contain labels from
 * the preceding exceptional branch; those are preserved unless a hard
 * class-3 rule or connected promotion supersedes them. `promotion_mask` is
 * returned as the 0/0xff mask. The return value is the number of
 * class-3 seeds before connected promotion. */
guint goodix_chicago_preprocessor_build_resolution_labels (const guint16                            primary[GOODIX_CHICAGO_PIXELS],
                                                           const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
                                                           const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
                                                           const GoodixChicagoResolutionThresholds *thresholds,
                                                           guint8                                   labels[GOODIX_CHICAGO_PIXELS],
                                                           guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS]);

/* Exceptional pre-pass. This branch reverses
 * the hard secondary comparison and adds the primary-mid/secondary-mid rule
 * used by the mode-0x18 classifier on selected captures. */
guint goodix_chicago_preprocessor_build_exceptional_resolution_labels (const guint16                            primary[GOODIX_CHICAGO_PIXELS],
                                                                       const guint16                            secondary[GOODIX_CHICAGO_PIXELS],
                                                                       const guint8                             input_mask[GOODIX_CHICAGO_PIXELS],
                                                                       const GoodixChicagoResolutionThresholds *thresholds,
                                                                       guint8                                   labels[GOODIX_CHICAGO_PIXELS],
                                                                       guint8                                   promotion_mask[GOODIX_CHICAGO_PIXELS]);

/* Class-count boundary. Labels 1 and 2 are cleared
 * in place; labels 0 and 3 are retained. The return value is
 * the number of class-3 pixels. */
guint goodix_chicago_preprocessor_classify_resolution_labels (guint     mode,
                                                              guint8    labels[GOODIX_CHICAGO_PIXELS],
                                                              guint     pixel_count,
                                                              guint     valid_pixel_count,
                                                              guint    *code_out,
                                                              gboolean *auxiliary_out);

/* Mode-0x18 wrapper around Chicago's core quality/coverage result.
 * `base_quality` and `coverage` are the signed integer outputs;
 * the returned values apply the bias, low-coverage penalty, and
 * final 0..100 clamps. */
void goodix_chicago_preprocessor_finalize_metrics (gint    base_quality,
                                                   gint    coverage,
                                                   guint8 *quality_out,
                                                   guint8 *coverage_out);

/* Cleaned mask used by the quality core. A pixel is
* cleared only when it and all available axial neighbors at distance one and
* two are saturated (0xff) in the enhanced image. */
void goodix_chicago_preprocessor_build_quality_mask (const guint8 enhanced[GOODIX_CHICAGO_PIXELS],
                                                     guint8       quality_mask[GOODIX_CHICAGO_PIXELS]);

/* process() plus the per-frame context (cbuf[0..5]). `hold`
 * marks a held frame (context flag 2): the resolution history is not updated and the saved state/code are
 * merged in. `context` may be NULL. The history behind the context lives in
 * the preprocessor object. */
GoodixChicagoPreprocessStatus goodix_chicago_preprocessor_process_context (GoodixChicagoPreprocessor *self,
                                                                           const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                                                           gint                       purpose,
                                                                           gboolean                   hold,
                                                                           guint8                     enhanced[GOODIX_CHICAGO_PIXELS],
                                                                           guint8                    *quality,
                                                                           guint8                    *coverage,
                                                                           guint8                     context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE]);

/* Read-only view of the adaptive state (for tests and calibration saving). */
typedef struct
{
  guint32        framenum;
  guint32        multiplier_count;
  const guint16 *kr;
  const guint16 *gain;
  const guint16 *multiplier;
  const guint16 *auxiliary;
} GoodixChicagoPreprocessState;

void goodix_chicago_preprocessor_get_state (const GoodixChicagoPreprocessor *self,
                                            GoodixChicagoPreprocessState    *state);

/* Every adaptive variable (including the context history), little endian, fixed order.  Restore with
 * preprocessor_new(same calibration, same ImageBase) + load_state(). */
gsize goodix_chicago_preprocessor_state_size (void);
void goodix_chicago_preprocessor_save_state (const GoodixChicagoPreprocessor *self,
                                             GByteArray                      *out);
gboolean goodix_chicago_preprocessor_load_state (GoodixChicagoPreprocessor *self,
                                                 const guint8              *data,
                                                 gsize                      size,
                                                 GError                   **error);
gboolean goodix_chicago_preprocessor_state_equal (const GoodixChicagoPreprocessor *a,
                                                  const GoodixChicagoPreprocessor *b);
/* The calibration payload given to preprocessor_new() (not a new reference). */
GBytes *goodix_chicago_preprocessor_get_calibration (const GoodixChicagoPreprocessor *self);

G_END_DECLS
