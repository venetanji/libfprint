// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Exact ANSI-381 BIR layout accepted by the production Chicago EngineAdapter. */

#pragma once

#include <glib.h>

#include "goodix-chicago-calibration.h"

G_BEGIN_DECLS

/* This is the fixed, one-view, uncompressed 16-bit ANSI-381 representation
 * used by EngineAdapterAcceptSampleData in the Wine oracle.  It is deliberately
 * a narrow contract, not a general ANSI-381 parser. */
#define GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN 0x20u
#define GOODIX_CHICAGO_BIR_BIOMETRIC_HDR_LEN 0x30u
#define GOODIX_CHICAGO_BIR_STANDARD_LEN 0x38u
#define GOODIX_CHICAGO_BIR_STANDARD_OFFSET \
  (GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN + GOODIX_CHICAGO_BIR_BIOMETRIC_HDR_LEN)
#define GOODIX_CHICAGO_BIR_PIXELS_OFFSET \
  (GOODIX_CHICAGO_BIR_STANDARD_OFFSET + GOODIX_CHICAGO_BIR_STANDARD_LEN)
#define GOODIX_CHICAGO_BIR_RAW_BYTES \
  (GOODIX_CHICAGO_PIXELS * sizeof (guint16))
#define GOODIX_CHICAGO_BIR_TOTAL_LEN \
  (GOODIX_CHICAGO_BIR_PIXELS_OFFSET + GOODIX_CHICAGO_BIR_RAW_BYTES)

typedef struct
{
  const guint8 *raw16_le;
  guint16       columns;
  guint16       rows;
} GoodixChicagoBirView;

/* Serialize one decoded Chicago raw frame in the exact ANSI-381 BIR shape
 * accepted by the production EngineAdapter oracle. The resulting GBytes owns
 * the record and is safe to hand to a future native preprocessing boundary. */
GBytes *goodix_chicago_bir_build (const guint16 *raw16,
                                  GError       **error);

/* Validate the same narrow record shape and expose its little-endian raw
 * pixel payload. @data remains owned by the caller. */
gboolean goodix_chicago_bir_parse (const guint8         *data,
                                   gsize                 data_len,
                                   GoodixChicagoBirView *out_view,
                                   GError              **error);

G_END_DECLS
