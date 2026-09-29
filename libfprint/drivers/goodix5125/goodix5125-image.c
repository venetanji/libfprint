/*
 * Goodix 27c6:5125 raw image decoder
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "goodix5125-image.h"

#include <gio/gio.h>

G_STATIC_ASSERT (GOODIX5125_IMAGE_PACKED_SIZE / 6 * 4 ==
                 GOODIX5125_IMAGE_N_PIXELS);
G_STATIC_ASSERT (GOODIX5125_IMAGE_STREAM_WIDTH * GOODIX5125_IMAGE_STREAM_HEIGHT ==
                 GOODIX5125_IMAGE_N_PIXELS);
G_STATIC_ASSERT (GOODIX5125_IMAGE_WIDTH * GOODIX5125_IMAGE_HEIGHT ==
                 GOODIX5125_IMAGE_N_PIXELS);

static gboolean
check_buffer (gconstpointer buffer,
              gsize         actual_len,
              gsize         expected_len,
              const char   *name,
              GError      **error)
{
  if (!buffer)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s buffer is NULL", name);
      return FALSE;
    }

  if (actual_len != expected_len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s buffer has %" G_GSIZE_FORMAT " elements, expected %"
                   G_GSIZE_FORMAT,
                   name, actual_len, expected_len);
      return FALSE;
    }

  return TRUE;
}

gboolean
goodix5125_image_decode (const guint8 *packed,
                         gsize         packed_len,
                         guint16      *pixels,
                         gsize         pixels_len,
                         GError      **error)
{
  gsize src;
  gsize dst;

  if (!check_buffer (packed, packed_len, GOODIX5125_IMAGE_PACKED_SIZE,
                     "Packed image", error) ||
      !check_buffer (pixels, pixels_len, GOODIX5125_IMAGE_N_PIXELS,
                     "Decoded image", error))
    return FALSE;

  for (src = 0, dst = 0; src < packed_len; src += 6, dst += 4)
    {
      pixels[dst] = ((packed[src] & 0x0f) << 8) | packed[src + 1];
      pixels[dst + 1] = (packed[src + 3] << 4) | (packed[src] >> 4);
      pixels[dst + 2] = ((packed[src + 5] & 0x0f) << 8) | packed[src + 2];
      pixels[dst + 3] = (packed[src + 4] << 4) | (packed[src + 5] >> 4);
    }

  return TRUE;
}

gboolean
goodix5125_image_decode_transposed (const guint8 *packed,
                                    gsize         packed_len,
                                    guint16      *pixels,
                                    gsize         pixels_len,
                                    GError      **error)
{
  g_autofree guint16 *stream = NULL;

  if (!check_buffer (pixels, pixels_len, GOODIX5125_IMAGE_N_PIXELS,
                     "Transposed image", error))
    return FALSE;

  stream = g_new (guint16, GOODIX5125_IMAGE_N_PIXELS);
  if (!goodix5125_image_decode (packed, packed_len, stream,
                                GOODIX5125_IMAGE_N_PIXELS, error))
    return FALSE;

  for (gsize i = 0; i < GOODIX5125_IMAGE_N_PIXELS; i++)
    pixels[(i % GOODIX5125_IMAGE_STREAM_WIDTH) * GOODIX5125_IMAGE_WIDTH +
           i / GOODIX5125_IMAGE_STREAM_WIDTH] = stream[i];

  return TRUE;
}
