// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: only the libfprint print_data envelope is kept. */

/* libfprint MR !648 print_data envelope of a packed Chicago template. */

#pragma once

#include <glib.h>

#include "goodix-chicago-calibration.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_PRINT_DATA_VERSION 1u
#define GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN 32u
#define GOODIX_CHICAGO_PRINT_DATA_TYPE "(uayayay)"

/* Native libfprint payload: version, sensor id, legacy calibration digest,
 * and the CRC-protected packed inner Chicago template. The digest is retained
 * for on-disk compatibility/diagnostics, but is not an identity boundary:
 * ImageBase and temporal calibration updates mutate the payload. */
GVariant *goodix_chicago_print_data_build (const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                           GBytes      *calibration,
                                           GBytes      *packed_template);

gboolean goodix_chicago_print_data_parse (GVariant    *data,
                                          const guint8 expected_sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                          GBytes      *expected_calibration,
                                          GBytes     **packed_template,
                                          GError     **error);

G_END_DECLS
