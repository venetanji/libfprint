// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Taken from libfprint MR !648; comments reworded. */

/* Goodix Chicago calibration file parser. */

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "goodix-chicago-calibration.h"

G_STATIC_ASSERT (GOODIX_CHICAGO_GAIN_MAP_OFFSET +
                 GOODIX_CHICAGO_CALIBRATION_MAP_BYTES <=
                 GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
G_STATIC_ASSERT (GOODIX_CHICAGO_OFFSET_MAP_OFFSET +
                 GOODIX_CHICAGO_CALIBRATION_MAP_BYTES <=
                 GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);

static guint16
read_le16 (const guint8 *data)
{
  return (guint16) data[0] | ((guint16) data[1] << 8);
}

static guint32
read_le32 (const guint8 *data)
{
  return (guint32) data[0] |
         ((guint32) data[1] << 8) |
         ((guint32) data[2] << 16) |
         ((guint32) data[3] << 24);
}

static void
write_le16 (guint8 *data,
            guint16 value)
{
  data[0] = value;
  data[1] = value >> 8;
}

static void
write_le32 (guint8 *data,
            guint32 value)
{
  data[0] = value;
  data[1] = value >> 8;
  data[2] = value >> 16;
  data[3] = value >> 24;
}

/* CRC-32/ISO-HDLC's reflected loop, but with no final
 * complement. The caller initializes this value to all ones. */
static guint32
calibration_map_crc32 (const guint8 *data,
                       gsize         length)
{
  guint32 crc = G_MAXUINT32;

  for (gsize i = 0; i < length; i++)
    {
      crc ^= data[i];
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }

  return crc;
}

static void
calibration_set_image_base (guint8       *payload,
                            const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                            GoodixChicagoProfile profile)
{
  guint16 prepared[GOODIX_CHICAGO_PIXELS];

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    prepared[pixel] = MIN (image_base[pixel], 0x0fffu);

  /* Mode-0x18 input preparation repairs only the non-corner
   * samples in the first and last rows. */
  for (guint column = 1;
       column + 1 < GOODIX_CHICAGO_IMAGE_WIDTH;
       column++)
    {
      prepared[column] =
        prepared[GOODIX_CHICAGO_IMAGE_WIDTH + column];
      prepared[(GOODIX_CHICAGO_IMAGE_HEIGHT - 1) *
               GOODIX_CHICAGO_IMAGE_WIDTH + column] =
        prepared[(GOODIX_CHICAGO_IMAGE_HEIGHT - 2) *
                 GOODIX_CHICAGO_IMAGE_WIDTH + column];
    }

  if (profile == GOODIX_CHICAGO_PROFILE_GF5298)
    {
      /* Mode17 repairs all four edges, including corners. The non-corner
       * top/bottom samples have already been prepared above. */
      prepared[0] = prepared[GOODIX_CHICAGO_IMAGE_WIDTH];
      prepared[GOODIX_CHICAGO_IMAGE_WIDTH - 1] = prepared[2 * GOODIX_CHICAGO_IMAGE_WIDTH - 1];
      prepared[(GOODIX_CHICAGO_IMAGE_HEIGHT - 1) * GOODIX_CHICAGO_IMAGE_WIDTH] =
        prepared[(GOODIX_CHICAGO_IMAGE_HEIGHT - 2) * GOODIX_CHICAGO_IMAGE_WIDTH];
      prepared[GOODIX_CHICAGO_PIXELS - 1] = prepared[GOODIX_CHICAGO_PIXELS - GOODIX_CHICAGO_IMAGE_WIDTH - 1];
      for (guint y = 0; y < GOODIX_CHICAGO_IMAGE_HEIGHT; y++)
        {
          guint row = y * GOODIX_CHICAGO_IMAGE_WIDTH;
          prepared[row] = prepared[row + 1];
          prepared[row + GOODIX_CHICAGO_IMAGE_WIDTH - 1] = prepared[row + GOODIX_CHICAGO_IMAGE_WIDTH - 2];
        }
    }

  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    write_le16 (payload + GOODIX_CHICAGO_OFFSET_MAP_OFFSET +
                (gsize) pixel * sizeof (guint16),
                prepared[pixel] +
                (profile == GOODIX_CHICAGO_PROFILE_GF5298 ? 7095u : 0u));

  write_le32 (payload + GOODIX_CHICAGO_OFFSET_MAP_CRC_OFFSET,
              calibration_map_crc32 (
                payload + GOODIX_CHICAGO_OFFSET_MAP_OFFSET,
                GOODIX_CHICAGO_CALIBRATION_MAP_BYTES));
}

GBytes *
goodix_chicago_calibration_generate (
  const guint16 image_base[GOODIX_CHICAGO_PIXELS])
{
  return goodix_chicago_calibration_generate_for_profile (
    image_base, GOODIX_CHICAGO_PROFILE_CHICAGO_HS, NULL);
}

GBytes *
goodix_chicago_calibration_generate_for_profile (
  const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoProfile profile, GError **error)
{
  static const guint8 version[] = "Preprocess_v_1.01.01";
  guint8 *payload;

  g_return_val_if_fail (image_base != NULL, NULL);

  if (profile != GOODIX_CHICAGO_PROFILE_CHICAGO_HS &&
      profile != GOODIX_CHICAGO_PROFILE_GF5298)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "openchicago: unsupported calibration profile");
      return NULL;
    }

  payload = g_malloc0 (GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);

  /* Initialize the active gain plane to Q13 unity and
   * the offset plane to zero. preprocessor_init then prepares ImageBase with
   * the same 12-bit clamp and edge-row repair as ordinary raw input before
   * making that prepared frame the initial offset/B plane. */
  for (guint pixel = 0; pixel < GOODIX_CHICAGO_PIXELS; pixel++)
    {
      const gsize map_offset = (gsize) pixel * sizeof (guint16);

      write_le16 (payload + GOODIX_CHICAGO_GAIN_MAP_OFFSET + map_offset,
                  0x2000u);
    }

  write_le32 (payload + GOODIX_CHICAGO_GAIN_MAP_CRC_OFFSET,
              calibration_map_crc32 (
                payload + GOODIX_CHICAGO_GAIN_MAP_OFFSET,
                GOODIX_CHICAGO_CALIBRATION_MAP_BYTES));
  calibration_set_image_base (payload, image_base, profile);
  write_le32 (payload + GOODIX_CHICAGO_TEMPORAL_SAMPLE_COUNT_OFFSET, 0);

  /* The native runtime intentionally consumes only the map/count
   * boundary, but retaining the version marker makes the generated
   * payload self-describing and loadable as a regular calibration file. */
  memcpy (payload + 0x22490u, version, sizeof (version));

  return g_bytes_new_take (payload,
                           GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
}

