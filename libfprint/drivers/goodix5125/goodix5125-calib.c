/*
 * Goodix 27c6:5125 OTP and volatile calibration helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "goodix5125-calib.h"

#include <gio/gio.h>
#include <string.h>

static const guint8 config_template[GOODIX5125_CONFIG_SIZE] = {
  0x70, 0x11, 0x74, 0x85, 0x00, 0x85, 0x2c, 0xb1, 0x18, 0xc9, 0x14, 0xdd, 0x00, 0xdd, 0x00, 0xdd,
  0x00, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
  0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
  0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
  0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
  0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
  0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
  0x40, 0x5c, 0x00, 0x80, 0x01, 0x20, 0x02, 0x08, 0x08, 0x36, 0x02, 0x80, 0x00, 0x38, 0x02, 0x80,
  0x00, 0x3a, 0x02, 0x80, 0x00, 0x2a, 0x01, 0x82, 0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
  0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03,
  0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80,
  0x15, 0x2a, 0x01, 0x08, 0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x19,
  0x00, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x2a, 0x01, 0x08, 0x00, 0x52, 0x00, 0x08,
  0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
};

G_STATIC_ASSERT (G_N_ELEMENTS (config_template) == GOODIX5125_CONFIG_SIZE);

static gboolean
otp_crc_matches (const guint8 *data,
                 gsize         length,
                 guint8        expected)
{
  return goodix5125_proto_otp_crc8 (data, length) == expected;
}

/* Keep the copy loop out of the fortified memcpy inliner. GCC 15 can lose the
 * dynamically validated OTP object size in sanitizer builds and then report
 * valid OTP subranges as zero-sized sources. */
static G_GNUC_NO_INLINE void
append_otp (guint8       *destination,
            const guint8 *otp,
            gsize         start,
            gsize         count)
{
  for (gsize i = 0; i < count; i++)
    destination[i] = otp[start + i];
}

static gboolean
validate_otp_checksums (const guint8 *otp,
                        GError      **error)
{
  guint8 buffer[32];
  gsize length;

#define APPEND_OTP(start, count) \
  G_STMT_START { \
    append_otp (buffer + length, otp, (start), (count)); \
    length += (count); \
  } G_STMT_END

    length = 0;
  APPEND_OTP (0, 11);
  APPEND_OTP (36, 4);
  if (!otp_crc_matches (buffer, length, otp[60]))
    goto invalid;

  length = 0;
  APPEND_OTP (20, 8);
  APPEND_OTP (29, 7);
  APPEND_OTP (40, 10);
  APPEND_OTP (54, 2);
  if (!otp_crc_matches (buffer, length, otp[63]))
    goto invalid;

  length = 0;
  APPEND_OTP (11, 9);
  APPEND_OTP (28, 1);
  APPEND_OTP (50, 4);
  APPEND_OTP (56, 4);
  APPEND_OTP (62, 1);
  if (!otp_crc_matches (buffer, length, otp[61]))
    goto invalid;

#undef APPEND_OTP
  return TRUE;

invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Goodix 5125 OTP checksum validation failed");
  return FALSE;
}

gboolean
goodix5125_otp_parse (const guint8      *otp,
                      gsize              otp_length,
                      Goodix5125OtpInfo *info,
                      GError           **error)
{
  gboolean ft_valid;
  gboolean mt_valid;
  guint8 offset_a;
  guint8 offset_b;
  guint8 offset_c;

  g_return_val_if_fail (info != NULL, FALSE);

  if (!otp || otp_length != GOODIX5125_OTP_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Goodix 5125 OTP must contain exactly 64 bytes");
      return FALSE;
    }
  if (!validate_otp_checksums (otp, error))
    return FALSE;

  ft_valid = otp_crc_matches (otp + 50, 4, otp[62]);
  mt_valid = otp_crc_matches (otp + 46, 4, otp[22]);
  if (!ft_valid && !mt_valid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix 5125 OTP contains no valid DAC group");
      return FALSE;
    }
  memcpy (info->dac, ft_valid ? otp + 50 : otp + 46, sizeof (info->dac));

  if (!otp[42] || otp[43] != (guint8) ~otp[42])
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix 5125 OTP tcode/complement is invalid");
      return FALSE;
    }

  info->tcode = (((otp[42] >> 4) + 1) * 16) + 64;
  info->fdt_delta = (((otp[42] & 0x0f) + 2) * 25600 /
                     (info->tcode * 3)) >> 4;

  offset_a = otp[27] & 0x03;
  offset_b = (otp[27] >> 4) & 0x03;
  offset_c = ((guint8) ~otp[27] >> 2) & 0x03;
  if (offset_a == offset_b || offset_a == offset_c)
    info->fdt_offset = offset_a;
  else if (offset_b == offset_c)
    info->fdt_offset = offset_b;
  else
    info->fdt_offset = 0;

  return TRUE;
}

static gboolean
patch_register (guint8  *config,
                gsize    start,
                gsize    length,
                guint16  reg,
                guint16  value,
                GError **error)
{
  gsize offset;

  if (start > GOODIX5125_CONFIG_SIZE ||
      length > GOODIX5125_CONFIG_SIZE - start)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix 5125 config section is out of range");
      return FALSE;
    }

  for (offset = start; offset + 4 <= start + length; offset += 4)
    {
      guint16 current;

      memcpy (&current, config + offset, sizeof (current));
      if (GUINT16_FROM_LE (current) == reg)
        {
          guint16 little = GUINT16_TO_LE (value);
          memcpy (config + offset + 2, &little, sizeof (little));
          return TRUE;
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "Goodix 5125 config register 0x%04x was not found", reg);
  return FALSE;
}

gboolean
goodix5125_config_build (const Goodix5125OtpInfo *info,
                         guint8                   config[GOODIX5125_CONFIG_SIZE],
                         GError                 **error)
{
  gsize section0_start = config_template[1];
  gsize section0_length = config_template[2];
  gsize section2_start = config_template[5];
  gsize section2_length = config_template[6];
  guint16 value;

  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (config != NULL, FALSE);

  memcpy (config, config_template, sizeof (config_template));
  if (!patch_register (config, section0_start, section0_length,
                       0x005c, info->tcode, error) ||
      !patch_register (config, section0_start, section0_length,
                       0x0220, (info->dac[0] << 4) | 8, error) ||
      !patch_register (config, section0_start, section0_length,
                       0x0236, info->dac[1], error) ||
      !patch_register (config, section0_start, section0_length,
                       0x0238, info->dac[2], error) ||
      !patch_register (config, section0_start, section0_length,
                       0x023a, info->dac[3], error))
    return FALSE;

  value = 0x0080 | ((guint16) info->fdt_delta << 8);
  if (!patch_register (config, section2_start, section2_length,
                       0x0082, value, error))
    return FALSE;

  if (info->fdt_offset &&
      !patch_register (config, section2_start, section2_length,
                       0x0056, 0x2000 | (info->fdt_offset + 4), error))
    return FALSE;

  if (goodix5125_proto_config_checksum_write (config,
                                              GOODIX5125_CONFIG_SIZE) !=
      GOODIX5125_PROTO_OK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix 5125 config checksum calculation failed");
      return FALSE;
    }
  return TRUE;
}
