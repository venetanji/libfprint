/*
 * Sensor identification and HAL table types for Validity/Synaptics VCSFW
 *
 * Copyright (C) 2024 libfprint contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

/*
 * SensorTypeInfo — sensor geometry and calibration parameters.
 * Derived from python-validity SensorTypeInfo (generated_tables.py).
 *
 * Each sensor type has fixed imaging geometry (bytes per scan line,
 * repeat multiplier for frame size, calibration dimensions) and an
 * optional calibration lookup blob.
 */
typedef struct
{
  guint16       sensor_type;
  guint16       bytes_per_line;
  guint8        repeat_multiplier;
  guint16       lines_per_calibration_data;
  guint16       line_width;
  const guint8 *calibration_blob;     /* may be NULL */
  gsize         calibration_blob_len;
} ValiditySensorTypeInfo;

/*
 * DeviceInfo — hardware identity to sensor type mapping.
 * Derived from python-validity DeviceInfo (hw_tables.py).
 *
 * The identify_sensor command (0x75) returns a hardware major + version.
 * DeviceInfo maps those to a sensor type (→ SensorTypeInfo) and a human
 * readable name.
 */
typedef struct
{
  guint16     major;
  guint16     type;           /* sensor type for SensorTypeInfo lookup */
  guint8      version;
  guint8      version_mask;   /* 0xff = exact match, 0x00 = wildcard */
  const char *name;
} ValidityDeviceInfo;

/*
 * Sensor identification from cmd 0x75 response.
 */
typedef struct
{
  guint16 hw_major;   /* hardware major (→ DeviceInfo.major) */
  guint16 hw_version; /* hardware version (→ DeviceInfo.version) */
} ValiditySensorIdent;

/*
 * Aggregate sensor state stored in FpiDeviceValidity.
 */
typedef struct
{
  ValiditySensorIdent           ident;
  const ValidityDeviceInfo     *device_info;
  const ValiditySensorTypeInfo *type_info;

  /* True silicon type for the finger-down interrupt logic. Normally equal to
   * device_info->type, but forced to 0x969 for FM-154/FM-3439 units that
   * re-enumerate reporting type 0x199 after suspend/resume (see validity.c). */
  guint16 real_sensor_type;

  /* Factory calibration bits (raw response from cmd 0x6f) */
  guint8 *factory_bits;
  gsize   factory_bits_len;
} ValiditySensorState;

/* ---- Parsing functions ---- */

/*
 * Parse the response from VCSFW_CMD_IDENTIFY_SENSOR (0x75).
 * Response format (after 2-byte status): zeroes(4LE) | version(2LE) | major(2LE).
 * Returns FALSE if the data is too short.
 */
gboolean validity_sensor_parse_identify (const guint8        *data,
                                         gsize                data_len,
                                         ValiditySensorIdent *out);

/* ---- HAL table lookups ---- */

/*
 * Look up a DeviceInfo entry by hardware major and version.
 * Exact match on (major, version & version_mask) is preferred;
 * falls back to fuzzy match when version_mask == 0.
 * Returns NULL if no match found.
 */
const ValidityDeviceInfo *validity_device_info_lookup (guint16 major,
                                                       guint16 version);

/*
 * Look up a SensorTypeInfo entry by sensor type.
 * Returns NULL if the type is not in the table.
 */
const ValiditySensorTypeInfo *validity_sensor_type_info_lookup (guint16 sensor_type);

/*
 * TRUE for silicon that never emits the type-2 finger-down interrupt (sensor
 * types 0x969 and 0xd51); such sensors signal finger contact with a type-3
 * capture event instead. See validity_sensor.c for details.
 */
gboolean validity_sensor_skips_finger_down (guint16 sensor_type);

/* ---- Command building ---- */

/*
 * Build the command bytes for VCSFW_CMD_GET_FACTORY_BITS (0x6f).
 * Format: cmd(1) | tag(2LE) | pad(2LE=0) | pad(4LE=0) = 9 bytes.
 * Returns the number of bytes written, or 0 if buf_len < 9.
 */
gsize validity_sensor_build_factory_bits_cmd (guint16 tag,
                                              guint8 *buf,
                                              gsize   buf_len);

/* ---- Lifecycle ---- */

void validity_sensor_state_init (ValiditySensorState *state);
void validity_sensor_state_clear (ValiditySensorState *state);
