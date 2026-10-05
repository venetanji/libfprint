/*
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <unistd.h>

#include "drivers/goodix5125/chicago/goodix-chicago-calibration.h"
#include "drivers/goodix5125/chicago/goodix-chicago-template.h"
#include "drivers/goodix5125/chicago/openchicago.h"
#include "drivers/goodix5125/goodix5125-calib.h"
#include "drivers/goodix5125/goodix5125-fdt.h"
#include "drivers/goodix5125/goodix5125-image.h"
#include "drivers/goodix5125/goodix5125-pairing.h"
#include "drivers/goodix5125/goodix5125-proto.h"

/* Synthetic fixture satisfying the documented OTP checksums. It must not be
 * replaced with the OTP from a physical sensor because that contains a stable
 * per-device identifier and factory calibration. */
static const guint8 synthetic_otp[GOODIX5125_OTP_SIZE] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
  0x28, 0x29, 0x44, 0xbb, 0x2c, 0x2d, 0x2e, 0x2f,
  0x30, 0x31, 0x11, 0x22, 0x33, 0x44, 0x36, 0x37,
  0x38, 0x39, 0x3a, 0x3b, 0x68, 0x56, 0x06, 0x36,
};

static void
pack_image_stream (const guint16 *pixels,
                   guint8        *packed)
{
  for (gsize src = 0, dst = 0;
       src < GOODIX5125_IMAGE_N_PIXELS;
       src += 4, dst += 6)
    {
      g_assert_cmpuint (pixels[src], <=, GOODIX5125_IMAGE_MAX_VALUE);
      g_assert_cmpuint (pixels[src + 1], <=, GOODIX5125_IMAGE_MAX_VALUE);
      g_assert_cmpuint (pixels[src + 2], <=, GOODIX5125_IMAGE_MAX_VALUE);
      g_assert_cmpuint (pixels[src + 3], <=, GOODIX5125_IMAGE_MAX_VALUE);

      packed[dst] = ((pixels[src + 1] & 0x0f) << 4) |
                    (pixels[src] >> 8);
      packed[dst + 1] = pixels[src] & 0xff;
      packed[dst + 2] = pixels[src + 2] & 0xff;
      packed[dst + 3] = pixels[src + 1] >> 4;
      packed[dst + 4] = pixels[src + 3] >> 4;
      packed[dst + 5] = ((pixels[src + 3] & 0x0f) << 4) |
                        (pixels[src + 2] >> 8);
    }
}

static void
test_protocol_roundtrip (void)
{
  static const guint8 payload[] = { 0x01, 0x00 };
  guint8 inner[32];
  guint8 outer[64];
  gsize inner_length = 0;
  gsize outer_length = 0;
  Goodix5125InnerFrame inner_frame;
  Goodix5125OuterFrame outer_frame;

  g_assert_cmpint (goodix5125_proto_inner_encode (
                     GOODIX5125_COMMAND_GET_IMAGE,
                     GOODIX5125_CHECKSUM_NORMAL,
                     payload, sizeof (payload), inner, sizeof (inner),
                     &inner_length), ==, GOODIX5125_PROTO_OK);
  g_assert_cmpint (goodix5125_proto_outer_encode (
                     GOODIX5125_OUTER_FLAG_MESSAGE,
                     inner, inner_length, outer, sizeof (outer),
                     &outer_length), ==, GOODIX5125_PROTO_OK);
  g_assert_cmpint (goodix5125_proto_outer_decode (
                     outer, outer_length, &outer_frame), ==,
                   GOODIX5125_PROTO_OK);
  g_assert_cmpint (goodix5125_proto_inner_decode (
                     outer_frame.payload, outer_frame.payload_len,
                     GOODIX5125_CHECKSUM_NORMAL, &inner_frame), ==,
                   GOODIX5125_PROTO_OK);
  g_assert_cmpint (inner_frame.command, ==, GOODIX5125_COMMAND_GET_IMAGE);
  g_assert_cmpmem (inner_frame.payload, inner_frame.payload_len,
                   payload, sizeof (payload));

  g_assert_false (goodix5125_proto_command_is_supported (0xa4));
  g_assert_true (goodix5125_proto_command_is_supported (0xe0));
  g_assert_true (goodix5125_proto_command_is_supported (0xd2));
  g_assert_false (goodix5125_proto_command_is_supported (0xf0));
}

