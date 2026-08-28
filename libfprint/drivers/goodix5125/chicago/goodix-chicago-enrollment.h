// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Native state for the recovered Chicago enrollment merge boundary. */

#pragma once

#include <glib.h>

#include "goodix-chicago-feature.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_ENROLLMENT_REQUIRED_SAMPLES 8u
#define GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES 12u
#define GOODIX_CHICAGO_ENROLLMENT_CAPACITY 50u
#define GOODIX_CHICAGO_SUBTEMPLATE_RECORD_LIMIT 120u
#define GOODIX_CHICAGO_FIRST_POSITION_DETAIL 0x64000064u
#define GOODIX_CHICAGO_CORRESPONDENCE_LIMIT 31u
#define GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT 42u
#define GOODIX_CHICAGO_METRIC_MAP_WIDTH 40u
#define GOODIX_CHICAGO_METRIC_MAP_HEIGHT 32u
#define GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES 160u
#define GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES 40u

typedef struct _GoodixChicagoEnrollment GoodixChicagoEnrollment;

typedef struct
{
  guint32 packed_position_detail;
  guint   progress;
  guint   position_x;
  guint   position_y;
} GoodixChicagoEnrollmentResult;

/* EngineAdapter's enrollment-tip state for sensor type 12.  Chicago retains
 * every successfully added subtemplate, while @used counts only samples that
 * advance the WBF enrollment progress. */
typedef struct
{
  guint    touched;
  guint    enrolled;
  guint    tipped;
  guint    used;
  guint    last_tipped;
  guint    continuous_tips;
  guint    tip_index;
  guint    previous_tip_index;
  gboolean deferred_pending;
  gboolean defer_current_sample;
  gboolean restore_deferred_sample;
} GoodixChicagoEngineEnrollmentPolicy;

void goodix_chicago_engine_enrollment_policy_init (GoodixChicagoEngineEnrollmentPolicy *policy);
gboolean goodix_chicago_engine_enrollment_policy_accept (GoodixChicagoEngineEnrollmentPolicy *policy,
                                                         guint                                position_x,
                                                         guint                                position_y,
                                                         guint                               *reject_detail);
gboolean goodix_chicago_engine_enrollment_policy_complete (const GoodixChicagoEngineEnrollmentPolicy *policy);

typedef struct
{
  guint                                  record_count;
  guint                                  active_count;
  guint                                  quality;
  guint                                  coverage;
  const GoodixChicagoFeatureRecord      *records;
  gboolean                               has_metric_data;
  const struct _GoodixChicagoMetricData *metric_data;
  guint                                  group_state;
  guint                                  relation_base;
  guint                                  study_state;
  guint                                  study_flags;
  guint                                  lineage_index;
  guint                                  replacement_count;
  guint                                  study_value_a;
  guint                                  study_value_b;
  /* Live-only AlgoChicago+0x12400 outputs. Packed gallery subtemplates do
   * not retain these fields. */
  guint                             density_positive_percent;
  guint                             density_class;
  guint                             density_inactive_count;
  GoodixChicagoFeatureLiveAuxiliary live_auxiliary;
} GoodixChicagoSubtemplateView;

typedef struct _GoodixChicagoMetricData
{
  guint8 primary[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES];
  guint8 secondary[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES];
  guint8 validity[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES];
  guint8 coarse_mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES];
  guint8 position_map[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES];
  guint  packed_resolution;
} GoodixChicagoMetricData;

typedef struct
{
  guint old_index;
  guint new_index;
} GoodixChicagoCorrespondence;

typedef struct
{
  gint32 x;
  gint32 y;
} GoodixChicagoPoint;

typedef struct
{
  gint32 values[6];
  gint32 error;
  guint  inlier_count;
  guint8 inliers[GOODIX_CHICAGO_TRANSFORM_POINT_LIMIT];
} GoodixChicagoTransformResult;

/* Exact 28-byte relation entry stored at enrollment+0x1c0. */
typedef struct
{
  gint32 inlier_count;
  gint32 transform[6];
} GoodixChicagoRelation;

