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

/* The first parameter's length and data */
#define PARAM0_LEN (CV2_HEADER_LEN + 4)
#define PARAM0_DATA (CV2_HEADER_LEN + CV2_PARAM_HEADER_LEN)

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

/* From the Windows driver's enrollment (windows-captures/03.pcap) */
static const char SENSOR_RESET_REQ[] =
  "0100000038000000820040000200000000000000000000000000000000000000"
  "00000000000000000c000000000000000400000000000000";

static const char CAPTURE_ID[] = "ac27493d858230b6ace063b560297f594315db27";
static const char FINAL_ID[] = "593f0a9481c697258513c8a921687629e44542f0";

static const char OPEN_REPLY[] =
  "010000003800000002004005020000008c9c0114000000000000000000000000"
  "00000000000000000c00000000000000040000008c9c0114";

static const char CAPTURE_START_REPLY[] =
  "010000004800000066004205020000008c9c0114000000000000000000000000"
  "00000000000000001c0000000000000014000000ac27493d858230b6ace063b5"
  "60297f594315db27";

static const char CAPTURE_PENDING_REPLY[] =
  "010000002c00000066004205020000008c9c0114850000000000000000000000"
  "000000000000000000000000";

static const char SAMPLE_REPLY[] =
  "01000000600000006c004005020000008c9c0114000000000000000000000000"
  "0000000000000000340000000000000004000000009c01140000000014000000"
  "ac27493d858230b6ace063b560297f594315db27000000000400000000000000";

static const char SAMPLE_DONE_REPLY[] =
  "01000000600000006c004005020000008c9c0114000000000000000000000000"
  "0000000000000000340000000000000004000000019c01140000000014000000"
  "593f0a9481c697258513c8a921687629e44542f0000000000400000000000000";

static const char SAMPLE_REJECTED_REPLY[] =
  "010000002c0000006c00400502000000889c0114590000000000000000000000"
  "000000000000000000000000";

static const char COMMIT_REPLY[] =
  "01000000400000006e004005020000008c9c0114000000000000000000000000"
  "00000000000000001400000003000000000000000000000004000000805a7800"
  "00";

static const char MATCH_HIT_REPLY[] =
  "01000000440000002f004205020000008c9c0114000000000000000000000000"
  "0000000000000000180000000000000004000000010000000000000004000000"
  "07a0df00";

static const char MATCH_MISS_REPLY[] =
  "01000000440000002f004205020000008c9c0114000000000000000000000000"
  "0000000000000000180000000000000004000000000000000000000004000000"
  "00000000";

static const char MATCH_EMPTY_REPLY[] =
  "010000002c0000002f004205020000008c9c0114470000000000000000000000"
  "000000000000000000000000";

static const char CANCEL_REPLY[] =
  "010000002c000000680040050200000000000000000000000000000000000000"
  "000000000000000000000000";

