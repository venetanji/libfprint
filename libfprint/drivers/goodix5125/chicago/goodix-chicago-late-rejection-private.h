// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

#pragma once

#include "goodix-chicago-match.h"

G_BEGIN_DECLS

gboolean goodix_chicago_match_late_rejection_compat (guint                                template_type,
                                                     guint                                width,
                                                     guint                                height,
                                                     gint32                               probe_quality,
                                                     const GoodixChicagoMatchScoreRecord *record,
                                                     const gint32                         transform[6],
                                                     gint32                               current_auxiliary,
                                                     gint32                               candidate_auxiliary,
                                                     gint32                               combined_auxiliary,
                                                     gint32                              *rejection_count,
                                                     gint32                              *status_flag);

G_END_DECLS