typedef void (*GoodixChicagoCapacityRelationBuilder) (const GoodixChicagoSubtemplateView *gallery,
                                                      const GoodixChicagoSubtemplateView *probe,
                                                      GoodixChicagoRelation              *relation);

/* Exact AlgoChicago+0x5aea0/+0x5a7b0 correspondence stage. Record bytes
 * 0x10..0x27 are compared, filtered by the official strict 0.95 best/second
 * ratio, and spatially deduplicated in official list order. */
guint goodix_chicago_enrollment_find_correspondences (const GoodixChicagoFeatureRecord *old_records,
                                                      guint                             old_count,
                                                      const GoodixChicagoFeatureRecord *new_records,
                                                      guint                             new_count,
                                                      GoodixChicagoCorrespondence       pairs[GOODIX_CHICAGO_CORRESPONDENCE_LIMIT]);

/* Exact AlgoChicago+0x59e80 three-point affine search. @source_points are
 * transformed into @target_points. Matrix coefficients are Q8; translations
 * and point coordinates use the feature records' Q8 coordinate space. */
void goodix_chicago_enrollment_estimate_transform (const GoodixChicagoPoint     *source_points,
                                                   const GoodixChicagoPoint     *target_points,
                                                   guint                         point_count,
                                                   GoodixChicagoTransformResult *result);

/* Exact +0x190d0 final evidence predicate after +0x59e80 and +0x510e0. */
gboolean goodix_chicago_enrollment_evidence_is_accepted (guint inlier_count,
                                                         gint  metric_a,
                                                         gint  metric_b);

/* Compose correspondence, transform, metric, and evidence boundaries into
 * the exact relation entry consumed by +0x19f30. */
void goodix_chicago_enrollment_build_relation (const GoodixChicagoFeatureRecord *old_records,
                                               guint                             old_count,
                                               const GoodixChicagoMetricData    *old_metric_data,
                                               const GoodixChicagoFeatureRecord *new_records,
                                               guint                             new_count,
                                               const GoodixChicagoMetricData    *new_metric_data,
                                               GoodixChicagoRelation            *relation,
                                               gint                             *metric_a,
                                               gint                             *metric_b,
                                               gboolean                         *evidence_accepted);

/* Production-mode specialization of AlgoChicago+0x510e0. The packed primary
 * maps are 40 x 32 LSB-first bit planes. The packed masks are 16 x 20 bit
 * planes expanded by the DLL to the subtemplate's 64 x 80 coordinates. */
void goodix_chicago_enrollment_calculate_relation_metrics (const guint8 new_primary[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
                                                           const guint8 new_mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES],
                                                           const guint8 old_primary[GOODIX_CHICAGO_METRIC_MAP_PACKED_BYTES],
                                                           const guint8 old_mask[GOODIX_CHICAGO_METRIC_MASK_PACKED_BYTES],
                                                           const gint32 transform[6],
                                                           gint        *metric_a,
                                                           gint        *metric_b);

/* Exact selector, agreement, and coverage outputs of AlgoChicago+0x510e0
 * config {1,0,0}. Identify uses {1,0,1}; with no adaptive-state argument its
 * returned selector and these two exported metrics are the same path. */
gint goodix_chicago_enrollment_calculate_config1_metrics (const GoodixChicagoMetricData *new_metric_data,
                                                          const GoodixChicagoMetricData *old_metric_data,
                                                          const gint32                   transform[6],
                                                          gint                          *metric_a,
                                                          gint                          *metric_b);

/* Exact type-24 special-state count path through +0x528c0/+0x546b0.  Unlike
 * types 7 and 23, +0x137a0 leaves the 40x32 live map zero-filled and only
 * overwrites its first six bytes with the live auxiliary record. */
void goodix_chicago_enrollment_calculate_live_auxiliary_counts_type24 (const guint8 auxiliary[6],
                                                                       const gint32 transform[6],
                                                                       gint        *count_zero,
                                                                       gint        *count_mixed,
                                                                       gint        *count_one);

