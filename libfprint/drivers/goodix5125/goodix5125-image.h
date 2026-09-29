/*
 * Goodix 27c6:5125 raw image decoder
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GOODIX5125_IMAGE_STREAM_WIDTH 64
#define GOODIX5125_IMAGE_STREAM_HEIGHT 80
#define GOODIX5125_IMAGE_WIDTH 80
#define GOODIX5125_IMAGE_HEIGHT 64
#define GOODIX5125_IMAGE_N_PIXELS 5120
#define GOODIX5125_IMAGE_PACKED_SIZE 7680
#define GOODIX5125_IMAGE_MAX_VALUE 4095

gboolean goodix5125_image_decode (const guint8 *packed,
                                  gsize         packed_len,
                                  guint16      *pixels,
                                  gsize         pixels_len,
                                  GError      **error);

gboolean goodix5125_image_decode_transposed (const guint8 *packed,
                                             gsize         packed_len,
                                             guint16      *pixels,
                                             gsize         pixels_len,
                                             GError      **error);

G_END_DECLS
