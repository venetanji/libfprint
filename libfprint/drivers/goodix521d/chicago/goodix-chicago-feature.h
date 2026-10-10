// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: complete type-24 feature extraction state. */

/* Native state for Chicago feature extraction. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GOODIX_CHICAGO_FEATURE_WIDTH 80u
#define GOODIX_CHICAGO_FEATURE_HEIGHT 64u
#define GOODIX_CHICAGO_FEATURE_PIXELS \
  (GOODIX_CHICAGO_FEATURE_WIDTH * GOODIX_CHICAGO_FEATURE_HEIGHT)
#define GOODIX_CHICAGO_FEATURE_SCALES 9u
#define GOODIX_CHICAGO_FEATURE_RECORD_LIMIT 120u

typedef struct
{
  gint32 x;
  gint32 y;
  gint32 scale;
  gint32 response;
} GoodixChicagoExtremum;

/* 24-byte candidate layout of the subpixel refinement. strength initially contains the raw DoG response. */
typedef struct
{
  gint32 x;
  gint32 y;
  gint32 scale;
  gint32 strength;
  gint16 refined_x;
  gint16 refined_y;
  gint32 scale_value;
} GoodixChicagoCandidate;

typedef struct
{
  guint16 foreground;
  gint16  refined_x;
  gint16  refined_y;
  gint16  orientation;
  gint32  strength;
  guint8  descriptor[48];
} GoodixChicagoFeatureRecord;

typedef struct
{
  gint32 strength;
  gint32 index;
} GoodixChicagoFeatureRank;

typedef struct
{
  gint32  x;
  gint32  y;
  gint32  scale_value;
  guint32 peak;
  guint32 selected_peak;
  guint16 secondary_orientation;
  guint16 reserved;
} GoodixChicagoFeatureAux;

/* Transient image-derived fields of a live subtemplate. They are used while matching a live probe
 * but are not copied into the packed gallery subtemplate. */
typedef struct
{
  guint negative_percent;
  guint positive_percent;
  guint neutral_percent;
  guint density_class;
  guint inactive_count;
} GoodixChicagoFeatureConsensus;

/* Live-only six-byte record of a subtemplate.  The type-24 path obtains byte zero from the preprocessor resolution context and
 * byte three from the decoded packed-resolution scale. */
typedef struct
{
  guint8 values[6];
} GoodixChicagoFeatureLiveAuxiliary;

/* Radius-six structure-tensor orientation map built by
 * from the enhanced image. */