/* Exact AlgoChicago+0x51be0 class-purity outputs written to score-record
 * offsets +0x18, +0x1c, and +0x20. */
gboolean goodix_chicago_enrollment_calculate_study_metrics (const GoodixChicagoMetricData *new_metric_data,
                                                            const GoodixChicagoMetricData *old_metric_data,
                                                            const gint32                   transform[6],
                                                            gint                          *metric_18,
                                                            gint                          *metric_1c,
                                                            gint                          *metric_20);

/* Exact production +0x54ec0 primary bit plane, +0x523c0 coarse mask, and
* +0x519d0/+0x53590 packed position map retained at subtemplate+0x130. */
void goodix_chicago_enrollment_build_metric_data (const guint8             enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  GoodixChicagoMetricData *metric_data);

GoodixChicagoEnrollment *goodix_chicago_enrollment_new (void);
void goodix_chicago_enrollment_free (GoodixChicagoEnrollment *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoEnrollment,
                               goodix_chicago_enrollment_free)

/* Exact first-sample +0x19950 -> +0x196a0 -> +0x19c00 path. The supplied
 * records must already be in prepare_subtemplate() representation. */
gboolean goodix_chicago_enrollment_insert_first (GoodixChicagoEnrollment          *self,
                                                 const GoodixChicagoFeatureRecord *records,
                                                 guint                             record_count,
                                                 guint                             active_count,
                                                 guint                             quality,
                                                 guint                             coverage,
                                                 const GoodixChicagoMetricData    *metric_data,
                                                 GoodixChicagoEnrollmentResult    *result,
                                                 GError                          **error);

/* Exact two-sample specialization of the second +0x19950 insertion. */
gboolean goodix_chicago_enrollment_insert_second (GoodixChicagoEnrollment          *self,
                                                  const GoodixChicagoFeatureRecord *records,
                                                  guint                             record_count,
                                                  guint                             active_count,
                                                  guint                             quality,
                                                  guint                             coverage,
                                                  const GoodixChicagoMetricData    *metric_data,
                                                  GoodixChicagoEnrollmentResult    *result,
                                                  GError                          **error);

/* Append any sample after the first and build its triangular block of
 * relations against every existing subtemplate. */
gboolean goodix_chicago_enrollment_insert_next (GoodixChicagoEnrollment          *self,
                                                const GoodixChicagoFeatureRecord *records,
                                                guint                             record_count,
                                                guint                             active_count,
                                                guint                             quality,
                                                guint                             coverage,
                                                const GoodixChicagoMetricData    *metric_data,
                                                GoodixChicagoEnrollmentResult    *result,
                                                GError                          **error);

/* Exact under-capacity templateStudy append at AlgoChicago+0x5ea80.  The
 * relation array is the per-gallery state retained by identify and maps the
 * live probe into each existing subtemplate. */
gboolean goodix_chicago_enrollment_append_study (GoodixChicagoEnrollment            *self,
                                                 const GoodixChicagoSubtemplateView *probe,
                                                 guint                               selected_index,
                                                 const GoodixChicagoRelation        *relations,
                                                 guint                               relation_count,
                                                 guint                              *appended_index,
                                                 GError                            **error);

/* Exact mutation half of the capacity-full AlgoChicago+0x5d550 path.  The
 * caller supplies the selected source and the replacement slot chosen by the
 * official capacity selector; relations map the live probe into every current
 * gallery subtemplate. */
gboolean goodix_chicago_enrollment_replace_study (GoodixChicagoEnrollment            *self,
                                                  const GoodixChicagoSubtemplateView *probe,
                                                  guint                               selected_index,
                                                  guint                               replacement_index,
                                                  const GoodixChicagoRelation        *relations,
                                                  guint                               relation_count,
                                                  GError                            **error);

/* Exact AlgoChicago+0x5e440 redundancy scores after the capacity relation
 * helpers have produced @relations.  @probe_score is the replacement
 * threshold and @gallery_scores contains one score for every existing slot.
 * The companion quality vector mirrors the official helper's sixth output. */
