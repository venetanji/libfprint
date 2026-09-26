/*
 * Database operations for Validity/Synaptics VCSFW sensors
 *
 * Implements on-chip template database management: command builders,
 * response parsers, identity encoding, and finger data formatting.
 *
 * Reference: python-validity db.py, flash.py, sensor.py
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

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity_pack.h"
#include "fpi-byte-utils.h"
#include "validity_db.h"
#include "validity.h"
#include "validity_hal.h"
#include "vcsfw_protocol.h"

/* ================================================================
 * Structure cleanup helpers
 * ================================================================ */

void
validity_db_info_clear (ValidityDbInfo *info)
{
  g_clear_pointer (&info->roots, g_free);
  memset (info, 0, sizeof (*info));
}

void
validity_user_storage_clear (ValidityUserStorage *storage)
{
  g_clear_pointer (&storage->name, g_free);
  g_clear_pointer (&storage->user_dbids, g_free);
  g_clear_pointer (&storage->user_val_sizes, g_free);
  memset (storage, 0, sizeof (*storage));
}

void
validity_match_result_clear (ValidityMatchResult *result)
{
  g_clear_pointer (&result->hash, g_free);
  memset (result, 0, sizeof (*result));
}

void
validity_user_clear (ValidityUser *user)
{
  g_clear_pointer (&user->identity, g_free);
  g_clear_pointer (&user->fingers, g_free);
  memset (user, 0, sizeof (*user));
}

void
validity_db_record_clear (ValidityDbRecord *record)
{
  g_clear_pointer (&record->value, g_free);
  memset (record, 0, sizeof (*record));
}

void
validity_record_children_clear (ValidityRecordChildren *children)
{
  g_clear_pointer (&children->children, g_free);
  memset (children, 0, sizeof (*children));
}

/* ================================================================
 * Command builders
 *
 * Each function allocates and returns a binary command payload.
 * The caller must g_free() the result.
 * ================================================================ */

/* cmd 0x4B: Get user storage
 * Format: 0x4B | dbid(2LE) | name_len(2LE) | name(NUL-terminated) */
guint8 *
validity_db_build_cmd_get_user_storage (const gchar *name,
                                        gsize       *out_len)
{
  gsize name_len = 0;

  if (name && name[0] != '\0')
    name_len = strlen (name) + 1; /* include NUL terminator */

  return validity_pack_new (out_len, "bhhd",
                            VCSFW_CMD_GET_USER_STORAGE,
                            0,                            /* dbid = 0 (lookup by name) */
                            (int) name_len,
                            (const guint8 *) name, name_len);
}

/* cmd 0x4A: Get user by dbid
 * Format: 0x4A | dbid(2LE) | 0(2LE) | 0(2LE) */
guint8 *
validity_db_build_cmd_get_user (guint16 dbid,
                                gsize  *out_len)
{
  return validity_pack_new (out_len, "bhhh",
                            VCSFW_CMD_GET_USER, dbid, 0, 0);
}

/* cmd 0x47: New record
 * Format: 0x47 | parent(2LE) | type(2LE) | storage(2LE) | data_len(2LE) | data */
guint8 *
validity_db_build_cmd_new_record (guint16       parent,
                                  guint16       type,
                                  guint16       storage,
                                  const guint8 *data,
                                  gsize         data_len,
                                  gsize        *out_len)
{
  return validity_pack_new (out_len, "bhhhhd",
                            VCSFW_CMD_NEW_RECORD,
                            (int) parent, (int) type,
                            (int) storage, (int) data_len,
                            data, data_len);
}

/* cmd 0x48: Delete record
 * Format: 0x48 | dbid(2LE) */
guint8 *
validity_db_build_cmd_del_record (guint16 dbid,
                                  gsize  *out_len)
{
  return validity_pack_new (out_len, "bh",
                            VCSFW_CMD_DEL_RECORD, dbid);
}

/* cmd 0x1a: Call cleanups (commit pending writes) */
guint8 *
validity_db_build_cmd_call_cleanups (gsize *out_len)
{
  return validity_pack_new (out_len, "b", 0x1a);
}

/* cmd 0x45: Query DB partition info (total/used/free bytes, record count). */
guint8 *
validity_db_build_cmd_db_info (gsize *out_len)
{
  return validity_pack_new (out_len, "b", VCSFW_CMD_DB_INFO);
}

/* cmd 0x3f: Erase a flash partition. Destructive -- reclaims the space held by
 * deleted-but-uncompacted DB records (the chip does not auto-compact after a
 * del_record, so the partition fills with 0x04b5). Must be bracketed by
 * db_write_enable + call_cleanups. Format: 0x3f | partition(1). */
