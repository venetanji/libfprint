// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

#include "goodix-chicago-runtime.h"

#include <string.h>

#include "goodix-chicago-feature.h"
#include "goodix-chicago-match.h"
#include "goodix-chicago-template.h"

struct _GoodixChicagoRuntimeProbe
{
  GoodixChicagoEnrollment     *enrollment;
  GoodixChicagoSubtemplateView view;
};

GoodixChicagoRuntimeProbe *
goodix_chicago_runtime_prepare_probe (
  GoodixChicagoPreprocessor  *preprocessor,
  const guint16               raw[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoRuntimeReject *reject,
  GError                    **error)
{
  guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS];
  guint8 resolution_labels[GOODIX_CHICAGO_PIXELS];
  GoodixChicagoFeatureRecord records[GOODIX_CHICAGO_FEATURE_RECORD_LIMIT];
  GoodixChicagoFeatureConsensus consensus;
  GoodixChicagoFeatureLiveAuxiliary live_auxiliary;
  GoodixChicagoMetricData metric_data;
  GoodixChicagoEnrollmentResult result;

  g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
  guint active_count = 0;
  guint record_count;
  guint8 quality;
  guint8 coverage;
  GoodixChicagoPreprocessStatus preprocess_status;
  guint resolution_code;
  guint resolution_peak_state;
  gboolean resolution_auxiliary;

  g_return_val_if_fail (preprocessor != NULL, NULL);
  g_return_val_if_fail (raw != NULL, NULL);
  if (reject)
    *reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;

  preprocess_status = goodix_chicago_preprocessor_build_enhanced_checked (
    preprocessor, raw, enhanced);
  goodix_chicago_preprocessor_finalize_metrics (
    goodix_chicago_preprocessor_compute_base_quality_from_enhanced (enhanced),
    goodix_chicago_preprocessor_compute_coverage (enhanced),
    &quality, &coverage);
  if (preprocess_status != GOODIX_CHICAGO_PREPROCESS_STATUS_OK)
    {
      if (reject)
        *reject = preprocess_status == GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT ?
                  GOODIX_CHICAGO_RUNTIME_REJECT_BAD_INPUT :
                  GOODIX_CHICAGO_RUNTIME_REJECT_POOR_CAPTURE;
      return NULL;
    }

  memset (records, 0, sizeof (records));
  record_count = goodix_chicago_feature_extract_subtemplate_full (
    enhanced, records, G_N_ELEMENTS (records), &active_count, &consensus);
  if (record_count == 0)
    {
      if (reject)
        *reject = GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES;
      return NULL;
    }

  goodix_chicago_enrollment_build_metric_data (enhanced, &metric_data);
  goodix_chicago_preprocessor_build_resolution_map_full (
    preprocessor, raw, resolution_labels, &resolution_peak_state);
  goodix_chicago_preprocessor_classify_resolution_labels (
    0x18, resolution_labels, GOODIX_CHICAGO_PIXELS,
    GOODIX_CHICAGO_PIXELS, &resolution_code, &resolution_auxiliary);
  metric_data.packed_resolution =
    goodix_chicago_preprocessor_pack_resolution_code (resolution_code);
  goodix_chicago_feature_build_live_auxiliary (
    resolution_peak_state, metric_data.packed_resolution, &live_auxiliary);
  probe = g_new0 (GoodixChicagoRuntimeProbe, 1);
  probe->enrollment = goodix_chicago_enrollment_new ();
  if (!goodix_chicago_enrollment_insert_first (
        probe->enrollment, records, record_count, active_count,
        quality, coverage, &metric_data, &result, error))
    return NULL;
  if (!goodix_chicago_enrollment_get_subtemplate (
        probe->enrollment, 0, &probe->view))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: could not expose Chicago runtime probe");
      return NULL;
    }
  probe->view.density_positive_percent = consensus.positive_percent;
  probe->view.density_class = consensus.density_class;
  probe->view.density_inactive_count = consensus.inactive_count;
  probe->view.live_auxiliary = live_auxiliary;

  return g_steal_pointer (&probe);
}

void
goodix_chicago_runtime_probe_free (GoodixChicagoRuntimeProbe *probe)
{
  if (!probe)
    return;
  goodix_chicago_enrollment_free (probe->enrollment);
  g_free (probe);
}

const GoodixChicagoSubtemplateView *
goodix_chicago_runtime_probe_get_view (
  const GoodixChicagoRuntimeProbe *probe)
{
  g_return_val_if_fail (probe != NULL, NULL);
  return &probe->view;
}

