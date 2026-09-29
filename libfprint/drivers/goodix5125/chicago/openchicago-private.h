// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com> */
/* openchicago internals for the tests (not installed). */

#pragma once

#include "openchicago.h"

G_BEGIN_DECLS

/* TRUE when both sessions hold identical preprocessor and feature extraction state
 * (compared member by member in memory, independent of the serializer). */
gboolean _oc_session_state_equal (OcSession *a,
                                  OcSession *b);

/* Drop the last frame of the enrolment in progress (the
 * backup path of OC_ENROLL_ENGINE uses it). */
gboolean _oc_enroll_delete_last (OcSession *self);

G_END_DECLS
