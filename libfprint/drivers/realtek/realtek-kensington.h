/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <string.h>

#define REALTEK_HOST_RECORD_SIZE 52
#define REALTEK_HOST_UUID_SIZE 16
#define REALTEK_HOST_COMMIT_SIZE 49

/* sysfs exposes the canonical SMBIOS UUID, rather than mixed-endian bytes. */
static gboolean
realtek_parse_host_uuid (const gchar *text, guint8 uuid[REALTEK_HOST_UUID_SIZE])
{
  guint8 any = 0, all = 0xff;
  guint digit = 0;

  if (!text || !g_uuid_string_is_valid (text))
    return FALSE;

  for (guint i = 0; text[i]; i++)
    {
      if (text[i] == '-')
        continue;

      if (!(digit & 1))
        uuid[digit / 2] = g_ascii_xdigit_value (text[i]) << 4;
      else
        uuid[digit / 2] |= g_ascii_xdigit_value (text[i]);
      digit++;
    }

  for (guint i = 0; i < 16; i++)
    {
      any |= uuid[i];
      all &= uuid[i];
    }
  return any != 0 && all != 0xff;
}

/* 52-byte records: occupancy, common 35-byte header/ID, host index, UUID. */
static gboolean
realtek_host_index_from_records (const guint8 *records, gsize length, guint count,
                                 const guint8 uuid[REALTEK_HOST_UUID_SIZE], guint8 *index)
{
  gboolean seen = FALSE;

  if (!records || !index || !uuid || count > length / REALTEK_HOST_RECORD_SIZE)
    return FALSE;

  *index = 0;
  for (guint i = 0; i < count; i++)
    {
      const guint8 *record = records + i * REALTEK_HOST_RECORD_SIZE;

      if (record[0] == 0)
        continue;
      if (record[2] != 0xff && record[2] != 0xf5)
        return FALSE;
      if (record[2] == 0xff && (record[3] == 0 || record[3] == ' '))
        return FALSE;
      if (record[0] != 1 || memcmp (record + 36, uuid, 16) != 0 ||
          (seen && record[35] != *index))
        return FALSE;
      *index = record[35];
      seen = TRUE;
    }
  return TRUE;
}

/* The common commit is 32 bytes; the host-bound variant appends an index
 * and a canonical UUID. The ID is kept separate from host identity. */
static void
realtek_fill_host_bound_commit (guint8 payload[REALTEK_HOST_COMMIT_SIZE], guint8 index,
                                const guint8 uuid[REALTEK_HOST_UUID_SIZE])
{
  payload[32] = index;
  memcpy (payload + 33, uuid, 16);
}