GBytes *
goodix_chicago_runtime_probe_pack (
  const GoodixChicagoRuntimeProbe *probe,
  GError                         **error)
{
  g_return_val_if_fail (probe != NULL, NULL);
  return goodix_chicago_enrollment_pack (probe->enrollment, error);
}

gboolean
goodix_chicago_runtime_match_print_data (
  const GoodixChicagoRuntimeProbe  *probe,
  GVariant                         *print_data,
  const guint8                      sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes                           *calibration,
  GoodixChicagoMatchTemplateResult *result,
  GError                          **error)
{
  g_autoptr(GBytes) packed = NULL;
  g_autoptr(GoodixChicagoEnrollment) gallery = NULL;
  const guint8 *packed_data;
  gsize packed_size;

  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  if (!goodix_chicago_print_data_parse (
        print_data, sensor_id, calibration, &packed, error))
    return FALSE;
  packed_data = g_bytes_get_data (packed, &packed_size);
  gallery = goodix_chicago_enrollment_unpack (
    packed_data, packed_size, error);
  if (!gallery)
    return FALSE;

  goodix_chicago_match_template_type24 (
    gallery, &probe->view, GOODIX_CHICAGO_RUNTIME_SELECTOR_THRESHOLD,
    result);
  return TRUE;
}

gint32
goodix_chicago_runtime_score_print_data (
  const GoodixChicagoRuntimeProbe *probe,
  GVariant                        *print_data,
  const guint8                     sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes                          *calibration,
  GError                         **error)
{
  GoodixChicagoMatchTemplateResult result;

  if (!goodix_chicago_runtime_match_print_data (
        probe, print_data, sensor_id, calibration, &result, error))
    return G_MININT32;
  return result.score;
}

gboolean
goodix_chicago_runtime_study_print_data (
  const GoodixChicagoRuntimeProbe        *probe,
  GVariant                               *print_data,
  const guint8                            sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes                                 *calibration,
  const GoodixChicagoMatchTemplateResult *result,
  GVariant                              **updated_print_data,
  GError                                **error)
{
  g_autoptr(GBytes) packed = NULL;
  g_autoptr(GBytes) updated_packed = NULL;
  g_autoptr(GoodixChicagoEnrollment) gallery = NULL;
  g_autofree GoodixChicagoRelation *capacity_relations = NULL;
  const guint8 *packed_data;
  gsize packed_size;
  guint count;
  guint replacement_index = G_MAXUINT;

  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (updated_print_data != NULL, FALSE);
  *updated_print_data = NULL;

  if (result->score <= 0 ||
      !result->selected_index_known || result->selected_index < 0 ||
      !result->study_eligibility_known || !result->study_eligible)
    return TRUE;

  if (!goodix_chicago_print_data_parse (
        print_data, sensor_id, calibration, &packed, error))
    return FALSE;
  packed_data = g_bytes_get_data (packed, &packed_size);
  gallery = goodix_chicago_enrollment_unpack (
    packed_data, packed_size, error);
  if (!gallery)
    return FALSE;

  count = goodix_chicago_enrollment_get_count (gallery);
  if ((guint) result->selected_index >= count ||
      result->study_relation_count != count)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: invalid templateStudy result index=%d relations=%u gallery=%u",
                   result->selected_index, result->study_relation_count, count);
      return FALSE;
    }

  if (count < goodix_chicago_enrollment_get_capacity (gallery))
    {
      if (!goodix_chicago_enrollment_append_study (
            gallery, &probe->view, (guint) result->selected_index,
            result->study_relations, result->study_relation_count,
            NULL, error))
        return FALSE;
    }
  else
    {
      capacity_relations = g_memdup2 (
        result->study_relations,
        result->study_relation_count * sizeof (*capacity_relations));
      if (!goodix_chicago_enrollment_select_capacity_replacement (
            gallery, &probe->view, (guint) result->selected_index,
            capacity_relations, result->study_relation_count,
            goodix_chicago_match_build_capacity_relation,
            &replacement_index))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "gdix51c0: capacity selector failed");
          return FALSE;
        }
      if (replacement_index == G_MAXUINT)
        return TRUE;
      if (!goodix_chicago_enrollment_replace_study (
            gallery, &probe->view, (guint) result->selected_index,
            replacement_index, capacity_relations,
            result->study_relation_count, error))
        return FALSE;
    }
  updated_packed = goodix_chicago_enrollment_pack (gallery, error);
  if (!updated_packed)
    return FALSE;

  *updated_print_data = goodix_chicago_print_data_build (
    sensor_id, calibration, updated_packed);
  return *updated_print_data != NULL;
}
