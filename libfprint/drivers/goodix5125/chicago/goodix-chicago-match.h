// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: see goodix-chicago-match.c. */

/* Chicago identify scoring. */

#pragma once

#include <glib.h>

#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-feature.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_MATCH_MATRIX_STRIDE 180u
#define GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT 42u
#define GOODIX_CHICAGO_MATCH_PAIR_LIMIT 31u

typedef struct
{
  guint old_begin;
  guint old_end;
  guint new_begin;
  guint new_end;
  guint first_half_gate;
  guint combined_gate;
} GoodixChicagoMatchCandidateConfig;

typedef struct
{
  gint best_distance;
  gint second_distance;
  gint best_index;
  gint second_index;
} GoodixChicagoMatchCandidate;

typedef struct
{
  gint old_index;
  gint new_index;
} GoodixChicagoMatchPair;

typedef struct
{
  gint32 x;
  gint32 y;
} GoodixChicagoMatchPoint;

typedef struct
{
  gint32 transform[6];
  gint32 error;
  guint  inlier_count;
  guint8 inliers[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT];
} GoodixChicagoMatchGeometry;

/* Mode-0 type-24 candidate policy (22 values). */
typedef struct
{
  gint32 values[22];
} GoodixChicagoMatchScoreConfig;

/* Accumulator of the type-24 branch of the metric. */
typedef struct
{
  gint32 eligible_count;
  gint32 matched_count;
  gint32 matched_percent;
  gint32 geometry_percent;
  gint32 mean_distance;
  gint32 special_count;
  gint32 reserved[5];
} GoodixChicagoMatchFeatureOverlap;

/* Final score evidence consumed by the ordinary type-24 aggregation path
 * and its fallback. */
typedef struct
{
  gint32 accepted_count;
  gint32 accepted_geometry_q8_sum;
  gint32 fallback_score;
  gint32 fallback_evaluated_count;
  gint32 fallback_max_metric;
  gint32 fallback_best_metric;
  gint32 fallback_best_geometry;
  gint32 fallback_best_coverage;
  gint32 fallback_geometry_sum;
} GoodixChicagoMatchAggregation;

/* Fields consumed from the result record by the ordinary
 * type-24 scheduler evidence predicate. */
typedef struct
{
  gint32 geometry_count;
  gint32 secondary_geometry_count;
  gint32 reserved08[2];
  gint32 selector;
  gint32 agreement;
  gint32 study_metric_18;
  gint32 study_metric_1c;
  gint32 study_metric_20;
  gint32 normalized_coverage;
  gint32 matched_percent;
  gint32 geometry_percent;
  gint32 penalty_flag_a;
  gint32 penalty_flag_b;
  gint32 reserved38[7];
  gint32 probe_quality;
  gint32 probe_coverage;
  gint32 gallery_quality;
  gint32 gallery_coverage;
  gint32 auxiliary_score;
} GoodixChicagoMatchScoreRecord;

/* Non-record inputs consumed by the type-24 study admission.
 * probe_auxiliary is the live subtemplate density percentage. */
typedef struct
{
  gint32 auxiliary_count;
  gint32 candidate_metric;
  gint32 probe_quality;
  gint32 probe_auxiliary;
  gint32 aggregate_metric;
} GoodixChicagoMatchStudyAdmissionInput;

typedef struct
{
  gint32 primary;
  gint32 secondary;
} GoodixChicagoMatchResolutionEvidence;

typedef struct
{
  gint32   auxiliary_count;
  gboolean raised;
} GoodixChicagoMatchSchedulerAuxiliary;

typedef struct
{
  gint32 confidence;
  gint32 status;
  gint32 special;
} GoodixChicagoMatchSchedulerEvidence;

/* Identify output retained by the type-24 path for template study.
 * The selected index is the original gallery index whose admitted
 * score strictly exceeded the previous best.  The
 * later learning-admission predicate is included, with
 * the prefix-three-or-greater type-24 auxiliary confusion counts and Q8
 * aggregate. */
