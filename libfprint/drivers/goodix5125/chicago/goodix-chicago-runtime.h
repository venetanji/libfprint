// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* High-level native Chicago composition shared by the live driver and tests. */

#pragma once

#include <gio/gio.h>

#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-match.h"
#include "goodix-chicago-preprocess.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_RUNTIME_SELECTOR_THRESHOLD 207

typedef struct _GoodixChicagoRuntimeProbe GoodixChicagoRuntimeProbe;

typedef enum {
  GOODIX_CHICAGO_RUNTIME_REJECT_NONE,
  GOODIX_CHICAGO_RUNTIME_REJECT_BAD_INPUT,
  GOODIX_CHICAGO_RUNTIME_REJECT_POOR_CAPTURE,
  GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES,
} GoodixChicagoRuntimeReject;

GoodixChicagoRuntimeProbe *goodix_chicago_runtime_prepare_probe (GoodixChicagoPreprocessor  *preprocessor,
                                                                 const guint16               raw[GOODIX_CHICAGO_PIXELS],
                                                                 GoodixChicagoRuntimeReject *reject,
                                                                 GError                    **error);

void goodix_chicago_runtime_probe_free (GoodixChicagoRuntimeProbe *probe);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoRuntimeProbe,
                               goodix_chicago_runtime_probe_free)

const GoodixChicagoSubtemplateView *
goodix_chicago_runtime_probe_get_view (
  const GoodixChicagoRuntimeProbe * probe);

GBytes *goodix_chicago_runtime_probe_pack (const GoodixChicagoRuntimeProbe *probe,
                                           GError                         **error);

gboolean goodix_chicago_runtime_match_print_data (const GoodixChicagoRuntimeProbe  *probe,
                                                  GVariant                         *print_data,
                                                  const guint8                      sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                                  GBytes                           *calibration,
                                                  GoodixChicagoMatchTemplateResult *result,
                                                  GError                          **error);

gint32 goodix_chicago_runtime_score_print_data (const GoodixChicagoRuntimeProbe *probe,
                                                GVariant                        *print_data,
                                                const guint8                     sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                                GBytes                          *calibration,
                                                GError                         **error);

/* Apply the official templateStudy append after a successful match.  A valid
 * match that is not eligible for study is a successful no-op and leaves
 * @updated_print_data set to NULL. */
gboolean goodix_chicago_runtime_study_print_data (const GoodixChicagoRuntimeProbe        *probe,
                                                  GVariant                               *print_data,
                                                  const guint8                            sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                                  GBytes                                 *calibration,
                                                  const GoodixChicagoMatchTemplateResult *result,
                                                  GVariant                              **updated_print_data,
                                                  GError                                **error);

G_END_DECLS
