/*
 * Runtime data file loader for Validity/Synaptics VCSFW fingerprint sensors
 *
 * Loads per-device blob data and common pairing/TLS constants from
 * external .bin files installed by the libfprint-validity-data package.
 *
 * File format: [raw_data: N bytes][HMAC-SHA256: 32 bytes]
 * Install path: /usr/share/libfprint/validity/
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

/* HMAC-SHA256 trailer size appended to each .bin file */
#define VALIDITY_DATA_HMAC_SIZE 32

/* Minimum file size: at least 1 byte of data + 32 bytes HMAC */
#define VALIDITY_DATA_MIN_FILE_SIZE (1 + VALIDITY_DATA_HMAC_SIZE)

/* Data file tags — identifies which blob is being loaded */
typedef enum {
  /* Per-device blobs (in <vid>_<pid>/ subdirectory) */
  VALIDITY_DATA_INIT = 0,
  VALIDITY_DATA_INIT_CLEAN_SLATE,
  VALIDITY_DATA_RESET,
  VALIDITY_DATA_DB_WRITE_ENABLE,

  /* Common data files (in base directory) */
  VALIDITY_DATA_PARTITION_SIG_STANDARD,
  VALIDITY_DATA_PARTITION_SIG_0090,
  VALIDITY_DATA_CA_PUBKEY,
  VALIDITY_DATA_TLS_PASSWORD,
  VALIDITY_DATA_GWK_SIGN,
  VALIDITY_DATA_FW_PUBKEY_X,
  VALIDITY_DATA_FW_PUBKEY_Y,

  VALIDITY_DATA_NUM_TAGS,
} ValidityDataTag;

/* Container for loaded data files */
typedef struct
{
  GBytes *entries[VALIDITY_DATA_NUM_TAGS];
} ValidityDataStore;

/* Search paths for data files (defined in validity_data.c) */
extern const gchar *validity_data_search_paths[];

/* Initialize a data store (all entries NULL) */
void     validity_data_store_init (ValidityDataStore *store);

/* Free all loaded data in a store */
void     validity_data_store_free (ValidityDataStore *store);

/* Load a single .bin file, verify its HMAC, and store in the given tag slot.
 * Returns TRUE on success. On failure, sets error and returns FALSE. */
gboolean validity_data_load_file (ValidityDataStore *store,
                                  ValidityDataTag    tag,
                                  const gchar       *filepath,
                                  GError           **error);

/* Load all per-device blob files for the given VID/PID.
 * Searches validity_data_search_paths for <vid>_<pid>/ subdirectory.
 * Returns TRUE if all required blobs found, FALSE with error otherwise. */
gboolean validity_data_load_device (ValidityDataStore *store,
                                    guint16            vid,
                                    guint16            pid,
                                    GError           **error);

/* Load all common data files (partition sigs, CA cert, TLS keys, etc.).
 * Returns TRUE if all files found, FALSE with error otherwise. */
gboolean validity_data_load_common (ValidityDataStore *store,
                                    GError           **error);

/* Get the raw data bytes for a given tag (without HMAC trailer).
 * Returns NULL if the tag has not been loaded. The returned GBytes
 * is owned by the store — do not unref. */
GBytes  *validity_data_get (const ValidityDataStore *store,
                            ValidityDataTag          tag);

/* Convenience: get raw data pointer and length for a tag.
 * Returns NULL with *len=0 if not loaded. */
const guint8 *validity_data_get_bytes (const ValidityDataStore *store,
                                       ValidityDataTag          tag,
                                       gsize                   *len);
