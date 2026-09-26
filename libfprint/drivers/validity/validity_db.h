/*
 * Database operations for Validity/Synaptics VCSFW sensors
 *
 * Implements on-chip template database management: listing users,
 * storing/deleting fingerprint records, and db_write_enable.
 *
 * The sensor has a hierarchical record DB stored on flash partition 4:
 *   Storage → User → Finger → Data
 *
 * Reference: python-validity db.py, flash.py
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

/* ================================================================
 * Database record types (from python-validity observation)
 * ================================================================ */
#define VALIDITY_DB_RECORD_TYPE_STORAGE 4
#define VALIDITY_DB_RECORD_TYPE_USER 5
#define VALIDITY_DB_RECORD_TYPE_FINGER 6
#define VALIDITY_DB_RECORD_TYPE_DATA 8

/* Identity type used in record payloads */
#define VALIDITY_IDENTITY_TYPE_SID 3

/* Minimum size for encoded identity (Windows union minimum = 0x4c) */
#define VALIDITY_IDENTITY_MIN_SIZE 0x4C

/* Storage name used by the sensor's DB */
#define VALIDITY_STORAGE_NAME "StgWindsor"

/* Flash partition for calibration data */
#define VALIDITY_FLASH_CALIBRATION_PARTITION 6

/* Clean slate flash header magic */
#define VALIDITY_CLEAN_SLATE_MAGIC 0x5002

/* ================================================================
 * DB Info — returned by cmd 0x45
 * ================================================================ */
typedef struct
{
  guint32  unknown1;   /* Always 1 */
  guint32  unknown0;   /* Always 0 */
  guint32  total;      /* Partition size */
  guint32  used;       /* Used (not deleted) */
  guint32  free_space; /* Unallocated space */
  guint16  records;    /* Total number, including deleted */
  guint16  n_roots;    /* Number of root records */
  guint16 *roots;      /* Root record IDs (owned, g_free) */
} ValidityDbInfo;

/* ================================================================
 * User Storage — returned by cmd 0x4B
 * Represents a named storage container that holds users.
 * ================================================================ */
typedef struct
{
  guint16 dbid;
  guint16 user_count;
  gchar  *name;        /* owned, g_free */

  /* Per-user entries: array of {dbid, value_size} pairs */
  guint16 *user_dbids;
  guint16 *user_val_sizes;
} ValidityUserStorage;

/* ================================================================
 * Finger record entry — part of a User record
 * ================================================================ */
typedef struct
{
  guint16 dbid;
  guint16 subtype;     /* WINBIO finger constant (1-10) */
  guint16 storage;
  guint16 value_size;
} ValidityFingerEntry;

/* ================================================================
 * User — returned by cmd 0x4A
 * ================================================================ */
typedef struct
{
  guint16              dbid;
  guint16              finger_count;
  guint8              *identity;      /* Raw identity bytes, owned */
  gsize                identity_len;
  ValidityFingerEntry *fingers;       /* owned array of finger_count entries */
} ValidityUser;

/* ================================================================
 * DB Record — returned by cmd 0x49 (get_record_value)
 * ================================================================ */
typedef struct
{
  guint16 dbid;
  guint16 type;
  guint16 storage;
  guint8 *value;       /* owned, g_free */
  gsize   value_len;
} ValidityDbRecord;

/* ================================================================
 * Record child entry — from cmd 0x46 (get_record_children)
 * ================================================================ */
typedef struct
{
  guint16 dbid;
  guint16 type;
} ValidityRecordChild;

typedef struct
{
  guint16              dbid;
  guint16              type;
  guint16              storage;
  guint16              child_count;
  ValidityRecordChild *children;   /* owned array */
} ValidityRecordChildren;

/* ================================================================
 * Match result — parsed from cmd 0x60 (get_match_result) response
 * ================================================================ */
typedef struct
{
  gboolean matched;      /* TRUE if user_dbid (tag 1) was found */
  guint32  user_dbid;    /* tag 1: matched user record dbid */
  guint16  subtype;      /* tag 3: matched finger subtype */
  guint8  *hash;         /* tag 4: match hash (owned, g_free) */
  gsize    hash_len;
} ValidityMatchResult;

void validity_match_result_clear (ValidityMatchResult *result);

gboolean validity_parse_match_result (const guint8        *data,
                                      gsize                data_len,
                                      ValidityMatchResult *result);

