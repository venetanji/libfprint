// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */

/* Chicago system-template envelope serializer/parser. */

#include <string.h>

#include <gio/gio.h>

#include "../common/goodix-crc.h"
#include "goodix-chicago-template.h"

static guint32
read_le32 (const guint8 *p)
{
  return ((guint32) p[0]) |
         ((guint32) p[1] << 8) |
         ((guint32) p[2] << 16) |
         ((guint32) p[3] << 24);
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

/* EngineAdapter+0x30ca0/+0x53f60. The wrapper accidentally narrows the byte
 * count to uint16 before calculating CRC-32/MPEG-2. Preserve that behavior:
 * completed Chicago templates are commonly larger than 64 KiB. */
static guint32
template_v1_crc (const guint8 *payload,
                 gsize         payload_without_crc_len)
{
  guint16 official_len = (guint16) payload_without_crc_len;

  return goodix_crc32_mpeg2 (payload, official_len);
}

static gboolean
template_type_supported (guint32 type)
{
  return type == GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V1 ||
         type == GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V2;
}

gboolean
goodix_chicago_template_parse (const guint8              *data,
                               gsize                      data_len,
                               GoodixChicagoTemplateView *out_view,
                               GError                   **error)
{
  guint32 type;
  guint32 declared_payload_len;

  g_return_val_if_fail (out_view != NULL, FALSE);
  memset (out_view, 0, sizeof (*out_view));

  if (!data || data_len < GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago system template is too short: %zu bytes",
                   data_len);
      return FALSE;
    }

  type = read_le32 (data);
  declared_payload_len = read_le32 (data + 4);

  if (!template_type_supported (type))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "gdix51c0: unsupported Chicago system template type 0x%08x",
                   type);
      return FALSE;
    }

  /* Both captured EngineAdapter template types declare every byte after the
   * leading type/length pair. Reject truncation and appended garbage before
   * handing a payload to the native Chicago implementation. */
  if ((gsize) declared_payload_len != data_len - 8)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago template length mismatch: header=%u actual=%zu",
                   declared_payload_len, data_len - 8);
      return FALSE;
    }

  out_view->type = type;
  out_view->declared_payload_len = declared_payload_len;
  if (type == GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V1)
    {
      gsize inner_len;
      guint32 stored_crc;
      guint32 expected_crc;

      if (declared_payload_len <
          GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_TRAILER_LEN)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "gdix51c0: Chicago V1 template is too short");
          return FALSE;
        }
      inner_len = declared_payload_len -
                  GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_TRAILER_LEN;
      out_view->chicago_payload =
        data + GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN;
      out_view->chicago_payload_len = inner_len;
      out_view->random = out_view->chicago_payload + inner_len;
      stored_crc = read_le32 (out_view->random +
                              GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN);
      expected_crc = template_v1_crc (
        data + GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN,
        declared_payload_len - GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_CRC_LEN);
      if (stored_crc != expected_crc)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "gdix51c0: Chicago V1 CRC mismatch: stored=0x%08x expected=0x%08x",
                       stored_crc, expected_crc);
          memset (out_view, 0, sizeof (*out_view));
          return FALSE;
        }
      out_view->stored_crc = stored_crc;
    }
  else
    {
      if (data_len < GOODIX_CHICAGO_SYSTEM_TEMPLATE_V2_HEADER_LEN)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "gdix51c0: Chicago V2 template is too short");
          memset (out_view, 0, sizeof (*out_view));
          return FALSE;
        }
      out_view->chicago_payload =
        data + GOODIX_CHICAGO_SYSTEM_TEMPLATE_V2_HEADER_LEN;
      out_view->chicago_payload_len =
        data_len - GOODIX_CHICAGO_SYSTEM_TEMPLATE_V2_HEADER_LEN;
    }
  return TRUE;
}