guint8 *
validity_db_build_cmd_erase_flash (guint8 partition,
                                   gsize *out_len)
{
  return validity_pack_new (out_len, "bb",
                            VCSFW_CMD_ERASE_FLASH, partition);
}

/* cmd 0x69: Create enrollment / End enrollment
 * Format: 0x69 | flag(4LE)
 * start=TRUE: flag=1, start=FALSE: flag=0 */
guint8 *
validity_db_build_cmd_create_enrollment (gboolean start,
                                         gsize   *out_len)
{
  return validity_pack_new (out_len, "bw",
                            VCSFW_CMD_CREATE_ENROLLMENT,
                            (guint32) (start ? 1 : 0));
}

/* cmd 0x6B: Enrollment update (with template data)
 * Format: 0x6B | prev_data */
guint8 *
validity_db_build_cmd_enrollment_update (const guint8 *prev_data,
                                         gsize         prev_len,
                                         gsize        *out_len)
{
  return validity_pack_new (out_len, "bd",
                            VCSFW_CMD_ENROLLMENT_UPDATE,
                            prev_data, prev_len);
}


/* ================================================================
 * Response parsers
 *
 * All data parameters point PAST the 2-byte status.
 * ================================================================ */

/* Parse DB info response (cmd 0x45) */
gboolean
validity_db_parse_info (const guint8   *data,
                        gsize           data_len,
                        ValidityDbInfo *out)
{
  FpiByteReader reader;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof (*out));
  fpi_byte_reader_init (&reader, data, data_len);

  /* Header: unknown1(4LE) unknown0(4LE) total(4LE) used(4LE) free(4LE) records(2LE) nroots(2LE) = 0x18 bytes */
  if (data_len < 0x18)
    return FALSE;

  if (!fpi_byte_reader_get_uint32_le (&reader, &out->unknown1))
    return FALSE;
  if (!fpi_byte_reader_get_uint32_le (&reader, &out->unknown0))
    return FALSE;
  if (!fpi_byte_reader_get_uint32_le (&reader, &out->total))
    return FALSE;
  if (!fpi_byte_reader_get_uint32_le (&reader, &out->used))
    return FALSE;
  if (!fpi_byte_reader_get_uint32_le (&reader, &out->free_space))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->records))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->n_roots))
    return FALSE;

  /* Root record IDs */
  if (out->n_roots > 0)
    {
      out->roots = g_new0 (guint16, out->n_roots);
      for (guint16 i = 0; i < out->n_roots; i++)
        {
          if (!fpi_byte_reader_get_uint16_le (&reader, &out->roots[i]))
            {
              validity_db_info_clear (out);
              return FALSE;
            }
        }
    }

  return TRUE;
}

/* Parse user storage response (cmd 0x4B)
 * Format: recid(2LE) usercnt(2LE) namesz(2LE) unknown(2LE)
 *         usrtab[usercnt * 4 bytes] name[namesz bytes] */
gboolean
validity_db_parse_user_storage (const guint8        *data,
                                gsize                data_len,
                                ValidityUserStorage *out)
{
  FpiByteReader reader;
  guint16 name_sz, unknown;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof (*out));
  fpi_byte_reader_init (&reader, data, data_len);

  /* Header: 8 bytes */
  if (data_len < 8)
    return FALSE;

  if (!fpi_byte_reader_get_uint16_le (&reader, &out->dbid))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->user_count))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &name_sz))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &unknown))
    return FALSE;

  /* User table: 4 bytes per entry (dbid(2LE) + value_size(2LE)) */
  if (out->user_count > 0)
    {
      out->user_dbids = g_new0 (guint16, out->user_count);
      out->user_val_sizes = g_new0 (guint16, out->user_count);

      for (guint16 i = 0; i < out->user_count; i++)
        {
          if (!fpi_byte_reader_get_uint16_le (&reader, &out->user_dbids[i]) ||
              !fpi_byte_reader_get_uint16_le (&reader, &out->user_val_sizes[i]))
            {
              validity_user_storage_clear (out);
              return FALSE;
            }
        }
    }

  /* Name */
  if (name_sz > 0)
    {
      const guint8 *name_data;
      if (!fpi_byte_reader_get_data (&reader, name_sz, &name_data))
        {
          validity_user_storage_clear (out);
          return FALSE;
        }
      out->name = g_strndup ((const gchar *) name_data, name_sz);
    }

  return TRUE;
}

/* Parse user response (cmd 0x4A)
 * Format: recid(2LE) fingercnt(2LE) unknown(2LE) identitysz(2LE)
 *         fingertab[8 * fingercnt] identity[identitysz] */
