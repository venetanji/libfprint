/*
 * Goodix 27c6:5125 dynamic finger-detection bases
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "goodix5125-fdt.h"

#include <gio/gio.h>

#define GOODIX5125_FDT_REPLY_SIZE 16
#define GOODIX5125_FDT_CHANNELS 6
#define GOODIX5125_FDT_DEFAULT_UNTOUCHED 0x13

static guint16
read_le16 (const guint8 *data)
{
  return data[0] | ((guint16) data[1] << 8);
}

static gboolean
validate_reply (const guint8 *reply,
                gsize         reply_length,
                GError      **error)
{
  if (!reply || reply_length < GOODIX5125_FDT_REPLY_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix FDT reply is shorter than 16 bytes");
      return FALSE;
    }
  return TRUE;
}

gboolean
goodix5125_fdt_reply_is_finger_off (const guint8 *reply,
                                    gsize         reply_length,
                                    gboolean     *finger_off,
                                    GError      **error)
{
  g_return_val_if_fail (finger_off != NULL, FALSE);

  if (!validate_reply (reply, reply_length, error))
    return FALSE;

  *finger_off = (read_le16 (reply + 2) &
                 ((1U << GOODIX5125_FDT_CHANNELS) - 1)) == 0;
  return TRUE;
}

static gboolean
write_channel (guint    channel,
               guint    value,
               guint8   output[GOODIX5125_FDT_BASE_SIZE],
               GError **error)
{
  value &= 0xff;
  if (value == 0 || value == 0xff)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Goodix FDT channel %u has invalid threshold 0x%02x",
                   channel, value);
      return FALSE;
    }

  output[channel * 2] = 0x80;
  output[channel * 2 + 1] = value;
  return TRUE;
}

gboolean
goodix5125_fdt_down_base_from_reply (
  const guint8 *reply,
  gsize         reply_length,
  guint8        output[GOODIX5125_FDT_BASE_SIZE],
  GError      **error)
{
  g_return_val_if_fail (output != NULL, FALSE);

  if (!validate_reply (reply, reply_length, error))
    return FALSE;

  for (guint channel = 0; channel < GOODIX5125_FDT_CHANNELS; channel++)
    {
      guint value = read_le16 (reply + 4 + channel * 2) >> 1;

      if (!write_channel (channel, value, output, error))
        return FALSE;
    }
  return TRUE;
}

gboolean
goodix5125_fdt_up_base_from_reply (
  const guint8 *reply,
  gsize         reply_length,
  guint8        delta,
  guint8        output[GOODIX5125_FDT_BASE_SIZE],
  GError      **error)
{
  guint16 touch_flags;
  guint untouched = delta >= 2 ? delta - 2 : GOODIX5125_FDT_DEFAULT_UNTOUCHED;

  g_return_val_if_fail (output != NULL, FALSE);

  if (!validate_reply (reply, reply_length, error))
    return FALSE;

  touch_flags = read_le16 (reply + 2);
  for (guint channel = 0; channel < GOODIX5125_FDT_CHANNELS; channel++)
    {
      guint value;

      if (touch_flags & (1U << channel))
        value = (read_le16 (reply + 4 + channel * 2) >> 1) + delta;
      else
        value = untouched;

      if (!write_channel (channel, value, output, error))
        return FALSE;
    }
  return TRUE;
}
