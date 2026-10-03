/*
 * Broadcom ControlVault2 (BCM5880) fingerprint driver: protocol unit tests
 *
 * Copyright (C) 2026 Matt Trent <matt@thetrents.org>
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

#include <string.h>

#include "fpi-device.h"
#include "cv2-proto.h"

#define SESSION 0x14019c8c

static const char OPEN_REQ[] =
  "0100000060000000020040040200000000000000000000000000000000000000"
  "0000000000000000340000000000000004000000440000000100000007000000"
  "6d7941707049440001000000080000006d795573657249440100000000000000";

static const char CLOSE_REQ[] =
  "0100000038000000040040040200000000000000000000000000000000000000"
  "00000000000000000c00000000000000040000008c9c0114";

static const char BEGIN_REQ[] =
  "01000000380000008a0040040200000000000000000000000000000000000000"
  "00000000000000000c000000000000000400000000000000";

static const char CAPTURE_START_REQ[] =
  "010000005000000066004204020000008c9c0114000000000000000000000000"
  "00000000000000002400000000000000040000008c9c01140000000004000000"
  "02000000000000000400000023000000";

static const char CANCEL_REQ[] =
  "0100000038000000680040040200000000000000000000000000000000000000"
  "00000000000000000c000000000000000400000000000000";

static const char SAMPLE_REQ[] =
  "010000005c0000006c004004020000008c9c0114000000000000000000000000"
  "00000000000000003000000000000000040000008c9c01140000000014000000"
  "ac27493d858230b6ace063b560297f594315db270200000000000000";

static const char COMMIT_REQ[] =
  "010000006c0000006e004004020000008c9c0114000000000000000000000000"
  "00000000000000004000000000000000040000008c9c01140000000014000000"
  "593f0a9481c697258513c8a921687629e44542f0020000000000000002000000"
  "000000000300000000000000";

static const char DISCARD_REQ[] =
  "01000000380000006d0040040200000000000000000000000000000000000000"
  "00000000000000000c000000000000000400000000000000";

static const char MATCH1_REQ[] =
  "01000000680000002f004204020000008c9c0114000000000000000000000000"
  "00000000000000003c00000000000000040000008c9c01140000000004000000"
  "480000000000000004000000e253000000000000040000000000000002000000"
  "0400000007a0df00";

static const char MATCH2_REQ[] =
  "010000006c0000002f004204020000008c9c0114000000000000000000000000"
  "00000000000000004000000000000000040000008c9c01140000000004000000"
  "480000000000000004000000e253000000000000040000000000000002000000"
  "080000002f6236001c835b00";

static const char DELETE_REQ[] =
  "010000004c0000000a004204020000008c9c0114000000000000000000000000"
  "00000000000000002000000000000000040000008c9c01140000000004000000"
  "2f6236000200000000000000";

static const char VERSION_REQ[] =
  "0100000038000000390040040200000000000000000000000000000000000000"
  "00000000000000000c000000000000000400000000000000";

static const char CAPTURE_ID[] = "ac27493d858230b6ace063b560297f594315db27";
static const char FINAL_ID[] = "593f0a9481c697258513c8a921687629e44542f0";

static GBytes *
hex_bytes (const char *hex)
{
  gsize len = strlen (hex) / 2;
  guint8 *buf = g_malloc (len);

  g_assert_cmpuint (strlen (hex) % 2, ==, 0);
  for (gsize i = 0; i < len; i++)
    buf[i] = (g_ascii_xdigit_value (hex[2 * i]) << 4) |
             g_ascii_xdigit_value (hex[2 * i + 1]);

  return g_bytes_new_take (buf, len);
}

static void
assert_message (GByteArray *message, const char *expected_hex)
{
  g_autoptr(GByteArray) owned = message;
  g_autoptr(GBytes) expected = hex_bytes (expected_hex);
  gsize len;
  const guint8 *data = g_bytes_get_data (expected, &len);

  g_assert_cmpmem (owned->data, owned->len, data, len);
}

static void
test_build_requests (void)
{
  g_autoptr(GBytes) capture_id = hex_bytes (CAPTURE_ID);
  g_autoptr(GBytes) final_id = hex_bytes (FINAL_ID);
  const guint32 one[] = { 0x00dfa007 };
  const guint32 two[] = { 0x0036622f, 0x005b831c };

  assert_message (cv2_build_open_session (), OPEN_REQ);
  assert_message (cv2_build_close_session (SESSION), CLOSE_REQ);
  assert_message (cv2_build_simple (CV2_CMD_ENROLL_BEGIN), BEGIN_REQ);
  assert_message (cv2_build_capture_start (SESSION), CAPTURE_START_REQ);
  assert_message (cv2_build_simple (CV2_CMD_CAPTURE_CANCEL), CANCEL_REQ);
  assert_message (cv2_build_enroll_sample (SESSION, g_bytes_get_data (capture_id, NULL)),
                  SAMPLE_REQ);
  assert_message (cv2_build_enroll_commit (SESSION, g_bytes_get_data (final_id, NULL)),
                  COMMIT_REQ);
  assert_message (cv2_build_simple (CV2_CMD_ENROLL_DISCARD), DISCARD_REQ);
  assert_message (cv2_build_match (SESSION, one, G_N_ELEMENTS (one)), MATCH1_REQ);
  assert_message (cv2_build_match (SESSION, two, G_N_ELEMENTS (two)), MATCH2_REQ);
  assert_message (cv2_build_delete (SESSION, 0x0036622f), DELETE_REQ);
  assert_message (cv2_build_simple (CV2_CMD_GET_VERSION), VERSION_REQ);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cv2/build/requests", test_build_requests);

  return g_test_run ();
}
