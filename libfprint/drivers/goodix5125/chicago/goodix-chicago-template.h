// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* EngineAdapter system-template envelope used by chicagoHS (GDIX51C0). */

#pragma once

#include <glib.h>

#include "goodix-chicago-calibration.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V1 0xffdffc01u
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V2 0xffdffc02u
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN 8u
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN 32u
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_CRC_LEN 4u
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_TRAILER_LEN \
  (GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN + \
   GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_CRC_LEN)
/* The older V2 layout has only been observed structurally. */
#define GOODIX_CHICAGO_SYSTEM_TEMPLATE_V2_HEADER_LEN 104u
#define GOODIX_CHICAGO_PRINT_DATA_VERSION 1u
#define GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN 32u
#define GOODIX_CHICAGO_PRINT_DATA_TYPE "(uayayay)"

typedef struct
{
  guint32       type;
  guint32       declared_payload_len;
  const guint8 *chicago_payload;
  gsize         chicago_payload_len;
  const guint8 *random;
  guint32       stored_crc;
} GoodixChicagoTemplateView;

/* Validate the system-template envelope and expose the inner Chicago payload.
 * V1's 32-byte random field and its official CRC are validated exactly.
 * @data remains owned by the caller. */
gboolean goodix_chicago_template_parse (const guint8              *data,
                                        gsize                      data_len,
                                        GoodixChicagoTemplateView *out_view,
                                        GError                   **error);

/* Wrap a packed Chicago template in the exact V1 EngineAdapter envelope.
 * @random must contain 32 bytes from a cryptographically secure RNG. Keeping
 * entropy acquisition outside this serializer makes oracle tests repeatable. */
GBytes *goodix_chicago_template_wrap_v1 (GBytes      *packed_template,
                                         const guint8 random[GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN]);

/* Native libfprint payload: version, sensor id, legacy calibration digest,
 * and the CRC-protected packed inner Chicago template. The digest is retained
 * for on-disk compatibility/diagnostics, but is not an identity boundary:
 * official ImageBase and temporal calibration updates mutate the payload. */
GVariant *goodix_chicago_print_data_build (const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                           GBytes      *calibration,
                                           GBytes      *packed_template);

gboolean goodix_chicago_print_data_parse (GVariant    *data,
                                          const guint8 expected_sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                          GBytes      *expected_calibration,
                                          GBytes     **packed_template,
                                          GError     **error);

G_END_DECLS