static void
test_pairing_white_box (void)
{
  static const guint8 psk[GOODIX5125_PSK_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  };

  g_autoptr(GBytes) payload = NULL;
  g_autoptr(GError) error = NULL;
  const guint8 *data;
  gsize length = 0;
  guint32 value;

  g_assert_true (goodix5125_pairing_self_test (&error));
  g_assert_no_error (error);
  payload = goodix5125_pairing_build_write_payload (psk, &error);
  g_assert_no_error (error);
  g_assert_nonnull (payload);
  data = g_bytes_get_data (payload, &length);
  g_assert_cmpuint (length, ==, 112);
  memcpy (&value, data, sizeof (value));
  g_assert_cmphex (GUINT32_FROM_LE (value), ==,
                   GOODIX5125_PSK_WB_ADDRESS);
  memcpy (&value, data + 4, sizeof (value));
  g_assert_cmpuint (GUINT32_FROM_LE (value), ==, GOODIX5125_WB_SIZE);
  g_assert_cmpmem (data + 110, 2, ((const guint8[]){ 0, 0 }), 2);
}

static void
test_pairing_hash_reply (void)
{
  guint8 response[9 + GOODIX5125_PSK_SIZE] = { 0 };
  guint8 hash[GOODIX5125_PSK_SIZE];
  guint32 value;
  gboolean present = FALSE;

  g_autoptr(GError) error = NULL;

  value = GUINT32_TO_LE (GOODIX5125_PSK_HASH_ADDRESS);
  memcpy (response + 1, &value, sizeof (value));
  value = GUINT32_TO_LE (GOODIX5125_PSK_SIZE);
  memcpy (response + 5, &value, sizeof (value));
  for (guint i = 0; i < GOODIX5125_PSK_SIZE; i++)
    response[9 + i] = i;

  g_assert_true (goodix5125_pairing_parse_hash_reply (
                   response, sizeof (response), &present, hash, &error));
  g_assert_no_error (error);
  g_assert_true (present);
  g_assert_cmpmem (hash, sizeof (hash), response + 9,
                   GOODIX5125_PSK_SIZE);

  response[0] = 1;
  g_assert_true (goodix5125_pairing_parse_hash_reply (
                   response, 1, &present, hash, &error));
  g_assert_no_error (error);
  g_assert_false (present);
}

