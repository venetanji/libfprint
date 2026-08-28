// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Exact ANSI-381 BIR serializer/parser used at the Chicago raw-frame boundary. */

#include <gio/gio.h>

#include "goodix-chicago-bir.h"

static guint16
read_le16 (const guint8 *p)
{
  return (guint16) p[0] | ((guint16) p[1] << 8);
}

static guint32
read_le32 (const guint8 *p)
{
  return ((guint32) p[0]) |
         ((guint32) p[1] << 8) |
         ((guint32) p[2] << 16) |
         ((guint32) p[3] << 24);
}

static void
write_le16 (guint8 *p,
            guint16 value)
{
  p[0] = (guint8) value;
  p[1] = (guint8) (value >> 8);
}

static void
write_le32 (guint8 *p,
            guint32 value)
{
  p[0] = (guint8) value;
  p[1] = (guint8) (value >> 8);
  p[2] = (guint8) (value >> 16);
  p[3] = (guint8) (value >> 24);
}

GBytes *
goodix_chicago_bir_build (const guint16 *raw16,
                          GError       **error)
{
  guint8 *record;

  if (!raw16)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "gdix51c0: cannot build Chicago BIR without a raw frame");
      return NULL;
    }

  record = g_malloc0 (GOODIX_CHICAGO_BIR_TOTAL_LEN);

  /* WINBIO_BIR header: offsets point at the biometric header and the ANSI
   * standard block. These values reproduce engine_probe.c::make_sample(). */
  write_le32 (record + 0x00, GOODIX_CHICAGO_BIR_BIOMETRIC_HDR_LEN);
  write_le32 (record + 0x04, GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN);
  write_le32 (record + 0x08,
              GOODIX_CHICAGO_BIR_STANDARD_LEN + GOODIX_CHICAGO_BIR_RAW_BYTES);
  write_le32 (record + 0x0c, GOODIX_CHICAGO_BIR_STANDARD_OFFSET);
  write_le16 (record + GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN + 0x28, 0x001b);
  write_le16 (record + GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN + 0x2a, 0x0401);

  /* ANSI-381 standard data block. */
  /* The ANSI RecordLength field is 64-bit little-endian. Its upper 32 bits
   * remain zero from g_malloc0(); write the lower size explicitly. */
  write_le32 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x00,
              GOODIX_CHICAGO_BIR_STANDARD_LEN + GOODIX_CHICAGO_BIR_RAW_BYTES);
  write_le32 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x08, 0x46495200);
  write_le32 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x0c, 0x30313000);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x10, 0x001b);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x12, 0x0401);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x14, 1);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x16, 1);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x18, 500);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x1a, 500);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x1c, 500);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x1e, 500);
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x20] = 1;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x21] = 1;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x22] = 16;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x23] = 0;
  write_le32 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x28,
              0x10 + GOODIX_CHICAGO_BIR_RAW_BYTES);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x2c,
              GOODIX_CHICAGO_IMAGE_WIDTH);
  write_le16 (record + GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x2e,
              GOODIX_CHICAGO_IMAGE_HEIGHT);
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x30] = 2;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x31] = 1;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x32] = 1;
  record[GOODIX_CHICAGO_BIR_STANDARD_OFFSET + 0x33] = 0xfe;

  for (gsize i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    write_le16 (record + GOODIX_CHICAGO_BIR_PIXELS_OFFSET + i * sizeof (guint16),
                raw16[i]);

  return g_bytes_new_take (record, GOODIX_CHICAGO_BIR_TOTAL_LEN);
}

gboolean
goodix_chicago_bir_parse (const guint8         *data,
                          gsize                 data_len,
                          GoodixChicagoBirView *out_view,
                          GError              **error)
{
  const guint8 *standard;

  g_return_val_if_fail (out_view != NULL, FALSE);
  out_view->raw16_le = NULL;
  out_view->columns = 0;
  out_view->rows = 0;

  if (!data || data_len != GOODIX_CHICAGO_BIR_TOTAL_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago BIR length is %zu, expected %zu",
                   data_len, (gsize) GOODIX_CHICAGO_BIR_TOTAL_LEN);
      return FALSE;
    }

  if (read_le32 (data + 0x00) != GOODIX_CHICAGO_BIR_BIOMETRIC_HDR_LEN ||
      read_le32 (data + 0x04) != GOODIX_CHICAGO_BIR_HEADER_BLOCK_LEN ||
      read_le32 (data + 0x08) != GOODIX_CHICAGO_BIR_STANDARD_LEN + GOODIX_CHICAGO_BIR_RAW_BYTES ||
      read_le32 (data + 0x0c) != GOODIX_CHICAGO_BIR_STANDARD_OFFSET)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: Chicago BIR wrapper offsets are invalid");
      return FALSE;
    }

  standard = data + GOODIX_CHICAGO_BIR_STANDARD_OFFSET;
  if (read_le32 (standard + 0x00) != GOODIX_CHICAGO_BIR_STANDARD_LEN + GOODIX_CHICAGO_BIR_RAW_BYTES ||
      read_le32 (standard + 0x04) != 0 ||
      read_le32 (standard + 0x08) != 0x46495200 ||
      read_le32 (standard + 0x0c) != 0x30313000 ||
      read_le16 (standard + 0x10) != 0x001b ||
      read_le16 (standard + 0x12) != 0x0401 ||
      read_le16 (standard + 0x14) != 1 ||
      read_le16 (standard + 0x16) != 1 ||
      read_le16 (standard + 0x18) != 500 ||
      read_le16 (standard + 0x1a) != 500 ||
      read_le16 (standard + 0x1c) != 500 ||
      read_le16 (standard + 0x1e) != 500 ||
      standard[0x20] != 1 || standard[0x21] != 1 ||
      standard[0x22] != 16 || standard[0x23] != 0 ||
      read_le32 (standard + 0x28) != 0x10 + GOODIX_CHICAGO_BIR_RAW_BYTES ||
      read_le16 (standard + 0x2c) != GOODIX_CHICAGO_IMAGE_WIDTH ||
      read_le16 (standard + 0x2e) != GOODIX_CHICAGO_IMAGE_HEIGHT ||
      standard[0x30] != 2 || standard[0x31] != 1 ||
      standard[0x32] != 1 || standard[0x33] != 0xfe)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: Chicago BIR ANSI-381 image record is invalid");
      return FALSE;
    }

  out_view->raw16_le = data + GOODIX_CHICAGO_BIR_PIXELS_OFFSET;
  out_view->columns = GOODIX_CHICAGO_IMAGE_WIDTH;
  out_view->rows = GOODIX_CHICAGO_IMAGE_HEIGHT;
  return TRUE;
}