GBytes *
goodix_chicago_calibration_rebase (
  GBytes       *payload,
  const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  GError      **error)
{
  return goodix_chicago_calibration_rebase_for_profile (
    payload, image_base, GOODIX_CHICAGO_PROFILE_CHICAGO_HS, error);
}

GBytes *
goodix_chicago_calibration_rebase_for_profile (
  GBytes *payload, const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoProfile profile, GError **error)
{
  const guint8 *payload_data;
  guint8 *rebased_data;
  gsize payload_len = 0;

  g_return_val_if_fail (payload != NULL, NULL);
  g_return_val_if_fail (image_base != NULL, NULL);

  if (!goodix_chicago_calibration_validate_payload (payload, error))
    return NULL;

  if (profile != GOODIX_CHICAGO_PROFILE_CHICAGO_HS &&
      profile != GOODIX_CHICAGO_PROFILE_GF5298)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "openchicago: unsupported calibration profile");
      return NULL;
    }
  payload_data = g_bytes_get_data (payload, &payload_len);
  rebased_data = g_memdup2 (payload_data, payload_len);
  calibration_set_image_base (rebased_data, image_base, profile);

  return g_bytes_new_take (rebased_data, payload_len);
}

gboolean
goodix_chicago_calibration_save (
  const gchar *path,
  const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
  GBytes      *payload,
  GError     **error)
{
  const guint8 *payload_data;
  g_autofree guint8 *file_data = NULL;
  gsize payload_len = 0;
  gboolean success;

  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (sensor_id != NULL, FALSE);
  g_return_val_if_fail (payload != NULL, FALSE);

  if (!goodix_chicago_calibration_validate_payload (payload, error))
    return FALSE;

  payload_data = g_bytes_get_data (payload, &payload_len);
  file_data = g_malloc (GOODIX_CHICAGO_CALIBRATION_FILE_LEN);
  memcpy (file_data, sensor_id, GOODIX_CHICAGO_SENSOR_ID_LEN);
  memcpy (file_data + GOODIX_CHICAGO_SENSOR_ID_LEN,
          payload_data, payload_len);

  success = g_file_set_contents_full (
    path, (const gchar *) file_data, GOODIX_CHICAGO_CALIBRATION_FILE_LEN,
    G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
    0600, error);
  if (!success)
    return FALSE;
  if (g_chmod (path, 0600) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Could not protect Chicago calibration %s: %s",
                   path, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

GBytes *
goodix_chicago_calibration_load (const gchar *path,
                                 const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                 GError     **error)
{
  g_autofree gchar *contents = NULL;
  GBytes *payload;
  gsize contents_len = 0;

  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (sensor_id != NULL, NULL);

  if (!g_file_get_contents (path, &contents, &contents_len, error))
    return NULL;

  if (contents_len != GOODIX_CHICAGO_CALIBRATION_FILE_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago calibration %s is %zu bytes, expected %zu",
                   path, contents_len,
                   (gsize) GOODIX_CHICAGO_CALIBRATION_FILE_LEN);
      return NULL;
    }

  if (memcmp (contents, sensor_id, GOODIX_CHICAGO_SENSOR_ID_LEN) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "gdix51c0: Chicago calibration belongs to another sensor");
      return NULL;
    }

  payload = g_bytes_new (contents + GOODIX_CHICAGO_SENSOR_ID_LEN,
                         GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
  if (!goodix_chicago_calibration_validate_payload (payload, error))
    {
      g_bytes_unref (payload);
      return NULL;
    }

  return payload;
}

gboolean
goodix_chicago_calibration_validate_payload (GBytes  *payload,
                                             GError **error)
{
  const guint8 *data;
  gsize data_len = 0;

  g_return_val_if_fail (payload != NULL, FALSE);

  data = g_bytes_get_data (payload, &data_len);
  if (data_len != GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago calibration payload is %zu bytes, expected %u",
                   data_len, GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
      return FALSE;
    }
  if (read_le32 (data + GOODIX_CHICAGO_GAIN_MAP_CRC_OFFSET) !=
      calibration_map_crc32 (data + GOODIX_CHICAGO_GAIN_MAP_OFFSET,
                             GOODIX_CHICAGO_CALIBRATION_MAP_BYTES) ||
      read_le32 (data + GOODIX_CHICAGO_OFFSET_MAP_CRC_OFFSET) !=
      calibration_map_crc32 (data + GOODIX_CHICAGO_OFFSET_MAP_OFFSET,
                             GOODIX_CHICAGO_CALIBRATION_MAP_BYTES))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: Chicago calibration map CRC mismatch");
      return FALSE;
    }
  return TRUE;
}

