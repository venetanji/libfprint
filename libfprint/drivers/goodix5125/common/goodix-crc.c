// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Goodix checksum primitives shared by independently verified device stacks.
 *
 * Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "goodix-crc.h"

guint32
goodix_crc32_mpeg2 (const guint8 *data,
                    gsize         length)
{
  guint32 crc = G_MAXUINT32;

  g_return_val_if_fail (data != NULL || length == 0, 0);
  for (gsize offset = 0; offset < length; offset++)
    {
      crc ^= (guint32) data[offset] << 24;
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc << 1) ^ (0x04c11db7u & -(crc >> 31));
    }
  return crc;
}