gboolean
validity_db_parse_user (const guint8 *data,
                        gsize         data_len,
                        ValidityUser *out)
{
  FpiByteReader reader;
  guint16 unknown, identity_sz;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof (*out));
  fpi_byte_reader_init (&reader, data, data_len);

  /* Header: 8 bytes */
  if (data_len < 8)
    return FALSE;

  if (!fpi_byte_reader_get_uint16_le (&reader, &out->dbid))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->finger_count))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &unknown))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &identity_sz))
    return FALSE;

  /* Finger table: 8 bytes per entry */
  if (out->finger_count > 0)
    {
      out->fingers = g_new0 (ValidityFingerEntry, out->finger_count);
      for (guint16 i = 0; i < out->finger_count; i++)
        {
          if (!fpi_byte_reader_get_uint16_le (&reader, &out->fingers[i].dbid) ||
              !fpi_byte_reader_get_uint16_le (&reader, &out->fingers[i].subtype) ||
              !fpi_byte_reader_get_uint16_le (&reader, &out->fingers[i].storage) ||
              !fpi_byte_reader_get_uint16_le (&reader, &out->fingers[i].value_size))
            {
              validity_user_clear (out);
              return FALSE;
            }
        }
    }

  /* Identity bytes */
  if (identity_sz > 0)
    {
      const guint8 *id_data;
      if (!fpi_byte_reader_get_data (&reader, identity_sz, &id_data))
        {
          validity_user_clear (out);
          return FALSE;
        }
      out->identity = g_memdup2 (id_data, identity_sz);
      out->identity_len = identity_sz;
    }

  return TRUE;
}

/* Parse record value response (cmd 0x49)
 * Format: dbid(2LE) type(2LE) storage(2LE) sz(2LE) pad(2bytes) value[sz] */
gboolean
validity_db_parse_record_value (const guint8     *data,
                                gsize             data_len,
                                ValidityDbRecord *out)
{
  FpiByteReader reader;
  guint16 sz;
  guint16 pad;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof (*out));
  fpi_byte_reader_init (&reader, data, data_len);

  /* Header: 10 bytes (python-validity: unpack('<xxHHHHxx', rsp[:12])
   * but our data is already past status, so: HHHHxx = 10 bytes) */
  if (data_len < 10)
    return FALSE;

  if (!fpi_byte_reader_get_uint16_le (&reader, &out->dbid))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->type))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->storage))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &sz))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &pad))
    return FALSE;

  /* Value data */
  if (sz > 0)
    {
      const guint8 *val_data;
      if (!fpi_byte_reader_get_data (&reader, sz, &val_data))
        {
          validity_db_record_clear (out);
          return FALSE;
        }
      out->value = g_memdup2 (val_data, sz);
      out->value_len = sz;
    }

  return TRUE;
}

/* Parse record children response (cmd 0x46)
 * Format: dbid(2LE) type(2LE) storage(2LE) sz(2LE) cnt(2LE) pad(2bytes)
 *         children[cnt * 4 bytes: dbid(2LE) type(2LE)] */
gboolean
validity_db_parse_record_children (const guint8           *data,
                                   gsize                   data_len,
                                   ValidityRecordChildren *out)
{
  FpiByteReader reader;
  guint16 sz, pad;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  memset (out, 0, sizeof (*out));
  fpi_byte_reader_init (&reader, data, data_len);

  /* Header: 12 bytes (python-validity: unpack('<xxHHHHHxx', rsp[:14])
   * past status: HHHH H xx = 12 bytes) */
  if (data_len < 12)
    return FALSE;

  if (!fpi_byte_reader_get_uint16_le (&reader, &out->dbid))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->type))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->storage))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &sz))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &out->child_count))
    return FALSE;
  if (!fpi_byte_reader_get_uint16_le (&reader, &pad))
    return FALSE;

  /* Child entries */
  if (out->child_count > 0)
    {
      out->children = g_new0 (ValidityRecordChild, out->child_count);
      for (guint16 i = 0; i < out->child_count; i++)
        {
          if (!fpi_byte_reader_get_uint16_le (&reader, &out->children[i].dbid) ||
              !fpi_byte_reader_get_uint16_le (&reader, &out->children[i].type))
            {
              validity_record_children_clear (out);
              return FALSE;
            }
        }
    }

  return TRUE;
}

/* Parse new record response (cmd 0x47)
 * Format: record_id(2LE) */
gboolean
validity_db_parse_new_record_id (const guint8 *data,
                                 gsize         data_len,
                                 guint16      *out_record_id)
{
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (out_record_id != NULL, FALSE);

  return validity_unpack (data, data_len, "h", out_record_id);
}

/* ================================================================
 * Identity helpers
 * ================================================================ */

/* Build an identity blob from an id string for the sensor DB.
 *
 * python-validity uses Windows SIDs. For libfprint we store the print's
 * generated id string, encoded as a type-3 (SID-type) identity with
 * zero-padding to the Windows union minimum size of 0x4C bytes.
 *
 * Format: type(4LE) | len(4LE) | id_bytes(len) | padding */