gboolean
goodix_chicago_calibration_get_corrections (GBytes  *payload,
                                            guint    pixel,
                                            guint16 *gain,
                                            guint16 *offset,
                                            GError **error)
{
  const guint8 *data;
  gsize data_len = 0;
  gsize map_offset;

  g_return_val_if_fail (payload != NULL, FALSE);

  data = g_bytes_get_data (payload, &data_len);
  if (data_len != GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago calibration payload is %zu bytes, expected %u",
                   data_len, GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
      return FALSE;
    }
  if (pixel >= GOODIX_CHICAGO_PIXELS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "gdix51c0: Chicago calibration pixel %u is outside 64x80", pixel);
      return FALSE;
    }

  map_offset = (gsize) pixel * sizeof (guint16);
  if (gain)
    *gain = read_le16 (data + GOODIX_CHICAGO_GAIN_MAP_OFFSET + map_offset);
  if (offset)
    *offset = read_le16 (data + GOODIX_CHICAGO_OFFSET_MAP_OFFSET + map_offset);
  return TRUE;
}

gboolean
goodix_chicago_calibration_get_temporal_sample_count (
  GBytes  *payload,
  guint32 *sample_count,
  GError **error)
{
  const guint8 *data;
  gsize data_len = 0;

  g_return_val_if_fail (payload != NULL, FALSE);
  g_return_val_if_fail (sample_count != NULL, FALSE);

  data = g_bytes_get_data (payload, &data_len);
  if (data_len != GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gdix51c0: Chicago calibration payload is %zu bytes, expected %u",
                   data_len, GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN);
      return FALSE;
    }

  *sample_count = read_le32 (
    data + GOODIX_CHICAGO_TEMPORAL_SAMPLE_COUNT_OFFSET);
  /* used as a divisor n + 1; outside the payload checksums */
  if (*sample_count == G_MAXUINT32)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "gdix51c0: Chicago calibration sample count out of range");
      return FALSE;
    }
  return TRUE;
}
