// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: see goodix-chicago-enrollment.c. */

/* Native state for the Chicago enrollment merge boundary. */

#pragma once

#include <glib.h>

#include "goodix-chicago-feature.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_ENROLLMENT_REQUIRED_SAMPLES 8u
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
  /* Live-only outputs. Packed gallery subtemplates do
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

/* 28-byte relation entry of the triangular relation table. */
typedef struct
{
  gint32 inlier_count;
  gint32 transform[6];
} GoodixChicagoRelation;

typedef void (*GoodixChicagoCapacityRelationBuilder) (const GoodixChicagoSubtemplateView *gallery,
                                                      const GoodixChicagoSubtemplateView *probe,
                                                      GoodixChicagoRelation              *relation);

/* Correspondence stage. Record bytes
 * 0x10..0x27 are compared, filtered by the strict 0.95 best/second
 * ratio, and spatially deduplicated in list order. */
guint goodix_chicago_enrollment_find_correspondences (const GoodixChicagoFeatureRecord *old_records,
                                                      guint                             old_count,
                                                      const GoodixChicagoFeatureRecord *new_records,
                                                      guint                             new_count,
                                                      GoodixChicagoCorrespondence       pairs[GOODIX_CHICAGO_CORRESPONDENCE_LIMIT]);

/* Three-point affine search. @source_points are
 * transformed into @target_points. Matrix coefficients are Q8; translations
 * and point coordinates use the feature records' Q8 coordinate space. */
void goodix_chicago_enrollment_estimate_transform (const GoodixChicagoPoint     *source_points,
                                                   const GoodixChicagoPoint     *target_points,
                                                   guint                         point_count,
                                                   GoodixChicagoTransformResult *result);

/* Final evidence predicate after the transform and metric stages. */
gboolean goodix_chicago_enrollment_evidence_is_accepted (guint inlier_count,
                                                         gint  metric_a,
                                                         gint  metric_b);

/* Compose correspondence, transform, metric, and evidence boundaries into
 * a relation entry. */
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

/* Selector, agreement, and coverage outputs of the metric with
 * config {1,0,0}. Identify uses {1,0,1}; with no adaptive-state argument its
 * returned selector and these two exported metrics are the same path. */
gint goodix_chicago_enrollment_calculate_config1_metrics (const GoodixChicagoMetricData *new_metric_data,
                                                          const GoodixChicagoMetricData *old_metric_data,
                                                          const gint32                   transform[6],
                                                          gint                          *metric_a,
                                                          gint                          *metric_b);

/* Type-24 special-state count path.  Unlike
 * types 7 and 23, leaves the 40x32 live map zero-filled and only
 * overwrites its first six bytes with the live auxiliary record. */
void goodix_chicago_enrollment_calculate_live_auxiliary_counts_type24 (const guint8 auxiliary[6],
                                                                       const gint32 transform[6],
                                                                       gint        *count_zero,
                                                                       gint        *count_mixed,
                                                                       gint        *count_one);

/* Class-purity outputs (study metrics) of a score record. */
gboolean goodix_chicago_enrollment_calculate_study_metrics (const GoodixChicagoMetricData *new_metric_data,
                                                            const GoodixChicagoMetricData *old_metric_data,
                                                            const gint32                   transform[6],
                                                            gint                          *metric_18,
                                                            gint                          *metric_1c,
                                                            gint                          *metric_20);

/* Primary bit plane, coarse mask, and packed position map of a
 * subtemplate. */
void goodix_chicago_enrollment_build_metric_data (const guint8             enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  GoodixChicagoMetricData *metric_data);

GoodixChicagoEnrollment *goodix_chicago_enrollment_new (void);
void goodix_chicago_enrollment_free (GoodixChicagoEnrollment *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoEnrollment,
                               goodix_chicago_enrollment_free)

/* First-sample path. The supplied
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

/* Under-capacity template study append.  The
 * relation array is the per-gallery state retained by identify and maps the
 * live probe into each existing subtemplate. */
void goodix_chicago_enrollment_apply_study_hits (GoodixChicagoEnrollment *self,
                                                 const guint32           *hits,
                                                 guint                    count);