typedef struct
{
  gint32   score;
  gint32   selected_index;
  gboolean selected_index_known;
  gint32   study_auxiliary_count;
  gboolean study_auxiliary_count_known;
  gint32   study_candidate_metric;
  gboolean study_eligible;
  gboolean study_eligibility_known;
  guint    study_relation_count;
  GoodixChicagoRelation
           study_relations[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
  /* Identify's increments of the subtemplate counter study_value_a (tag 0xbe):
   * one for every record with a status, one for every admitted record on the
   * best-record path.  They persist only when template study repacks the
   * template (update > 0). */
  guint32 hit_increments[GOODIX_CHICAGO_ENROLLMENT_CAPACITY];
} GoodixChicagoMatchTemplateResult;

G_STATIC_ASSERT (sizeof (GoodixChicagoMatchScoreRecord) == 0x68);
G_STATIC_ASSERT (G_STRUCT_OFFSET (GoodixChicagoMatchScoreRecord,
                                  study_metric_18) == 0x18);
G_STATIC_ASSERT (G_STRUCT_OFFSET (GoodixChicagoMatchScoreRecord,
                                  study_metric_1c) == 0x1c);
G_STATIC_ASSERT (G_STRUCT_OFFSET (GoodixChicagoMatchScoreRecord,
                                  study_metric_20) == 0x20);
G_STATIC_ASSERT (G_STRUCT_OFFSET (GoodixChicagoMatchScoreRecord,
                                  probe_quality) == 0x54);
G_STATIC_ASSERT (G_STRUCT_OFFSET (GoodixChicagoMatchScoreRecord,
                                  gallery_quality) == 0x5c);

/* Identify candidate-distance stage. The caller may
 * invoke this repeatedly for record partitions; existing top-two candidates
 * are retained and improved. @distance_matrix and @direction_matrix use the
 * 180-byte row stride. */
void goodix_chicago_match_update_candidates (const GoodixChicagoFeatureRecord        *old_records,
                                             const GoodixChicagoFeatureRecord        *new_records,
                                             const GoodixChicagoMatchCandidateConfig *config,
                                             GoodixChicagoMatchCandidate             *candidates,
                                             guint8                                  *distance_matrix,
                                             guint8                                  *direction_matrix);

void goodix_chicago_match_init_candidates (GoodixChicagoMatchCandidate *candidates,
                                           guint                        count);

/* Ratio gate, spatial uniqueness filter, and
 * bounded best-candidate selection used after the candidate-distance stage. */
guint goodix_chicago_match_select_candidates (const GoodixChicagoFeatureRecord  *new_records,
                                              const GoodixChicagoMatchCandidate *candidates,
                                              guint                              candidate_count,
                                              guint                              limit,
                                              guint                              best_multiplier,
                                              guint                              second_multiplier,
                                              GoodixChicagoMatchPair            *pairs);

/* Type-24 fallback correspondence path. It
 * builds the gallery-to-probe distance matrix, finds the best two gallery
 * rows for each probe column within matching foreground partitions, applies
 * the selector, and returns gallery:probe pairs. */
guint goodix_chicago_match_fallback_correspondences (const GoodixChicagoFeatureRecord *gallery_records,
                                                     guint                             gallery_count,
                                                     guint                             gallery_split,
                                                     const GoodixChicagoFeatureRecord *probe_records,
                                                     guint                             probe_count,
                                                     guint                             probe_split,
                                                     GoodixChicagoMatchPair            pairs[GOODIX_CHICAGO_MATCH_PAIR_LIMIT]);

/* Disconnected-component fallback mode 2.  This uses
 * ordinary correspondences, the direct non-strict geometry
 * result, config-1 metrics, and the capacity-specific acceptance forest. */
void goodix_chicago_match_build_capacity_relation (const GoodixChicagoSubtemplateView *gallery,
                                                   const GoodixChicagoSubtemplateView *probe,
                                                   GoodixChicagoRelation              *relation);

/* Compose the no-adaptive type-24 fields consumed by the ordinary scheduler. */
void goodix_chicago_match_score_subtemplate_type24 (const GoodixChicagoSubtemplateView *gallery,
                                                    const GoodixChicagoSubtemplateView *probe,
                                                    GoodixChicagoMatchScoreRecord      *record,
                                                    GoodixChicagoMatchGeometry         *geometry);

/* Study aggregate (Q8).  The metric
 * helper produces four binary confusion classes; the caller retains the two
 * equal classes and combines the two unequal classes into @count_mixed. */
gint32 goodix_chicago_match_study_aggregate_q8_type24 (gint32 count_zero,
                                                       gint32 count_mixed,
                                                       gint32 count_one);

/* Type-24 study admission filter.  It only clears the two in/out flags. */
void goodix_chicago_match_filter_study_admission_type24 (const GoodixChicagoMatchScoreRecord         *record,
                                                         const GoodixChicagoMatchStudyAdmissionInput *input,
                                                         gint32                                      *status,
                                                         gint32                                      *study_eligible);

void goodix_chicago_match_decode_gallery_resolution_evidence (guint32                               packed_resolution,
                                                              GoodixChicagoMatchResolutionEvidence *evidence);

/* Transform-area normalization used at the start of the type-24 late
 * rejection predicate: the Q8 affine inverse, and the count of integer
 * source pixels retained by a transform.  The policy
 * uses the larger forward/inverse count. */
void goodix_chicago_match_invert_transform_q8_type24 (const gint32 transform[6],
                                                      gint32       inverse[6]);
gint32 goodix_chicago_match_transform_overlap_area_type24 (guint        width,
                                                           guint        height,
                                                           const gint32 transform[6]);

/* Type-24 branch of the late candidate rejection policy.  The return value
 * is the rejection bit; a true
 * result also increments @rejection_count once. @status_flag may be cleared
 * independently by the final weak-geometry policy. */
gboolean goodix_chicago_match_late_rejection_type24 (guint                                width,
                                                     guint                                height,
                                                     gint32                               probe_quality,
                                                     const GoodixChicagoMatchScoreRecord *record,
                                                     const gint32                         transform[6],
                                                     gint32                               current_auxiliary,
                                                     gint32                               candidate_auxiliary,
                                                     gint32                               combined_auxiliary,
                                                     gint32                              *rejection_count,
                                                     gint32                              *status_flag);

/* Type-24 specialization of the candidate prefilter.  A true result skips the ordinary candidate before
 * scheduler evidence, late rejection, study filtering, and aggregation. */
gboolean goodix_chicago_match_candidate_prefilter_type24 (const GoodixChicagoMatchScoreRecord *record,
                                                          gint32                               auxiliary_count,
                                                          gint32                               candidate_metric);

/* Type-24 identify scheduler recurrence.
 * The running count starts with the canonical decode of the live
 * probe's packed resolution.  Each gallery contributes its own canonical
 * decode to @combined_count (capped at five), while only strong gallery
 * classes update the running value consumed later.  The final
 * argument is the counter incremented by preceding type-24 late rejections. */
void goodix_chicago_match_scheduler_auxiliary_init_type24 (guint32                               probe_packed_resolution,
                                                           GoodixChicagoMatchSchedulerAuxiliary *state);
gint32 goodix_chicago_match_scheduler_auxiliary_consume_type24 (GoodixChicagoMatchSchedulerAuxiliary *state,
                                                                guint32                               gallery_packed_resolution,
                                                                gint32                                prior_rejection_count);

/* Offline type-24 identify result over an unpacked gallery and one probe.
 * This keeps the identify -> template study selected-index
 * boundary in addition to the public score. */
void goodix_chicago_match_template_type24 (const GoodixChicagoEnrollment      *gallery_template,
                                           const GoodixChicagoSubtemplateView *probe,
                                           gint32                              selector_threshold,
                                           GoodixChicagoMatchTemplateResult   *result);

/* Orientation-aware affine consensus. Source
 * points are probe records and target points are gallery records. */
void goodix_chicago_match_estimate_geometry (const GoodixChicagoMatchPoint *source_points,
                                             const GoodixChicagoMatchPoint *target_points,
                                             const gint32                  *target_orientations,
                                             const gint32                  *source_orientations,
                                             guint                          point_count,
                                             guint                          minimum_inliers,
                                             gboolean                       strict,
                                             GoodixChicagoMatchGeometry    *result);

/* Orientation consistency filter. @inliers is the
 * geometry mask on input and is cleared in place for rejected pairs. */
guint goodix_chicago_match_filter_orientations (const gint32  transform[6],
                                                const gint32 *target_orientations,
                                                const gint32 *source_orientations,
                                                guint         point_count,
                                                guint8        inliers[GOODIX_CHICAGO_MATCH_GEOMETRY_LIMIT]);

/* Fixed-point least-squares refit. The transform
 * is replaced only when residual error improves without losing inliers. */
gboolean goodix_chicago_match_refine_geometry (const GoodixChicagoMatchPoint *source_points,
                                               const GoodixChicagoMatchPoint *target_points,
                                               const guint8                  *inliers,
                                               guint                          point_count,
                                               gint32                         error_limit,
                                               gint32                         transform[6]);

/* Compose the geometry stages in order after pair materialization. */
guint goodix_chicago_match_geometry_consensus (const GoodixChicagoMatchPoint *source_points,
                                               const GoodixChicagoMatchPoint *target_points,
                                               const gint32                  *target_orientations,
                                               const gint32                  *source_orientations,
                                               guint                          point_count,
                                               guint                          minimum_inliers,
                                               GoodixChicagoMatchGeometry    *result);

/* Materialize record-index pairs and run the complete geometry
 * chain. Invalid/sentinel pairs are ignored. */
guint goodix_chicago_match_geometry_consensus_records (const GoodixChicagoFeatureRecord *gallery_records,
                                                       guint                             gallery_count,
                                                       const GoodixChicagoFeatureRecord *probe_records,
                                                       guint                             probe_count,
                                                       const GoodixChicagoMatchPair     *pairs,
                                                       guint                             pair_count,
                                                       guint                             minimum_inliers,
                                                       GoodixChicagoMatchGeometry       *result);

/* Type-24 branch. Coordinates and transform use
 * Q8 fixed point. The scorer passes its geometric-inlier count
 * separately, so geometry_percent can legitimately exceed 100. */
void goodix_chicago_match_feature_overlap_type24 (const GoodixChicagoFeatureRecord *gallery_records,
                                                  guint                             gallery_count,
                                                  const GoodixChicagoFeatureRecord *probe_records,
                                                  guint                             probe_count,
                                                  guint                             gallery_width,
                                                  guint                             gallery_height,
                                                  const gint32                      transform[6],
                                                  gint32                            geometry_count,
                                                  GoodixChicagoMatchFeatureOverlap *result);

/* Ordinary type-24 admission gate. The helper
 * may force admission with status 1; otherwise both metric thresholds are
 * strict. */
gboolean goodix_chicago_match_accept_type24 (gint32 selector,
                                             gint32 agreement,
                                             gint32 helper_status,
                                             gint32 selector_threshold);

/* Ordinary type-24 profile. */
void goodix_chicago_match_scheduler_evidence_type24 (const GoodixChicagoMatchScoreRecord *record,
                                                     gint32                               quality_a,
                                                     gint32                               quality_b,
                                                     gboolean                             penalize_low_coverage,
                                                     GoodixChicagoMatchSchedulerEvidence *evidence);

G_END_DECLS