GBytes *
goodix_chicago_template_wrap_v1 (
  GBytes      *packed_template,
  const guint8 random[GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN])
{
  const guint8 *inner;
  gsize inner_len;
  gsize total_len;
  guint32 payload_len;
  guint8 *wrapped;

  g_return_val_if_fail (packed_template != NULL, NULL);
  g_return_val_if_fail (random != NULL, NULL);
  inner = g_bytes_get_data (packed_template, &inner_len);
  g_return_val_if_fail (inner_len <=
                        G_MAXUINT32 -
                        GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_TRAILER_LEN, NULL);
  payload_len = inner_len +
                GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_TRAILER_LEN;
  total_len = GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN + payload_len;
  wrapped = g_malloc (total_len);
  write_le32 (wrapped, GOODIX_CHICAGO_SYSTEM_TEMPLATE_TYPE_V1);
  write_le32 (wrapped + 4, payload_len);
  memcpy (wrapped + GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN,
          inner, inner_len);
  memcpy (wrapped + GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN + inner_len,
          random, GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_RANDOM_LEN);
  write_le32 (wrapped + total_len -
              GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_CRC_LEN,
              template_v1_crc (
                wrapped + GOODIX_CHICAGO_SYSTEM_TEMPLATE_PREFIX_LEN,
                payload_len - GOODIX_CHICAGO_SYSTEM_TEMPLATE_V1_CRC_LEN));
  return g_bytes_new_take (wrapped, total_len);
}

static void
calibration_digest (GBytes *calibration,
                    guint8  digest[GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN])
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  const guint8 *bytes;
  gsize size;
  gsize digest_size = GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN;

  bytes = g_bytes_get_data (calibration, &size);
  g_checksum_update (checksum, bytes, size);
  g_checksum_get_digest (checksum, digest, &digest_size);
  g_assert_cmpuint (digest_size, ==,
                    GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN);
}

GVariant *
goodix_chicago_print_data_build (
  const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes      *calibration,
  GBytes      *packed_template)
{
  guint8 digest[GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN];
  const guint8 *template_data;
  gsize template_size;

  g_return_val_if_fail (sensor_id != NULL, NULL);
  g_return_val_if_fail (calibration != NULL, NULL);
  g_return_val_if_fail (packed_template != NULL, NULL);
  calibration_digest (calibration, digest);
  template_data = g_bytes_get_data (packed_template, &template_size);
  return g_variant_ref_sink (g_variant_new (
                               "(u@ay@ay@ay)", GOODIX_CHICAGO_PRINT_DATA_VERSION,
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, sensor_id,
                                                          GOODIX_CHICAGO_SENSOR_ID_LEN, 1),
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, digest,
                                                          sizeof (digest), 1),
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, template_data,
                                                          template_size, 1)));
}

gboolean
goodix_chicago_print_data_parse (
  GVariant    *data,
  const guint8 expected_sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes      *expected_calibration,
  GBytes     **packed_template,
  GError     **error)
{
  g_autoptr(GVariant) sensor = NULL;
  g_autoptr(GVariant) calibration_hash = NULL;
  g_autoptr(GVariant) template = NULL;
  const guint8 *sensor_data;
  const guint8 *digest_data;
  const guint8 *template_data;
  guint32 version;
  gsize sensor_size;
  gsize digest_size;
  gsize template_size;

  g_return_val_if_fail (packed_template != NULL, FALSE);
  *packed_template = NULL;
  if (data == NULL || !g_variant_is_of_type (
        data, G_VARIANT_TYPE (GOODIX_CHICAGO_PRINT_DATA_TYPE)))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "gdix51c0: print is not native Chicago data");
      return FALSE;
    }
  g_variant_get (data, "(u@ay@ay@ay)", &version, &sensor,
                 &calibration_hash, &template);
  if (version != GOODIX_CHICAGO_PRINT_DATA_VERSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "gdix51c0: unsupported Chicago print version %u", version);
      return FALSE;
    }
  sensor_data = g_variant_get_fixed_array (sensor, &sensor_size, 1);
  digest_data = g_variant_get_fixed_array (calibration_hash, &digest_size, 1);
  template_data = g_variant_get_fixed_array (template, &template_size, 1);
  if (sensor_size != GOODIX_CHICAGO_SENSOR_ID_LEN ||
      digest_size != GOODIX_CHICAGO_CALIBRATION_DIGEST_LEN ||
      template_size == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: malformed native Chicago print");
      return FALSE;
    }
  if (expected_sensor_id != NULL &&
      memcmp (sensor_data, expected_sensor_id, sensor_size) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: Chicago print belongs to another sensor");
      return FALSE;
    }

  /* Calibration is runtime state, not template identity.  The official
   * NeedUpdateImageBase path mutates its B/ImageBase plane and saves it again;
   * temporal learning can mutate the gain/count state too.  Older native
   * envelopes retained a full-payload digest for diagnostics, but rejecting
   * on that digest makes every legitimate official rebase invalidate all
   * enrolled fingers.  Keep validating the field's shape for format safety,
   * and keep the sensor-id binding above as the cross-device boundary. */
  (void) digest_data;
  (void) expected_calibration;
  *packed_template = g_bytes_new (template_data, template_size);
  return TRUE;
}
