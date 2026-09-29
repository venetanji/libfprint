// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Modified for openchicago: only the print_data envelope is kept. */

/* libfprint print_data envelope of a packed Chicago template. */

#include <string.h>

#include <gio/gio.h>


#include "goodix-chicago-template.h"

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

  /* Calibration is runtime state, not template identity.  An
   * ImageBase update mutates its B/ImageBase plane and saves it again;
   * temporal learning can mutate the gain/count state too.  Older native
   * envelopes retained a full-payload digest for diagnostics, but rejecting
   * on that digest makes every legitimate rebase invalidate all
   * enrolled fingers.  Keep validating the field's shape for format safety,
   * and keep the sensor-id binding above as the cross-device boundary. */
  (void) digest_data;
  (void) expected_calibration;
  *packed_template = g_bytes_new (template_data, template_size);
  return TRUE;
}