static const char VERSION_REPLY[] =
  "0100000050030000390040010200000000000000000000000000000000000000"
  "000000000000000024030000010000001c0300005553485f4348495049443a30"
  "353831303231310a5553485f434849505f5355425f5645523a30303030303538"
  "310a5553485f52454c5f5645523a30313032303030300a5553485f52454c5f55"
  "5047524144455f5645523a30303431323031350a5553485f52454c5f4255494c"
  "445f5645523a30303030303030300a5553485f424f4f545f434d443a35383830"
  "303130300a5553485f5245424f4f545f434e543a30303030303030300a555348"
  "5f5342495f4d4f44453a30303030303030300a5553485f464c534857525f434e"
  "543a30303030303030300a5553485f4f54505f53544154533a30303030303030"
  "310a5553485f435553545f49443a36303030303030310a5553485f435553545f"
  "5245563a30303030303030300a5553485f5343445f524f4d5f5645523a303130"
  "31303630300a5553485f43565f5645523a30313030303233350a5553485f4343"
  "49445f5645523a30303033303130310a5553485f4849445f5645523a30303030"
  "303032630a5553485f44505f5645525f55505045523a30303035303030300a55"
  "53485f44505f5645525f4c4f5745523a30303033303365300a5553485f525341"
  "5f5645523a30303030303130310a5553485f49505f52455649443a3030303030"
  "3030300a5553485f5057525f49444c455f54494d453a30303034393365300a55"
  "53485f5057525f4d4f44453a30303030303030330a5553485f555342445f5049"
  "443a30303030353833340a5553485f5359535f49443a30303030303030320a55"
  "53485f46505f4e4558545f53454e534f523a30303031306130310a5553485f46"
  "505f44505f53454e534f523a30303030303030300a5553485f46505f53594e41"
  "50544943535f53454e534f523a30303030303030300a5553485f4145535f4b45"
  "595f56414c49443a30303030303030310a5553485f424f4152445f49443a6263"
  "3030303030300a5553485f494d475f535441543a30303030303030300a555348"
  "5f494d475f464c4147533a30303030303037650a5553485f4355525f5449434b"
  "3a30303034323430320a5553485f4255494c445f54494d453a41756720313820"
  "323032352031373a35343a34300a0000";

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
  assert_message (cv2_build_simple (CV2_CMD_SENSOR_RESET), SENSOR_RESET_REQ);
}

static GByteArray *
hex_array (const char *hex)
{
  g_autoptr(GBytes) bytes = hex_bytes (hex);
  gsize len;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  GByteArray *array = g_byte_array_sized_new (len);

  g_byte_array_append (array, data, len);
  return array;
}

static Cv2Reply *
parse_hex (const char *hex, guint16 cmd, GError **error)
{
  g_autoptr(GBytes) bytes = hex_bytes (hex);
  gsize len;
  const guint8 *data = g_bytes_get_data (bytes, &len);

  return cv2_reply_parse (data, len, cmd, error);
}

static Cv2Reply *
parse_array (GByteArray *array, guint16 cmd, GError **error)
{
  return cv2_reply_parse (array->data, array->len, cmd, error);
}

static void
assert_id (const guint8 *id, const char *expected_hex)
{
  g_autoptr(GBytes) expected = hex_bytes (expected_hex);
  gsize len;
  const guint8 *data = g_bytes_get_data (expected, &len);

  g_assert_cmpmem (id, CV2_CAPTURE_ID_LEN, data, len);
}

static void
test_parse_open (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) reply = parse_hex (OPEN_REPLY, CV2_CMD_OPEN_SESSION, &error);
  guint32 session = 0;

  g_assert_no_error (error);
  g_assert_cmpuint (reply->cmd, ==, CV2_CMD_OPEN_SESSION);
  g_assert_cmpuint (reply->flags, ==, 0x0540);
  g_assert_cmpuint (reply->status, ==, 0);
  g_assert_cmpuint (reply->session, ==, SESSION);
  g_assert_cmpuint (reply->params->len, ==, 1);
  g_assert_true (cv2_reply_get_session (reply, &session, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (session, ==, SESSION);
}

static void
test_parse_capture_and_samples (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) start = parse_hex (CAPTURE_START_REPLY, CV2_CMD_CAPTURE_START, &error);
  g_autoptr(Cv2Reply) sample = NULL;
  g_autoptr(Cv2Reply) done_reply = NULL;
  guint8 id[CV2_CAPTURE_ID_LEN];
  gboolean done = TRUE;

  g_assert_no_error (error);
  g_assert_true (cv2_reply_get_capture_id (start, id, &error));
  assert_id (id, CAPTURE_ID);

  sample = parse_hex (SAMPLE_REPLY, CV2_CMD_ENROLL_SAMPLE, &error);
  g_assert_no_error (error);
  g_assert_true (cv2_reply_get_sample (sample, &done, id, &error));
  g_assert_false (done);
  assert_id (id, CAPTURE_ID);

  done_reply = parse_hex (SAMPLE_DONE_REPLY, CV2_CMD_ENROLL_SAMPLE, &error);
  g_assert_no_error (error);
  g_assert_true (cv2_reply_get_sample (done_reply, &done, id, &error));
  g_assert_true (done);
  assert_id (id, FINAL_ID);
}