void goodix_chicago_feature_build_orientation_map (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   guint8       orientation[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Orientation-guided seven-sample image returned by
 * and passed as the first argument. */
void goodix_chicago_feature_build_prepared_image (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  guint8       prepared[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Mode-24 input.  The image header keeps the sensor transport geometry
 * (80 columns x 64 rows); the byte stream is filtered with that geometry by selector
 * 6. */
void goodix_chicago_feature_build_source (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          guint8       source[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Nine-plane Q16 Gaussian scale space built.
 * Plane zero and one share the feature source; planes two through eight are
 * successively filtered from the preceding plane. */
void goodix_chicago_feature_build_scale_space (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               guint16      scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                               [GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Mode-24 difference-of-Gaussian extrema scan. The returned
 * records are ordered by scale 6..1, then y, then x. */
guint goodix_chicago_feature_collect_extrema (const guint16          scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                              [GOODIX_CHICAGO_FEATURE_PIXELS],
                                              GoodixChicagoExtremum *extrema,
                                              guint                  capacity);

/* Mode-24 subpixel refinement and acceptance boundary. The candidate may be
 * moved even when rejected. On success, the fixed-point coordinates,
 * scale value, strength, and curvature are populated. */
gboolean goodix_chicago_feature_refine_extremum (const guint16           scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                                 [GOODIX_CHICAGO_FEATURE_PIXELS],
                                                 GoodixChicagoCandidate *candidate,
                                                 guint32                *curvature);

/* Mode-24 single-orientation materialization.
 * Descriptor bytes remain zero at this boundary and are populated by the
 * following stage. */
void goodix_chicago_feature_materialize_candidate (const guint8                  feature_source[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const guint32                 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const gint16                  orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const GoodixChicagoCandidate *candidate,
                                                   guint                         index,
                                                   GoodixChicagoFeatureRecord   *record,
                                                   GoodixChicagoFeatureRank     *rank,
                                                   GoodixChicagoFeatureAux      *aux);

/* Mode-24 descriptor stage. This fills the binary descriptor
 * fields and status byte of an already materialized feature record. */
void goodix_chicago_feature_build_descriptor (const guint32                  magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                              const gint16                   orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
                                              const GoodixChicagoFeatureAux *aux,
                                              GoodixChicagoFeatureRecord    *record);

/* Mode-24 caller-side finalization after the descriptor stage. */
void goodix_chicago_feature_finalize_record (GoodixChicagoFeatureRecord *record);

/* Mode-24 feature mask: selector 7 smoothing,
 * reflected Sobel/local mean, threshold 110, and output value 1. */
void goodix_chicago_feature_build_mask (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                        guint8       mask[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Record annotations.  They correspond to the finalized record order and
 * intentionally precede mask compaction. */
void goodix_chicago_feature_build_annotations (const guint8                      enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               const guint8                      mask[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               const GoodixChicagoFeatureRecord *records,
                                               guint                             count,
                                               guint8                           *annotations);

/* The same status pass, retaining its otherwise transient result
 * scalars. @template_type controls the 5/10 percent class threshold;
 * the normal sensor path uses type 24. */
gboolean goodix_chicago_feature_classify_status_full (GoodixChicagoFeatureRecord    *records,
                                                      guint                          count,
                                                      guint                          template_type,
                                                      GoodixChicagoFeatureConsensus *consensus);

/* Mask gate. Records whose rounded coordinate
 * lands on a zero mask pixel are removed by replacing them with the current
 * tail record; the resulting order matters for matching. */
guint goodix_chicago_feature_filter_mask (const guint8                mask[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          GoodixChicagoFeatureRecord *records,
                                          guint                       count);

/* On-record representation conversion. The normal
 * enrollment path passes reverse_bits=FALSE; TRUE reproduces the alternate
 * descriptor bit/tail conversion used by other modes. */
void goodix_chicago_feature_pack_record (GoodixChicagoFeatureRecord *record,
                                         gboolean                    reverse_bits);

/* In-place class partition. Class zero records are
 * moved ahead of class one records. The returned value is the class-zero
 * count, which is distinct from the total record count. If supplied,
 * annotations are swapped in parallel with their records. */
guint goodix_chicago_feature_partition_records (GoodixChicagoFeatureRecord *records,
                                                guint8                     *annotations,
                                                guint                       count);

/* Record[0x39] local-neighbor score. The
 * annotation array must already have undergone the same swaps as
 * its records. */
void goodix_chicago_feature_score_neighbors (GoodixChicagoFeatureRecord *records,
                                             const guint8               *annotations,
                                             guint                       count);

/* First-pass signed input assembled. Selector 0
 * filters its prepared byte-image argument, selector 1 filters that result,
 * then the two planes are combined as 10 * filter0 - 9 * filter1. */
void goodix_chicago_feature_build_gradient_input (const guint8 feature_image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  gint32       input[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Central-difference magnitude and Q12-radian orientation images.  Its
 * signed 32-bit input is the scale-pair gradient input.  Only the inner 62 x 78 pixels are populated. */
void goodix_chicago_feature_build_gradients (const gint32 input[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             guint32      magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             gint16       orientation[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Feature extraction (template type 24) as a whole.  Cross-frame state and
 * the identify consensus live in this struct, owned by the openchicago
 * session.  Initialise with goodix_chicago_feature_state_init(). */
typedef struct
{
  gint                          last_coverage;
  gint                          history_count;
  gint                          stable_count;
  gint                          last_level;
  guint8                        history[3][GOODIX_CHICAGO_FEATURE_PIXELS];
  GoodixChicagoFeatureConsensus consensus;      /* identify consensus */
} GoodixChicagoFeatureState;

void goodix_chicago_feature_state_init (GoodixChicagoFeatureState *state);

gboolean goodix_chicago_feature_decode_mark (const guint8 image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             guint32     *packed,
                                             guint8       region[GOODIX_CHICAGO_FEATURE_PIXELS]);

void goodix_chicago_feature_pack_record_mode (GoodixChicagoFeatureRecord *record,
                                              gboolean                    identify);

/* @context: bytes 0..5 of the preprocessor's per-frame buffer (cbuf).
 * @identify: 0 for enrolment, 1 for identify.
 * Returns the record count. */
guint goodix_chicago_feature_get_feature (GoodixChicagoFeatureState     *state,
                                          const guint8                   image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          const guint8                   context[6],
                                          guint                          quality,
                                          guint                          coverage,
                                          gboolean                       identify,
                                          GoodixChicagoFeatureRecord    *records,
                                          guint                          capacity,
                                          guint                         *active_count,
                                          GoodixChicagoFeatureConsensus *consensus,
                                          guint32                       *packed_resolution,
                                          guint8                         live_auxiliary[6]);

G_END_DECLS
