// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: see goodix-chicago-runtime.h. */

#include "goodix-chicago-runtime.h"

#include <string.h>

struct _GoodixChicagoRuntimeProbe
{
  GoodixChicagoEnrollment     *enrollment;
  GoodixChicagoSubtemplateView view;
};

gboolean
goodix_chicago_runtime_preprocess (GoodixChicagoPreprocessor *preprocessor,
                                   const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                   gboolean                   enroll,
                                   GoodixChicagoRuntimeFrame *frame)
{
  g_return_val_if_fail (preprocessor != NULL, FALSE);
  g_return_val_if_fail (raw != NULL, FALSE);
  g_return_val_if_fail (frame != NULL, FALSE);

  frame->quality = 0;
  frame->coverage = 0;
  frame->status = goodix_chicago_preprocessor_process_context (
    preprocessor, raw, enroll ? 1 : 0, FALSE, frame->enhanced,
    &frame->quality, &frame->coverage, frame->context);
  return frame->status == GOODIX_CHICAGO_PREPROCESS_STATUS_OK;
}

GoodixChicagoRuntimeProbe *
goodix_chicago_runtime_probe_new (GoodixChicagoFeatureState       *feature_state,
                                  const GoodixChicagoRuntimeFrame *frame,
                                  gboolean                         identify,
                                  GoodixChicagoRuntimeReject      *reject,
                                  GError                         **error)
{
  GoodixChicagoFeatureRecord records[GOODIX_CHICAGO_FEATURE_RECORD_LIMIT];
  GoodixChicagoFeatureConsensus consensus;
  guint8 live_auxiliary[6];
  guint32 packed_resolution = 0;
  GoodixChicagoMetricData metric_data;
  GoodixChicagoEnrollmentResult result;

  g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
  guint active_count = 0;
  guint record_count;

  g_return_val_if_fail (feature_state != NULL, NULL);
  g_return_val_if_fail (frame != NULL, NULL);
  if (reject)
    *reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;

  memset (records, 0, sizeof (records));
  memset (&consensus, 0, sizeof (consensus));
  record_count = goodix_chicago_feature_get_feature (
    feature_state, frame->enhanced, frame->context, frame->quality,
    frame->coverage, identify, records, G_N_ELEMENTS (records),
    &active_count, &consensus, &packed_resolution, live_auxiliary);
  if (record_count == 0)
    {
      if (reject)
        *reject = GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES;
      return NULL;
    }

  goodix_chicago_enrollment_build_metric_data (frame->enhanced, &metric_data);
  metric_data.packed_resolution = packed_resolution;
  probe = g_new0 (GoodixChicagoRuntimeProbe, 1);
  probe->enrollment = goodix_chicago_enrollment_new ();
  if (!goodix_chicago_enrollment_insert_first (
        probe->enrollment, records, record_count, active_count,
        frame->quality, frame->coverage, &metric_data, &result, error))
    return NULL;
  if (!goodix_chicago_enrollment_get_subtemplate (
        probe->enrollment, 0, &probe->view))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: could not expose Chicago runtime probe");
      return NULL;
    }
  probe->view.density_positive_percent = consensus.positive_percent;
  probe->view.density_class = consensus.density_class;
  probe->view.density_inactive_count = consensus.inactive_count;
  memcpy (probe->view.live_auxiliary.values, live_auxiliary, 6);

  return g_steal_pointer (&probe);
}

GoodixChicagoRuntimeProbe *
goodix_chicago_runtime_prepare_probe (
  GoodixChicagoPreprocessor  *preprocessor,
  GoodixChicagoFeatureState  *feature_state,
  const guint16               raw[GOODIX_CHICAGO_PIXELS],
  gboolean                    identify,
  GoodixChicagoRuntimeFrame  *frame,
  GoodixChicagoRuntimeReject *reject,
  GError                    **error)
{
  g_autofree GoodixChicagoRuntimeFrame *own = NULL;

  if (!frame)
    {
      own = g_new (GoodixChicagoRuntimeFrame, 1);
      frame = own;
    }
  if (!goodix_chicago_runtime_preprocess (preprocessor, raw, !identify, frame))
    {
      if (reject)
        *reject = GOODIX_CHICAGO_RUNTIME_REJECT_PREPROCESS;
      return NULL;
    }
  return goodix_chicago_runtime_probe_new (feature_state, frame, identify,
                                           reject, error);
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

static GoodixChicagoEnrollment *
unpack_template (GBytes *packed, GError **error)
{
  const guint8 *data;
  gsize size;

  g_return_val_if_fail (packed != NULL, NULL);
  data = g_bytes_get_data (packed, &size);
  return goodix_chicago_enrollment_unpack (data, size, error);
}

gboolean
goodix_chicago_runtime_match (
  const GoodixChicagoRuntimeProbe  *probe,
  GBytes                           *packed,
  GoodixChicagoMatchTemplateResult *result,
  GError                          **error)
{
  g_autoptr(GoodixChicagoEnrollment) gallery = NULL;

  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  gallery = unpack_template (packed, error);
  if (!gallery)
    return FALSE;

  goodix_chicago_match_template_type24 (
    gallery, &probe->view, GOODIX_CHICAGO_RUNTIME_SELECTOR_THRESHOLD,
    result);
  return TRUE;
}

gboolean
goodix_chicago_runtime_study (
  const GoodixChicagoRuntimeProbe        *probe,
  GBytes                                 *packed,
  const GoodixChicagoMatchTemplateResult *result,
  GBytes                                **updated,
  GError                                **error)
{
  g_autoptr(GoodixChicagoEnrollment) gallery = NULL;
  g_autofree GoodixChicagoRelation *capacity_relations = NULL;
  guint count;
  guint replacement_index = G_MAXUINT;

  g_return_val_if_fail (probe != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (updated != NULL, FALSE);
  *updated = NULL;

  if (result->score <= 0 ||
      !result->selected_index_known || result->selected_index < 0 ||
      !result->study_eligibility_known || !result->study_eligible)
    return TRUE;

  gallery = unpack_template (packed, error);
  if (!gallery)
    return FALSE;

  count = goodix_chicago_enrollment_get_count (gallery);
  if ((guint) result->selected_index >= count ||
      result->study_relation_count != count)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "openchicago: invalid template study result index=%d relations=%u gallery=%u",
                   result->selected_index, result->study_relation_count, count);
      return FALSE;
    }

  goodix_chicago_enrollment_apply_study_hits (gallery, result->hit_increments, count);
  if (count < goodix_chicago_enrollment_get_capacity (gallery))
    {
      if (!goodix_chicago_enrollment_append_study (
            gallery, &probe->view, (guint) result->selected_index,
            result->study_relations, result->study_relation_count,
            NULL, goodix_chicago_match_build_capacity_relation, error))
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
                               "openchicago: capacity selector failed");
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
  *updated = goodix_chicago_enrollment_pack (gallery, error);
  return *updated != NULL;
}