static void
test_parse_commit_with_trailing_byte (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) reply = parse_hex (COMMIT_REPLY, CV2_CMD_ENROLL_COMMIT, &error);
  guint32 template_id = 0;

  g_assert_no_error (error);
  g_assert_cmpuint (g_bytes_get_size (reply->raw), ==, 0x40);
  g_assert_true (cv2_reply_get_template (reply, &template_id, &error));
  g_assert_cmphex (template_id, ==, 0x00785a80);
}

static void
test_parse_match (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) hit = parse_hex (MATCH_HIT_REPLY, CV2_CMD_MATCH, &error);
  g_autoptr(Cv2Reply) miss = NULL;
  gboolean matched = FALSE;
  guint32 template_id = 0;

  g_assert_no_error (error);
  g_assert_true (cv2_reply_get_match (hit, &matched, &template_id, &error));
  g_assert_true (matched);
  g_assert_cmphex (template_id, ==, 0x00dfa007);

  miss = parse_hex (MATCH_MISS_REPLY, CV2_CMD_MATCH, &error);
  g_assert_no_error (error);
  g_assert_true (cv2_reply_get_match (miss, &matched, &template_id, &error));
  g_assert_false (matched);
  g_assert_cmphex (template_id, ==, 0);
}

static void
test_parse_header_only (void)
{
  struct
  {
    const char *hex;
    guint16     cmd;
    guint32     status;
  } cases[] = {
    { SAMPLE_REJECTED_REPLY, CV2_CMD_ENROLL_SAMPLE, CV2_STATUS_SAMPLE_REJECTED },
    { CAPTURE_PENDING_REPLY, CV2_CMD_CAPTURE_START, CV2_STATUS_CAPTURE_PENDING },
    { MATCH_EMPTY_REPLY, CV2_CMD_MATCH, CV2_STATUS_NO_TEMPLATES },
    { CANCEL_REPLY, CV2_CMD_CAPTURE_CANCEL, CV2_STATUS_OK },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(Cv2Reply) reply = parse_hex (cases[i].hex, cases[i].cmd, &error);

      g_assert_no_error (error);
      g_assert_cmphex (reply->status, ==, cases[i].status);
      g_assert_cmpuint (reply->params->len, ==, 0);
    }
}

static void
test_parse_version (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) reply = parse_hex (VERSION_REPLY, CV2_CMD_GET_VERSION, &error);
  g_autofree gchar *text = NULL;

  g_assert_no_error (error);
  text = cv2_reply_get_version_text (reply, &error);
  g_assert_no_error (error);
  g_assert_true (g_str_has_prefix (text, "USH_CHIPID:05810211\n"));
  g_assert_nonnull (strstr (text, "\nUSH_REL_UPGRADE_VER:00412015\n"));
  g_assert_true (g_str_has_suffix (text, "USH_BUILD_TIME:Aug 18 2025 17:54:40\n"));
}

static void
assert_proto_error (GError *error)
{
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
}