static void
test_pairing_state (void)
{
  guint8 psk[GOODIX5125_PSK_SIZE];
  guint8 loaded[GOODIX5125_PSK_SIZE];

  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = NULL;
  g_autofree gchar *path = NULL;
  GStatBuf stat_buffer;

  for (guint i = 0; i < GOODIX5125_PSK_SIZE; i++)
    psk[i] = 0xa0 + i;
  directory = g_dir_make_tmp ("libfprint-goodix5125-pairing-XXXXXX",
                              &error);
  g_assert_no_error (error);
  g_assert_nonnull (directory);
  path = g_build_filename (directory, "psk", NULL);

  g_assert_cmpint (goodix5125_pairing_load_psk (path, loaded, &error), ==,
                   GOODIX5125_PSK_STATE_MISSING);
  g_assert_no_error (error);
  g_assert_true (goodix5125_pairing_save_psk (path, psk, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_stat (path, &stat_buffer), ==, 0);
  g_assert_cmpuint (stat_buffer.st_mode & 0777, ==, 0600);
  g_assert_cmpint (goodix5125_pairing_load_psk (path, loaded, &error), ==,
                   GOODIX5125_PSK_STATE_FOUND);
  g_assert_no_error (error);
  g_assert_cmpmem (loaded, sizeof (loaded), psk, sizeof (psk));

  g_assert_cmpint (g_chmod (path, 0644), ==, 0);
  g_assert_cmpint (goodix5125_pairing_load_psk (path, loaded, &error), ==,
                   GOODIX5125_PSK_STATE_ERROR);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&error);

  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_protocol_nav_fixed_trailer (void)
{
  const guint8 nav_reply[] = {
    GOODIX5125_COMMAND_NAVIGATION, 0x04, 0x00, 0x11, 0x22, 0x33, 0x88,
  };
  Goodix5125InnerFrame decoded = { 0 };

  g_assert_cmpint (goodix5125_proto_inner_decode (
                     nav_reply, sizeof (nav_reply),
                     GOODIX5125_CHECKSUM_TRAILER_88, &decoded),
                   ==, GOODIX5125_PROTO_OK);
  g_assert_cmpint (decoded.command, ==, GOODIX5125_COMMAND_NAVIGATION);
  g_assert_cmpuint (decoded.payload_len, ==, 3);
  g_assert_cmpmem (decoded.payload, decoded.payload_len,
                   nav_reply + GOODIX5125_INNER_HEADER_SIZE, 3);
}

static void
test_otp_and_config (void)
{
  g_autoptr(GError) error = NULL;
  Goodix5125OtpInfo info;
  guint8 config[GOODIX5125_CONFIG_SIZE];

  g_assert_true (goodix5125_otp_parse (synthetic_otp, sizeof (synthetic_otp),
                                       &info, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (info.tcode, ==, 0x0090);
  g_assert_cmpuint (info.fdt_delta, ==, 0x16);
  g_assert_cmpuint (info.fdt_offset, ==, 1);
  g_assert_cmpmem (info.dac, sizeof (info.dac),
                   ((const guint8[]){ 0x11, 0x22, 0x33, 0x44 }), 4);

  g_assert_true (goodix5125_config_build (&info, config, &error));
  g_assert_no_error (error);
  g_assert_cmphex (config[115], ==, 0x90);
  g_assert_cmphex (config[116], ==, 0x00);
  g_assert_cmphex (config[119], ==, 0x18);
  g_assert_cmphex (config[120], ==, 0x01);
  g_assert_cmphex (config[123], ==, 0x22);
  g_assert_cmphex (config[127], ==, 0x33);
  g_assert_cmphex (config[131], ==, 0x44);
  g_assert_cmphex (config[175], ==, 0x80);
  g_assert_cmphex (config[176], ==, 0x16);
  g_assert_cmpint (goodix5125_proto_config_checksum_verify (
                     config, sizeof (config)), ==, GOODIX5125_PROTO_OK);
}

static void
test_image_decode (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *packed = g_malloc0 (GOODIX5125_IMAGE_PACKED_SIZE);
  g_autofree guint16 *pixels = g_new0 (guint16,
                                       GOODIX5125_IMAGE_N_PIXELS);

  memcpy (packed, ((const guint8[]){ 0xc5, 0xab, 0x47,
                                     0x5a, 0x73, 0xb6 }), 6);
  g_assert_true (goodix5125_image_decode (packed,
                                          GOODIX5125_IMAGE_PACKED_SIZE,
                                          pixels,
                                          GOODIX5125_IMAGE_N_PIXELS,
                                          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (pixels[0], ==, 1451);
  g_assert_cmpuint (pixels[1], ==, 1452);
  g_assert_cmpuint (pixels[2], ==, 1607);
  g_assert_cmpuint (pixels[3], ==, 1851);
}

static void
test_image_crc (void)
{
  g_autoptr(GError) error = NULL;
  gsize length = GOODIX5125_IMAGE_PACKED_SIZE + GOODIX5125_IMAGE_CRC_SIZE;
  g_autofree guint8 *packed = g_new (guint8, length);

  /* CRC-32/MPEG-2 0x4407d0f7, stored as the MCU does: d0 f7 44 07 */
  for (gsize i = 0; i < GOODIX5125_IMAGE_PACKED_SIZE; i++)
    packed[i] = (i * 7 + (i >> 8)) & 0xff;
  memcpy (packed + GOODIX5125_IMAGE_PACKED_SIZE,
          ((const guint8[]){ 0xd0, 0xf7, 0x44, 0x07 }), 4);
  g_assert_true (goodix5125_image_check_crc (packed, length, &error));
  g_assert_no_error (error);

  packed[100] ^= 0x01;
  g_assert_false (goodix5125_image_check_crc (packed, length, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  packed[100] ^= 0x01;

  g_assert_false (goodix5125_image_check_crc (packed, length - 1, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_image_decode_transpose (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *packed = g_new (guint8,
                                     GOODIX5125_IMAGE_PACKED_SIZE);
  g_autofree guint16 *stream = g_new (guint16,
                                      GOODIX5125_IMAGE_N_PIXELS);
  g_autofree guint16 *pixels = g_new (guint16,
                                      GOODIX5125_IMAGE_N_PIXELS);

  for (gsize i = 0; i < GOODIX5125_IMAGE_N_PIXELS; i++)
    stream[i] = (i * 37 + 19) & GOODIX5125_IMAGE_MAX_VALUE;
  pack_image_stream (stream, packed);

  g_assert_true (goodix5125_image_decode_transposed (
                   packed,
                   GOODIX5125_IMAGE_PACKED_SIZE,
                   pixels,
                   GOODIX5125_IMAGE_N_PIXELS,
                   &error));
  g_assert_no_error (error);

  for (gsize i = 0; i < GOODIX5125_IMAGE_N_PIXELS; i++)
    g_assert_cmpuint (pixels[(i % GOODIX5125_IMAGE_STREAM_WIDTH) *
                             GOODIX5125_IMAGE_WIDTH +
                             i / GOODIX5125_IMAGE_STREAM_WIDTH],
                      ==,
                      stream[i]);
}

static void
test_fdt_bases (void)
{
  const guint8 reply[] = {
    0x02, 0x00, 0x15, 0x00,
    0x22, 0x01, 0x7f, 0x01, 0xbe, 0x00,
    0x5e, 0x01, 0xcf, 0x00, 0x56, 0x01,
  };
  const guint8 expected_down[] = {
    0x80, 0x91, 0x80, 0xbf, 0x80, 0x5f,
    0x80, 0xaf, 0x80, 0x67, 0x80, 0xab,
  };

  g_autoptr(GError) error = NULL;
  guint8 base[GOODIX5125_FDT_BASE_SIZE];
  guint8 bad_reply[sizeof (reply)];
  gboolean finger_off = TRUE;

  g_assert_true (goodix5125_fdt_reply_is_finger_off (
                   reply, sizeof (reply), &finger_off, &error));
  g_assert_no_error (error);
  g_assert_false (finger_off);

  g_assert_true (goodix5125_fdt_down_base_from_reply (
                   reply, sizeof (reply), base, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (base, sizeof (base), expected_down,
                   sizeof (expected_down));

  /* a reply with an invalid channel leaves the bases untouched */
  memcpy (bad_reply, reply, sizeof (bad_reply));
  bad_reply[10] = 0x00;
  bad_reply[11] = 0x00;
  g_assert_false (goodix5125_fdt_down_base_from_reply (
                    bad_reply, sizeof (bad_reply), base, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpmem (base, sizeof (base), expected_down,
                   sizeof (expected_down));
}

static void
test_chicago_calibration_persistence (void)
{
  static const guint8 packed_template_data[] = { 0x51, 0x25, 0xaa, 0x55 };

  g_autoptr(GBytes) calibration = NULL;
  g_autoptr(GBytes) loaded = NULL;
  g_autoptr(GBytes) packed_template = NULL;
  g_autoptr(GBytes) parsed_template = NULL;
  g_autoptr(GVariant) print_data = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = NULL;
  guint16 image_base[GOODIX_CHICAGO_PIXELS];
  guint8 wrong_sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN];
  gint fd;

  for (guint i = 0; i < G_N_ELEMENTS (image_base); i++)
    image_base[i] = 1800 + i % 700;

  calibration = goodix_chicago_calibration_generate (image_base);
  g_assert_nonnull (calibration);
  g_assert_true (goodix_chicago_calibration_validate_payload (calibration,
                                                              &error));
  g_assert_no_error (error);

  fd = g_file_open_tmp ("libfprint-goodix5125-calibration-XXXXXX",
                        &path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);

  g_assert_true (goodix_chicago_calibration_save (
                   path, synthetic_otp, calibration, &error));
  g_assert_no_error (error);
  {
    GStatBuf stat_buffer;

    g_assert_cmpint (g_stat (path, &stat_buffer), ==, 0);
    g_assert_cmpuint (stat_buffer.st_mode & 0777, ==, 0600);
  }
  loaded = goodix_chicago_calibration_load (path, synthetic_otp, &error);
  g_assert_no_error (error);
  g_assert_nonnull (loaded);
  g_assert_true (g_bytes_equal (calibration, loaded));

  memcpy (wrong_sensor_id, synthetic_otp, sizeof (wrong_sensor_id));
  wrong_sensor_id[0] ^= 0xff;
  g_assert_null (goodix_chicago_calibration_load (
                   path, wrong_sensor_id, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&error);

  packed_template = g_bytes_new_static (packed_template_data,
                                        sizeof (packed_template_data));
  print_data = goodix_chicago_print_data_build (
    synthetic_otp, calibration, packed_template);
  g_assert_nonnull (print_data);
  g_assert_true (goodix_chicago_print_data_parse (
                   print_data, synthetic_otp, calibration, &parsed_template,
                   &error));
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (packed_template, parsed_template));

  g_clear_pointer (&parsed_template, g_bytes_unref);
  g_assert_false (goodix_chicago_print_data_parse (
                    print_data, wrong_sensor_id, calibration,
                    &parsed_template, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  g_assert_cmpint (g_unlink (path), ==, 0);
}

static gsize
make_message (Goodix5125OuterFlag    flag,
              Goodix5125Command      command,
              const guint8          *payload,
              gsize                  payload_len,
              Goodix5125ChecksumMode checksum_mode,
              guint8                *out,
              gsize                  out_size)
{
  guint8 inner[64];
  gsize inner_len = 0;
  gsize written = 0;

  /* the encoder only emits the 0x88 trailer for NOP requests */
  g_assert_cmpint (goodix5125_proto_inner_encode (command,
                                                  GOODIX5125_CHECKSUM_NORMAL,
                                                  payload, payload_len, inner,
                                                  sizeof (inner), &inner_len),
                   ==, GOODIX5125_PROTO_OK);
  if (checksum_mode == GOODIX5125_CHECKSUM_TRAILER_88)
    inner[inner_len - 1] = 0x88;
  g_assert_cmpint (goodix5125_proto_outer_encode (flag, inner, inner_len, out,
                                                  out_size, &written),
                   ==, GOODIX5125_PROTO_OK);
  return written;
}

static void
classify (const guint8          *buffer,
          gsize                  len,
          Goodix5125Command      command,
          Goodix5125OuterFlag    reply_flag,
          Goodix5125ChecksumMode reply_mode,
          Goodix5125ProtoResult  expected_result,
          Goodix5125RxKind       expected_kind,
          gsize                  expected_len)
{
  Goodix5125RxKind kind;
  const guint8 *payload;
  gsize payload_len;

  g_assert_cmpint (goodix5125_proto_classify_rx (buffer, len, command,
                                                 reply_flag, reply_mode,
                                                 &kind, &payload,
                                                 &payload_len),
                   ==, expected_result);
  if (expected_result != GOODIX5125_PROTO_OK)
    return;
  g_assert_cmpint (kind, ==, expected_kind);
  g_assert_cmpuint (payload_len, ==, expected_len);
}

/* 27c6:5125: the ACK is optional, stale messages are skipped. */
static void
test_protocol_classify_rx (void)
{
  static const guint8 ack_ok[] = { GOODIX5125_COMMAND_QUERY_STATE, 0x01 };
  static const guint8 ack_fail[] = { GOODIX5125_COMMAND_QUERY_STATE, 0x00 };
  static const guint8 ack_other[] = { GOODIX5125_COMMAND_FDT_DOWN, 0x03 };
  static const guint8 state[14] = { 1, 2, 3 };
  static const guint8 fdt[16] = { 0 };
  static const guint8 nav[4] = { 9, 8, 7, 6 };
  guint8 buffer[128];
  gsize len;

  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_COMMAND_ACK,
                      ack_ok, sizeof (ack_ok), GOODIX5125_CHECKSUM_NORMAL,
                      buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_QUERY_STATE,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
            GOODIX5125_PROTO_OK, GOODIX5125_RX_ACK, 0);

  /* lost ACK: the reply comes first */
  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE,
                      GOODIX5125_COMMAND_QUERY_STATE, state, sizeof (state),
                      GOODIX5125_CHECKSUM_NORMAL, buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_QUERY_STATE,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
            GOODIX5125_PROTO_OK, GOODIX5125_RX_REPLY, sizeof (state));

  /* a late FDT-down reply and its ACK while waiting for the MCU state */
  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE,
                      GOODIX5125_COMMAND_FDT_DOWN, fdt, sizeof (fdt),
                      GOODIX5125_CHECKSUM_NORMAL, buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_QUERY_STATE,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
            GOODIX5125_PROTO_OK, GOODIX5125_RX_SKIP, 0);
  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_COMMAND_ACK,
                      ack_other, sizeof (ack_other), GOODIX5125_CHECKSUM_NORMAL,
                      buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_QUERY_STATE,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
            GOODIX5125_PROTO_OK, GOODIX5125_RX_SKIP, 0);

  /* a failed ACK of this command is an error */
  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_COMMAND_ACK,
                      ack_fail, sizeof (ack_fail), GOODIX5125_CHECKSUM_NORMAL,
                      buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_QUERY_STATE,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
            GOODIX5125_PROTO_INVALID_ACK, GOODIX5125_RX_SKIP, 0);

  /* TLS image: the outer payload is the reply; a message is not */
  {
    static const guint8 record[5] = { 0x17, 0x03, 0x03, 0x00, 0x00 };
    gsize written = 0;

    g_assert_cmpint (goodix5125_proto_outer_encode (
                       GOODIX5125_OUTER_FLAG_TLS, record, sizeof (record),
                       buffer, sizeof (buffer), &written),
                     ==, GOODIX5125_PROTO_OK);
    classify (buffer, written, GOODIX5125_COMMAND_GET_IMAGE,
              GOODIX5125_OUTER_FLAG_TLS, GOODIX5125_CHECKSUM_NORMAL,
              GOODIX5125_PROTO_OK, GOODIX5125_RX_REPLY, sizeof (record));
    classify (buffer, written, GOODIX5125_COMMAND_QUERY_STATE,
              GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
              GOODIX5125_PROTO_OK, GOODIX5125_RX_SKIP, 0);
  }

  /* NAV reply with the fixed 0x88 trailer */
  len = make_message (GOODIX5125_OUTER_FLAG_MESSAGE,
                      GOODIX5125_COMMAND_NAVIGATION, nav, sizeof (nav),
                      GOODIX5125_CHECKSUM_TRAILER_88, buffer, sizeof (buffer));
  classify (buffer, len, GOODIX5125_COMMAND_NAVIGATION,
            GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_TRAILER_88,
            GOODIX5125_PROTO_OK, GOODIX5125_RX_REPLY, sizeof (nav));
}

static guint32
state_crc32 (const guint8 *data, gsize length)
{
  guint32 crc = G_MAXUINT32;

  for (gsize i = 0; i < length; i++)
    {
      crc ^= data[i];
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
  return ~crc;
}

/* Set a u32 in a section of a saved openchicago state and fix its CRC. */
static GBytes *
state_patch_u32 (GBytes *state, const gchar tag[4], gsize offset,
                 guint32 value)
{
  gsize length;
  const guint8 *data = g_bytes_get_data (state, &length);
  guint8 *copy = g_memdup2 (data, length);
  guint32 le = GUINT32_TO_LE (value);
  gsize position = 8;

  while (position + 8 <= length - 4)
    {
      guint32 section = copy[position + 4] | copy[position + 5] << 8 |
                        copy[position + 6] << 16 |
                        (guint32) copy[position + 7] << 24;

      if (memcmp (copy + position, tag, 4) == 0)
        {
          g_assert_cmpuint (offset + 4, <=, section);
          memcpy (copy + position + 8 + offset, &le, 4);
          break;
        }
      position += 8 + section;
    }
  g_assert_cmpuint (position + 8, <=, length - 4);
  le = GUINT32_TO_LE (state_crc32 (copy, length - 4));
  memcpy (copy + length - 4, &le, 4);
  return g_bytes_new_take (copy, length);
}

static void
test_chicago_state_bounds (void)
{
  /* The state file is only checksummed: counters used as indices or
   * divisors must be range-checked on load. */
  static const struct
  {
    const gchar *tag;
    gsize        offset;
    guint32      value;
  } cases[] = {
    { "FEAT", 4, G_MAXUINT32 },  /* history_count, indexes history[] */
    { "FEAT", 4, 4 },
    { "PREP", 0, G_MAXUINT32 },  /* framenum, divisor (n + 1) */
  };
  guint16 image_base[OC_FRAME_PIXELS];

  g_autoptr(OcSession) session = NULL;
  g_autoptr(OcSession) restored = NULL;
  g_autoptr(GBytes) state = NULL;
  g_autoptr(GError) error = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (image_base); i++)
    image_base[i] = 1800 + i % 700;
  session = oc_session_new (image_base, &error);
  g_assert_no_error (error);
  state = oc_session_save_state (session);

  restored = oc_session_new_from_state (state, &error);
  g_assert_no_error (error);
  g_assert_nonnull (restored);

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(GBytes) broken = state_patch_u32 (state, cases[i].tag,
                                                  cases[i].offset,
                                                  cases[i].value);
      g_autoptr(OcSession) rejected = NULL;

      rejected = oc_session_new_from_state (broken, &error);
      g_assert_null (rejected);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_clear_error (&error);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix5125/protocol/roundtrip", test_protocol_roundtrip);
  g_test_add_func ("/goodix5125/protocol/nav-fixed-trailer",
                   test_protocol_nav_fixed_trailer);
  g_test_add_func ("/goodix5125/protocol/classify-rx",
                   test_protocol_classify_rx);
  g_test_add_func ("/goodix5125/pairing/white-box",
                   test_pairing_white_box);
  g_test_add_func ("/goodix5125/pairing/hash-reply",
                   test_pairing_hash_reply);
  g_test_add_func ("/goodix5125/pairing/state",
                   test_pairing_state);
  g_test_add_func ("/goodix5125/calibration/otp-config", test_otp_and_config);
  g_test_add_func ("/goodix5125/image/decode", test_image_decode);
  g_test_add_func ("/goodix5125/image/decode-transpose",
                   test_image_decode_transpose);
  g_test_add_func ("/goodix5125/image/crc", test_image_crc);
  g_test_add_func ("/goodix5125/fdt/bases", test_fdt_bases);
  g_test_add_func ("/goodix5125/chicago/calibration-persistence",
                   test_chicago_calibration_persistence);
  g_test_add_func ("/goodix5125/chicago/state-bounds",
                   test_chicago_state_bounds);
  return g_test_run ();
}