/* ================================================================
 * Command builders — produce binary TLS command payloads
 *
 * All returned buffers are g_malloc'd and must be g_free'd by caller.
 * ================================================================ */

/* cmd 0x4B: Get user storage by name */
guint8 *validity_db_build_cmd_get_user_storage (const gchar *name,
                                                gsize       *out_len);

/* cmd 0x4A: Get user by dbid */
guint8 *validity_db_build_cmd_get_user (guint16 dbid,
                                        gsize  *out_len);

/* cmd 0x47: New record */
guint8 *validity_db_build_cmd_new_record (guint16       parent,
                                          guint16       type,
                                          guint16       storage,
                                          const guint8 *data,
                                          gsize         data_len,
                                          gsize        *out_len);

/* cmd 0x48: Delete record */
guint8 *validity_db_build_cmd_del_record (guint16 dbid,
                                          gsize  *out_len);

/* cmd 0x1a: Call cleanups (commit pending writes) */
guint8 *validity_db_build_cmd_call_cleanups (gsize *out_len);

/* User record DB partition -- erase this to reclaim flash after 0x04b5. */
#define VALIDITY_DB_PARTITION 4
guint8 *validity_db_build_cmd_erase_flash (guint8 partition,
                                           gsize *out_len);

guint8 *validity_db_build_cmd_db_info (gsize *out_len);

/* ================================================================
 * Response parsers — parse binary TLS response payloads
 *
 * All parse functions take data AFTER the 2-byte status has been stripped.
 * Return TRUE on success, FALSE on parse error.
 * ================================================================ */

gboolean validity_db_parse_info (const guint8   *data,
                                 gsize           data_len,
                                 ValidityDbInfo *out);

gboolean validity_db_parse_user_storage (const guint8        *data,
                                         gsize                data_len,
                                         ValidityUserStorage *out);

gboolean validity_db_parse_user (const guint8 *data,
                                 gsize         data_len,
                                 ValidityUser *out);

gboolean validity_db_parse_record_value (const guint8     *data,
                                         gsize             data_len,
                                         ValidityDbRecord *out);

gboolean validity_db_parse_record_children (const guint8           *data,
                                            gsize                   data_len,
                                            ValidityRecordChildren *out);

/* cmd 0x47 response: parse new record ID */
gboolean validity_db_parse_new_record_id (const guint8 *data,
                                          gsize         data_len,
                                          guint16      *out_record_id);

/* ================================================================
 * Identity helpers
 * ================================================================ */

/* Build an identity blob from an id string, suitable for the sensor DB.
 * Returns a buffer of at least VALIDITY_IDENTITY_MIN_SIZE bytes. */
guint8 *validity_db_build_identity (const gchar *id_str,
                                    gsize       *out_len);

/* Extract the id bytes from a user record's identity (the inverse of
 * validity_db_build_identity). @out_id points into @user->identity. */
gboolean validity_db_user_get_id (const ValidityUser *user,
                                  const guint8      **out_id,
                                  gsize              *out_id_len);

/* Build finger data payload for new_finger (format from make_finger_data) */
guint8 *validity_db_build_finger_data (guint16       subtype,
                                       const guint8 *template_data,
                                       gsize         template_len,
                                       const guint8 *tid,
                                       gsize         tid_len,
                                       gsize        *out_len);

/* ================================================================
 * Structure cleanup helpers
 * ================================================================ */

void validity_db_info_clear (ValidityDbInfo *info);
void validity_user_storage_clear (ValidityUserStorage *storage);
void validity_user_clear (ValidityUser *user);
void validity_db_record_clear (ValidityDbRecord *record);
void validity_record_children_clear (ValidityRecordChildren *children);

/* ================================================================
 * Enrollment commands
 * ================================================================ */

/* cmd 0x69: Create enrollment (start) / End enrollment */
guint8 *validity_db_build_cmd_create_enrollment (gboolean start,
                                                 gsize   *out_len);

/* cmd 0x6B: Enrollment update (with template data) */
guint8 *validity_db_build_cmd_enrollment_update (const guint8 *prev_data,
                                                 gsize         prev_len,
                                                 gsize        *out_len);

/* ================================================================
 * Match commands (inlined at call sites in verify/enroll SSMs)
 * ================================================================ */

/* ================================================================
 * db_write_enable blob access
 * ================================================================ */

typedef struct _FpiDeviceValidity FpiDeviceValidity;

const guint8 *validity_db_get_write_enable_blob (FpiDeviceValidity *self,
                                                 gsize             *out_len);