static void
test_parse_malformed (void)
{
  g_autoptr(GByteArray) msg = NULL;
  g_autoptr(GError) error = NULL;

  /* Truncated header */
  msg = hex_array (OPEN_REPLY);
  g_byte_array_set_size (msg, 0x20);
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Header length larger than the bytes read */
  msg = hex_array (OPEN_REPLY);
  g_byte_array_set_size (msg, msg->len - 4);
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Bad magic */
  msg = hex_array (OPEN_REPLY);
  msg->data[CV2_HDR_MAGIC] = 2;
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Reply for another command */
  g_assert_null (parse_hex (OPEN_REPLY, CV2_CMD_CLOSE_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);

  /* Parameter area length disagrees with the message length */
  msg = hex_array (OPEN_REPLY);
  msg->data[CV2_HDR_PARAMS_LEN] = 0x10;
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Parameter overruns the message */
  msg = hex_array (OPEN_REPLY);
  msg->data[PARAM0_LEN] = 0x40;
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Parameter length near G_MAXUINT32: must not wrap when padded (32-bit) */
  msg = hex_array (OPEN_REPLY);
  msg->data[PARAM0_LEN] = 0xfd;
  msg->data[PARAM0_LEN + 1] = 0xff;
  msg->data[PARAM0_LEN + 2] = 0xff;
  msg->data[PARAM0_LEN + 3] = 0xff;
  g_assert_null (parse_array (msg, CV2_CMD_OPEN_SESSION, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);

  /* Truncated parameter header: 4 bytes of parameter area */
  msg = hex_array (CANCEL_REPLY);
  g_byte_array_set_size (msg, msg->len + 4);
  memset (msg->data + CV2_HEADER_LEN, 0, 4);
  msg->data[CV2_HDR_LEN] = CV2_HEADER_LEN + 4;
  msg->data[CV2_HDR_PARAMS_LEN] = 0x04;
  g_assert_null (parse_array (msg, CV2_CMD_CAPTURE_CANCEL, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_clear_pointer (&msg, g_byte_array_unref);
}

static void
test_getters_reject_wrong_shape (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) start = parse_hex (CAPTURE_START_REPLY, CV2_CMD_CAPTURE_START, NULL);
  g_autoptr(Cv2Reply) open = parse_hex (OPEN_REPLY, CV2_CMD_OPEN_SESSION, NULL);
  g_autoptr(Cv2Reply) hit = NULL;
  g_autoptr(GByteArray) msg = NULL;
  gboolean matched;
  guint32 value;
  gchar *text;

  /* 0:20 where 0:4 is expected */
  g_assert_false (cv2_reply_get_session (start, &value, &error));
  assert_proto_error (error);
  g_clear_error (&error);

  /* Missing parameter */
  g_assert_false (cv2_reply_get_template (open, &value, &error));
  assert_proto_error (error);
  g_clear_error (&error);

  /* Not a string parameter */
  text = cv2_reply_get_version_text (open, &error);
  g_assert_null (text);
  assert_proto_error (error);
  g_clear_error (&error);

  /* Match flag other than 0 or 1 */
  msg = hex_array (MATCH_HIT_REPLY);
  msg->data[PARAM0_DATA] = 2;
  hit = parse_array (msg, CV2_CMD_MATCH, NULL);
  g_assert_false (cv2_reply_get_match (hit, &matched, &value, &error));
  assert_proto_error (error);
}

static void
test_peek_cmd (void)
{
  g_autoptr(GBytes) open = hex_bytes (OPEN_REPLY);
  gsize len;
  const guint8 *data = g_bytes_get_data (open, &len);
  guint16 cmd = 0;

  g_assert_true (cv2_message_peek_cmd (data, len, &cmd));
  g_assert_cmpuint (cmd, ==, CV2_CMD_OPEN_SESSION);
  g_assert_false (cv2_message_peek_cmd (data, 10, &cmd));
}

static void
test_int_event (void)
{
  g_autoptr(GBytes) reply_ready = hex_bytes ("0000000050000000");
  g_autoptr(GBytes) finger = hex_bytes ("0300000000000000");
  g_autoptr(GBytes) short_touch = hex_bytes ("0300000007000000");
  g_autoptr(GBytes) too_long = hex_bytes ("0000000065100000");
  g_autoptr(GBytes) too_short = hex_bytes ("000000000c000000");
  g_autoptr(GBytes) unknown = hex_bytes ("0700000001000000");
  g_autoptr(GError) error = NULL;
  Cv2IntEvent event;

  g_assert_true (cv2_int_event_parse (g_bytes_get_data (reply_ready, NULL), 8, &event, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (event.type, ==, CV2_INT_REPLY);
  g_assert_cmpuint (event.value, ==, 0x50);

  g_assert_true (cv2_int_event_parse (g_bytes_get_data (finger, NULL), 8, &event, &error));
  g_assert_cmpuint (event.type, ==, CV2_INT_FINGER);
  g_assert_cmpuint (event.value, ==, 0);

  /* The finger was lifted too soon */
  g_assert_true (cv2_int_event_parse (g_bytes_get_data (short_touch, NULL), 8, &event, &error));
  g_assert_cmpuint (event.type, ==, CV2_INT_FINGER);
  g_assert_cmpuint (event.value, ==, 7);

  /* Unknown types are for the caller to reject */
  g_assert_true (cv2_int_event_parse (g_bytes_get_data (unknown, NULL), 8, &event, &error));
  g_assert_cmpuint (event.type, ==, 7);
  g_assert_no_error (error);

  /* A reply length larger than the reply buffer, or smaller than a header */
  g_assert_false (cv2_int_event_parse (g_bytes_get_data (too_long, NULL), 8, &event, &error));
  assert_proto_error (error);
  g_clear_error (&error);
  g_assert_false (cv2_int_event_parse (g_bytes_get_data (too_short, NULL), 8, &event, &error));
  assert_proto_error (error);
  g_clear_error (&error);

  /* Too short to hold a type and a value */
  g_assert_false (cv2_int_event_parse (g_bytes_get_data (finger, NULL), 7, &event, &error));
  assert_proto_error (error);
}

static void
test_status_classify (void)
{
  const struct
  {
    guint16    cmd;
    guint32    status;
    Cv2Outcome outcome;
    GQuark     (*domain) (void);  /* NULL: no error expected */
    gint       code;
  } cases[] = {
    { CV2_CMD_OPEN_SESSION, 0x00, CV2_OUTCOME_OK, NULL, 0 },
    { CV2_CMD_ENROLL_SAMPLE, 0x59, CV2_OUTCOME_RETRY, fp_device_retry_quark, FP_DEVICE_RETRY_GENERAL },
    { CV2_CMD_CAPTURE_START, 0x85, CV2_OUTCOME_CAPTURE_PENDING, fp_device_error_quark, FP_DEVICE_ERROR_BUSY },
    { CV2_CMD_MATCH, 0x47, CV2_OUTCOME_NO_MATCH, NULL, 0 },
    { CV2_CMD_ENROLL_COMMIT, 0x24, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_NOT_SUPPORTED },
    { CV2_CMD_DELETE, 0x13, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_GENERAL },
    { CV2_CMD_ENROLL_COMMIT, 0x8d, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_PROTO },
    { CV2_CMD_ENROLL_SAMPLE, 0x85, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_PROTO },
    { CV2_CMD_CAPTURE_START, 0x47, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_PROTO },
    { CV2_CMD_DELETE, 0x1b, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_DATA_NOT_FOUND },
    { CV2_CMD_MATCH, 0x1b, CV2_OUTCOME_ERROR, fp_device_error_quark, FP_DEVICE_ERROR_DATA_NOT_FOUND },
    { CV2_CMD_ENROLL_SAMPLE, 0x89, CV2_OUTCOME_RETRY, fp_device_retry_quark, FP_DEVICE_RETRY_GENERAL },
    { CV2_CMD_MATCH, 0x89, CV2_OUTCOME_RETRY, fp_device_retry_quark, FP_DEVICE_RETRY_GENERAL },
    { CV2_CMD_CLOSE_SESSION, 0x00, CV2_OUTCOME_OK, NULL, 0 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(GError) error = NULL;
      Cv2Outcome outcome = cv2_status_classify (cases[i].cmd, cases[i].status, &error);

      g_assert_cmpint (outcome, ==, cases[i].outcome);
      if (cases[i].domain)
        g_assert_error (error, cases[i].domain (), cases[i].code);
      else
        g_assert_no_error (error);
    }
}

static void
test_version_lookup (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Cv2Reply) reply = parse_hex (VERSION_REPLY, CV2_CMD_GET_VERSION, &error);
  g_autofree gchar *text = cv2_reply_get_version_text (reply, &error);
  g_autofree gchar *upgrade = cv2_version_lookup (text, "USH_REL_UPGRADE_VER");
  g_autofree gchar *chip = cv2_version_lookup (text, "USH_CHIPID");
  g_autofree gchar *build = cv2_version_lookup (text, "USH_BUILD_TIME");
  g_autofree gchar *missing = cv2_version_lookup (text, "USH_NOPE");
  g_autofree gchar *prefix = cv2_version_lookup (text, "USH_REL");
  g_autofree gchar *reordered = cv2_version_lookup ("B:2\nUSH_REL_UPGRADE_VER:00412001\nA:1",
                                                    "USH_REL_UPGRADE_VER");
  g_autofree gchar *crlf = cv2_version_lookup ("USH_REL_UPGRADE_VER:00412015\r\n",
                                               "USH_REL_UPGRADE_VER");
  g_autofree gchar *garbage = cv2_version_lookup ("\x01\x02\x03", "USH_REL_UPGRADE_VER");

  g_assert_cmpstr (upgrade, ==, "00412015");
  g_assert_cmpstr (chip, ==, "05810211");
  g_assert_cmpstr (build, ==, "Aug 18 2025 17:54:40");
  g_assert_null (missing);
  g_assert_null (prefix);
  g_assert_cmpstr (reordered, ==, "00412001");
  g_assert_cmpstr (crlf, ==, "00412015");
  g_assert_null (garbage);
}

static void
test_firmware_gate (void)
{
  g_autoptr(GError) old = cv2_firmware_error ("00412001");
  g_autoptr(GError) unknown = cv2_firmware_error (NULL);

  g_assert_cmpint (cv2_firmware_enroll_support ("00412001"), ==, CV2_FW_TOO_OLD);
  g_assert_cmpint (cv2_firmware_enroll_support ("00412015"), ==, CV2_FW_OK);
  g_assert_cmpint (cv2_firmware_enroll_support ("00412100"), ==, CV2_FW_OK);
  g_assert_cmpint (cv2_firmware_enroll_support (NULL), ==, CV2_FW_UNKNOWN);
  g_assert_cmpint (cv2_firmware_enroll_support (""), ==, CV2_FW_UNKNOWN);
  g_assert_cmpint (cv2_firmware_enroll_support ("0041201"), ==, CV2_FW_UNKNOWN);
  g_assert_cmpint (cv2_firmware_enroll_support ("zz412015"), ==, CV2_FW_UNKNOWN);

  g_assert_error (old, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (old->message, "00412001"));
  g_assert_nonnull (strstr (old->message, "Dell"));
  g_assert_error (unknown, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cv2/build/requests", test_build_requests);
  g_test_add_func ("/cv2/parse/open", test_parse_open);
  g_test_add_func ("/cv2/parse/capture-and-samples", test_parse_capture_and_samples);
  g_test_add_func ("/cv2/parse/commit-trailing-byte", test_parse_commit_with_trailing_byte);
  g_test_add_func ("/cv2/parse/match", test_parse_match);
  g_test_add_func ("/cv2/parse/header-only", test_parse_header_only);
  g_test_add_func ("/cv2/parse/version", test_parse_version);
  g_test_add_func ("/cv2/parse/malformed", test_parse_malformed);
  g_test_add_func ("/cv2/parse/wrong-shape", test_getters_reject_wrong_shape);
  g_test_add_func ("/cv2/parse/peek-cmd", test_peek_cmd);
  g_test_add_func ("/cv2/parse/int-event", test_int_event);
  g_test_add_func ("/cv2/status/classify", test_status_classify);
  g_test_add_func ("/cv2/firmware/version-lookup", test_version_lookup);
  g_test_add_func ("/cv2/firmware/gate", test_firmware_gate);

  return g_test_run ();
}