gboolean goodix_chicago_enrollment_append_study (GoodixChicagoEnrollment             *self,
                                                 const GoodixChicagoSubtemplateView  *probe,
                                                 guint                                selected_index,
                                                 const GoodixChicagoRelation         *relations,
                                                 guint                                relation_count,
                                                 guint                               *appended_index,
                                                 GoodixChicagoCapacityRelationBuilder build_unresolved_relation,
                                                 GError                             **error);

/* Mutation half of the capacity-full path.  The
 * caller supplies the selected source and the replacement slot chosen by the
 * capacity selector; relations map the live probe into every current
 * gallery subtemplate. */
gboolean goodix_chicago_enrollment_replace_study (GoodixChicagoEnrollment            *self,
                                                  const GoodixChicagoSubtemplateView *probe,
                                                  guint                               selected_index,
                                                  guint                               replacement_index,
                                                  const GoodixChicagoRelation        *relations,
                                                  guint                               relation_count,
                                                  GError                            **error);

/* Redundancy scores after the capacity relation
 * helpers have produced @relations.  @probe_score is the replacement
 * threshold and @gallery_scores contains one score for every existing slot.
 * The companion quality vector is returned as well. */
gboolean goodix_chicago_enrollment_calculate_capacity_scores (const GoodixChicagoEnrollment      *self,
                                                              const GoodixChicagoSubtemplateView *probe,
                                                              const GoodixChicagoRelation        *relations,
                                                              guint                               relation_count,
                                                              gint32                             *probe_score,
                                                              gint32                              gallery_scores[GOODIX_CHICAGO_ENROLLMENT_CAPACITY],
                                                              gint32                              gallery_qualities[GOODIX_CHICAGO_ENROLLMENT_CAPACITY]);

/* Relation-closure traversal.  The caller supplies
 * the probe relations after fallback rematching, or NULL for the gallery-only
 * reconstruction performed after unpack.  Missing gallery relations are
 * filled in place with the synthetic marker (2), while real matcher
 * relations and their strengths remain untouched. */
gboolean goodix_chicago_enrollment_synthesize_capacity_relations (GoodixChicagoEnrollment *self,
                                                                  GoodixChicagoRelation   *probe_relations,
                                                                  guint                    relation_count);

/* Full capacity selector.  This composes relation closure, fallback
 * rematching and redundancy scores and applies the replacement predicates.  @build_unresolved_relation supplies
 * feature rematching for disconnected gallery components.
 * @replacement_index is G_MAXUINT when the probe should not update a full
 * gallery. */
gboolean goodix_chicago_enrollment_select_capacity_replacement (GoodixChicagoEnrollment             *self,
                                                                const GoodixChicagoSubtemplateView  *probe,
                                                                guint                                selected_index,
                                                                GoodixChicagoRelation               *probe_relations,
                                                                guint                                relation_count,
                                                                GoodixChicagoCapacityRelationBuilder build_unresolved_relation,
                                                                guint                               *replacement_index);

/* The enrolment protocol temporarily removes the first directional-tip image, then
 * re-adds it after the following image. Drop the most recently inserted
 * subtemplate so that the later insert follows the same Chicago lifecycle. */
gboolean goodix_chicago_enrollment_drop_last (GoodixChicagoEnrollment *self,
                                              GError                 **error);

guint goodix_chicago_enrollment_get_count (const GoodixChicagoEnrollment *self);
gboolean goodix_chicago_enrollment_get_match_order_index (const GoodixChicagoEnrollment *self,
                                                          guint                          schedule_index,
                                                          guint                         *gallery_index);
guint goodix_chicago_enrollment_get_capacity (const GoodixChicagoEnrollment *self);
gsize goodix_chicago_enrollment_get_packed_size (const GoodixChicagoEnrollment *self);
GBytes *goodix_chicago_enrollment_pack (const GoodixChicagoEnrollment *self,
                                        GError                       **error);
GoodixChicagoEnrollment *goodix_chicago_enrollment_unpack (const guint8 *data,
                                                           gsize         data_len,
                                                           GError      **error);
gboolean goodix_chicago_enrollment_get_subtemplate (const GoodixChicagoEnrollment *self,
                                                    guint                          index,
                                                    GoodixChicagoSubtemplateView  *view);

G_END_DECLS
