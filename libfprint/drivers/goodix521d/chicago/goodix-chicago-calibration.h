// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 * Taken from libfprint MR !648; comments reworded. */

/* Goodix Chicago calibration file parser. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Explicit sensor behavior; the historical API remains ChicagoHS mode 24. */
typedef enum {
  GOODIX_CHICAGO_PROFILE_GF5298 = 17,
  GOODIX_CHICAGO_PROFILE_CHICAGO_HS = 24,
} GoodixChicagoProfile;

#define GOODIX_CHICAGO_SENSOR_ID_LEN 16u
#define GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN 0x224b0u
#define GOODIX_CHICAGO_CALIBRATION_FILE_LEN \
  (GOODIX_CHICAGO_SENSOR_ID_LEN + GOODIX_CHICAGO_CALIBRATION_PAYLOAD_LEN)

#define GOODIX_CHICAGO_IMAGE_WIDTH 80u
#define GOODIX_CHICAGO_IMAGE_HEIGHT 64u
#define GOODIX_CHICAGO_PIXELS \
  (GOODIX_CHICAGO_IMAGE_WIDTH * GOODIX_CHICAGO_IMAGE_HEIGHT)
#define GOODIX_CHICAGO_CALIBRATION_MAP_BYTES \
  (GOODIX_CHICAGO_PIXELS * sizeof (guint16))

/* Fields consumed when a calibration payload is loaded. The two
 * CRC words cover the two 64x80 uint16 maps that are copied into its native
 * preprocessor state.  Later payload fields remain intentionally opaque until
 * their consumers are mapped. */
#define GOODIX_CHICAGO_GAIN_MAP_CRC_OFFSET 0x00000u
#define GOODIX_CHICAGO_OFFSET_MAP_CRC_OFFSET 0x00004u
#define GOODIX_CHICAGO_GAIN_MAP_OFFSET 0x00008u
#define GOODIX_CHICAGO_OFFSET_MAP_OFFSET 0x09928u
/* Sample count in the state header paired with the persistent gain average. */
#define GOODIX_CHICAGO_TEMPORAL_SAMPLE_COUNT_OFFSET 0x18488u

/* Load `goodix_calib.dat` layout used for selector-12
 * ChicagoHS: [16-byte sensor id][one 0x224b0-byte payload]. The returned
 * bytes contain only the calibration payload. */
GBytes *goodix_chicago_calibration_load (const gchar *path,
                                         const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                         GError     **error);

/* Build the deterministic first-run calibration for a no-finger
 * ImageBase. The gain plane starts at Q13 unity, the offset plane is the
 * prepared no-finger ImageBase, and the temporal sample count starts at zero. */
GBytes *goodix_chicago_calibration_generate (const guint16 image_base[GOODIX_CHICAGO_PIXELS]);

/* Reproduce preprocessor_init(ImageBase) on an already loaded calibration:
 * retain the accumulated gain/count/opaque state, replace only the prepared
 * ImageBase (B/offset) plane, and repair its CRC. */
GBytes *goodix_chicago_calibration_rebase (GBytes       *payload,
                                           const guint16 image_base[GOODIX_CHICAGO_PIXELS],
                                           GError      **error);

/* Additive profile-aware calibration preparation. Legacy helpers use mode24. */
GBytes *goodix_chicago_calibration_generate_for_profile (
  const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoProfile profile, GError **error);
GBytes *goodix_chicago_calibration_rebase_for_profile (
  GBytes *payload, const guint16 image_base[GOODIX_CHICAGO_PIXELS],
  GoodixChicagoProfile profile, GError **error);

/* Persist the native sensor-bound file wrapper consumed by load(). */
gboolean goodix_chicago_calibration_save (const gchar *path,
                                          const guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN],
                                          GBytes      *payload,
                                          GError     **error);

/* Validate the payload boundary independently of its sensor-id file wrapper.
 * This is suitable for native preprocessor initialization as well as file
 * loading. */
gboolean goodix_chicago_calibration_validate_payload (GBytes  *payload,
                                                      GError **error);

/* Read one little-endian gain/offset correction pair from a loaded Chicago
 * calibration payload. `pixel` is row-major in the native 64x80 sensor grid.
 * Either output pointer may be NULL. */
gboolean goodix_chicago_calibration_get_corrections (GBytes  *payload,
                                                     guint    pixel,
                                                     guint16 *gain,
                                                     guint16 *offset,
                                                     GError **error);

gboolean goodix_chicago_calibration_get_temporal_sample_count (GBytes  *payload,
                                                               guint32 *sample_count,
                                                               GError **error);

G_END_DECLS
