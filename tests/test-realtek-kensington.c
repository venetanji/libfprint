/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <glib.h>
#include <string.h>

#include "../libfprint/drivers/realtek/realtek-kensington.h"

static void
test_uuid (void)
{
  guint8 uuid[16] = {0};
  const guint8 expected[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x4d, 0xef,
                             0x80, 0x12, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

  g_assert_true (realtek_parse_host_uuid ("12345678-9abc-4def-8012-334455667788", uuid));
  g_assert_cmpmem (uuid, sizeof uuid, expected, sizeof expected);
  g_assert_false (realtek_parse_host_uuid ("00000000-0000-0000-0000-000000000000", uuid));
  g_assert_false (realtek_parse_host_uuid ("ffffffff-ffff-ffff-ffff-ffffffffffff", uuid));
  g_assert_false (realtek_parse_host_uuid ("not-a-uuid", uuid));
}

static void
test_records (void)
{
  guint8 records[3 * 52] = {0};
  guint8 uuid[16] = {1, 2, 3, 4};
  guint8 index = 0xff;

  g_assert_true (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  g_assert_cmpuint (index, ==, 0);

  records[0] = 1;
  records[2] = 0xff;
  records[3] = 'F';
  records[35] = 7;
  memcpy (records + 36, uuid, sizeof uuid);
  records[2 * 52] = 1;
  records[2 * 52 + 2] = 0xff;
  records[2 * 52 + 3] = 'F';
  records[2 * 52 + 35] = 7;
  memcpy (records + 2 * 52 + 36, uuid, sizeof uuid);
  g_assert_true (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  g_assert_cmpuint (index, ==, 7);

  g_assert_false (realtek_host_index_from_records (records, sizeof records - 1, 3, uuid, &index));
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 4, uuid, &index));

  records[2 * 52 + 2] = 0x12;
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  records[2 * 52 + 2] = 0xff;
  records[2 * 52 + 3] = 0;
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  records[2 * 52 + 2] = 0xf5;
  g_assert_true (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  records[2 * 52 + 2] = 0xff;
  records[2 * 52 + 3] = 'F';

  records[2 * 52 + 35] = 8;
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  records[2 * 52 + 35] = 7;
  records[2 * 52 + 36] ^= 1;
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
  records[2 * 52 + 36] ^= 1;
  records[0] = 2;
  g_assert_false (realtek_host_index_from_records (records, sizeof records, 3, uuid, &index));
}

static void
test_commit (void)
{
  guint8 payload[49] = {0};
  guint8 uuid[16] = {1, 2, 3, 4};

  memset (payload, 0xa5, 32);
  realtek_fill_host_bound_commit (payload, 7, uuid);
  for (guint i = 0; i < 32; i++)
    g_assert_cmpuint (payload[i], ==, 0xa5);
  g_assert_cmpuint (payload[32], ==, 7);
  g_assert_cmpmem (payload + 33, 16, uuid, sizeof uuid);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/realtek/kensington/uuid", test_uuid);
  g_test_add_func ("/realtek/kensington/records", test_records);
  g_test_add_func ("/realtek/kensington/commit", test_commit);
  return g_test_run ();
}