guint8 *
validity_db_build_identity (const gchar *id_str,
                            gsize       *out_len)
{
  FpiByteWriter writer;
  gsize id_len;
  gsize payload_len;
  gsize total_len;

  g_return_val_if_fail (id_str != NULL, NULL);

  id_len = strlen (id_str);
  payload_len = 4 + 4 + id_len;
  total_len = MAX (payload_len, VALIDITY_IDENTITY_MIN_SIZE);

  fpi_byte_writer_init_with_size (&writer, total_len, FALSE);
  fpi_byte_writer_put_uint32_le (&writer, VALIDITY_IDENTITY_TYPE_SID);
  fpi_byte_writer_put_uint32_le (&writer, id_len);
  fpi_byte_writer_put_data (&writer, (const guint8 *) id_str, id_len);
  /* Pad to minimum identity size */
  if (total_len > payload_len)
    fpi_byte_writer_fill (&writer, 0, total_len - payload_len);

  *out_len = fpi_byte_writer_get_pos (&writer);
  return fpi_byte_writer_reset_and_get_data (&writer);
}

/* Extract the id bytes from a user record's identity blob, the inverse of
 * validity_db_build_identity(): type(4LE) | len(4LE) | id_bytes(len).
 * @out_id points into @user->identity. Returns FALSE if the record has no
 * identity, or it is malformed or empty. The bytes are not validated as
 * UTF-8 or NUL-free; compare them by length and content. */
gboolean
validity_db_user_get_id (const ValidityUser *user,
                         const guint8      **out_id,
                         gsize              *out_id_len)
{
  FpiByteReader reader;
  guint32 id_type, id_len;
  const guint8 *id;

  g_return_val_if_fail (user != NULL, FALSE);
  g_return_val_if_fail (out_id != NULL && out_id_len != NULL, FALSE);

  if (!user->identity)
    return FALSE;

  fpi_byte_reader_init (&reader, user->identity, user->identity_len);
  if (!fpi_byte_reader_get_uint32_le (&reader, &id_type) ||
      !fpi_byte_reader_get_uint32_le (&reader, &id_len) ||
      id_len == 0 ||
      !fpi_byte_reader_get_data (&reader, id_len, &id))
    return FALSE;

  *out_id = id;
  *out_id_len = id_len;
  return TRUE;
}

/* Build finger data for new_finger (from python-validity make_finger_data)
 *
 * Format:
 *   subtype(2LE) | flags=3(2LE) | tinfo_len(2LE) | 0x20(2LE)
 *   tag1=1(2LE) | len(2LE) | template_data
 *   tag2=2(2LE) | len(2LE) | tid
 *   padding[0x20 bytes] */
guint8 *
validity_db_build_finger_data (guint16       subtype,
                               const guint8 *template_data,
                               gsize         template_len,
                               const guint8 *tid,
                               gsize         tid_len,
                               gsize        *out_len)
{
  FpiByteWriter writer;
  gsize template_block = 4 + template_len;  /* tag(2) + len(2) + data */
  gsize tid_block = 4 + tid_len;
  gsize tinfo_len = template_block + tid_block;
  gsize total = 8 + tinfo_len + 0x20;  /* header + data + padding */

  fpi_byte_writer_init_with_size (&writer, total, FALSE);

  /* Header */
  fpi_byte_writer_put_uint16_le (&writer, subtype);
  fpi_byte_writer_put_uint16_le (&writer, 3);  /* flags */
  fpi_byte_writer_put_uint16_le (&writer, tinfo_len);
  fpi_byte_writer_put_uint16_le (&writer, 0x20);

  /* Template block */
  fpi_byte_writer_put_uint16_le (&writer, 1);  /* tag */
  fpi_byte_writer_put_uint16_le (&writer, template_len);
  if (template_len > 0)
    fpi_byte_writer_put_data (&writer, template_data, template_len);

  /* TID block */
  fpi_byte_writer_put_uint16_le (&writer, 2);  /* tag */
  fpi_byte_writer_put_uint16_le (&writer, tid_len);
  if (tid_len > 0)
    fpi_byte_writer_put_data (&writer, tid, tid_len);

  /* Padding */
  fpi_byte_writer_fill (&writer, 0, 0x20);

  *out_len = fpi_byte_writer_get_pos (&writer);
  return fpi_byte_writer_reset_and_get_data (&writer);
}

/* ================================================================
 * db_write_enable blob access
 * ================================================================ */

const guint8 *
validity_db_get_write_enable_blob (FpiDeviceValidity *self, gsize *out_len)
{
  return validity_data_get_bytes (&self->device_data,
                                  VALIDITY_DATA_DB_WRITE_ENABLE,
                                  out_len);
}