gboolean goodix_chicago_enrollment_calculate_capacity_scores (const GoodixChicagoEnrollment      *self,
                                                              const GoodixChicagoSubtemplateView *probe,
                                                              const GoodixChicagoRelation        *relations,
                                                              guint                               relation_count,
                                                              gint32                             *probe_score,
                                                              gint32                              gallery_scores[GOODIX_CHICAGO_ENROLLMENT_CAPACITY],
                                                              gint32                              gallery_qualities[GOODIX_CHICAGO_ENROLLMENT_CAPACITY]);

/* Exact AlgoChicago+0x5c3a0 relation-closure traversal.  The caller supplies
 * the post-+0x5caf0 probe relations, or NULL for the gallery-only
 * reconstruction performed after unpack.  Missing gallery relations are
 * filled in place with the official synthetic marker (2), while real matcher
 * relations and their strengths remain untouched. */
gboolean goodix_chicago_enrollment_synthesize_capacity_relations (GoodixChicagoEnrollment *self,
                                                                  GoodixChicagoRelation   *probe_relations,
                                                                  guint                    relation_count);

/* Exact graph-propagation phase at the start of AlgoChicago+0x5caf0 mode 2.
 * Starting with identify's direct probe relations, it walks the packed
 * gallery graph and synthesizes every probe relation reachable without the
 * helper's later feature-rematching fallback. */
gboolean goodix_chicago_enrollment_propagate_capacity_relations (GoodixChicagoEnrollment *self,
                                                                 GoodixChicagoRelation   *probe_relations,
                                                                 guint                    relation_count,
                                                                 guint                   *unresolved_count);

/* Full capacity selector at +0x5d550.  This composes the
 * recovered +0x5c3a0 -> +0x5caf0 -> +0x5c3a0 -> +0x5e440 chain and applies
 * the official replacement predicates.  @build_unresolved_relation supplies
 * +0x599e0/+0x58b20 feature rematching for disconnected gallery components.
 * @replacement_index is G_MAXUINT when the probe should not update a full
 * gallery. */
gboolean goodix_chicago_enrollment_select_capacity_replacement (GoodixChicagoEnrollment             *self,
                                                                const GoodixChicagoSubtemplateView  *probe,
                                                                guint                                selected_index,
                                                                GoodixChicagoRelation               *probe_relations,
                                                                guint                                relation_count,
                                                                GoodixChicagoCapacityRelationBuilder build_unresolved_relation,
                                                                guint                               *replacement_index);

/* EngineAdapter temporarily removes the first directional-tip image, then
 * re-adds it after the following image. Drop the most recently inserted
 * subtemplate so that the later insert follows the same Chicago lifecycle. */
gboolean goodix_chicago_enrollment_drop_last (GoodixChicagoEnrollment *self,
                                              GError                 **error);

guint goodix_chicago_enrollment_get_count (const GoodixChicagoEnrollment *self);
gboolean goodix_chicago_enrollment_get_match_order_index (const GoodixChicagoEnrollment *self,
                                                          guint                          schedule_index,
                                                          guint                         *gallery_index);
guint goodix_chicago_enrollment_get_capacity (const GoodixChicagoEnrollment *self);
guint goodix_chicago_enrollment_get_required_samples (const GoodixChicagoEnrollment *self);
guint goodix_chicago_enrollment_get_transform_count (const GoodixChicagoEnrollment *self);
gsize goodix_chicago_enrollment_get_packed_size (const GoodixChicagoEnrollment *self);
GBytes *goodix_chicago_enrollment_pack (const GoodixChicagoEnrollment *self,
                                        GError                       **error);
GoodixChicagoEnrollment *goodix_chicago_enrollment_unpack (const guint8 *data,
                                                           gsize         data_len,
                                                           GError      **error);
gboolean goodix_chicago_enrollment_get_relation (const GoodixChicagoEnrollment *self,
                                                 guint                          index,
                                                 GoodixChicagoRelation         *relation);
gboolean goodix_chicago_enrollment_get_subtemplate (const GoodixChicagoEnrollment *self,
                                                    guint                          index,
                                                    GoodixChicagoSubtemplateView  *view);

G_END_DECLS
