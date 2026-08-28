/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <unistd.h>

#include "drivers/goodix5125/chicago/goodix-chicago-calibration.h"
#include "drivers/goodix5125/chicago/goodix-chicago-template.h"
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
  const guint8 expected_up[] = {
    0x80, 0xae, 0x80, 0x1b, 0x80, 0x7c,
    0x80, 0x1b, 0x80, 0x84, 0x80, 0x1b,
  };

  g_autoptr(GError) error = NULL;
  guint8 base[GOODIX5125_FDT_BASE_SIZE];
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

  g_assert_true (goodix5125_fdt_up_base_from_reply (
                   reply, sizeof (reply), 0x1d, base, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (base, sizeof (base), expected_up, sizeof (expected_up));
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix5125/protocol/roundtrip", test_protocol_roundtrip);
  g_test_add_func ("/goodix5125/protocol/nav-fixed-trailer",
                   test_protocol_nav_fixed_trailer);
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
  g_test_add_func ("/goodix5125/fdt/bases", test_fdt_bases);
  g_test_add_func ("/goodix5125/chicago/calibration-persistence",
                   test_chicago_calibration_persistence);
  return g_test_run ();
}
