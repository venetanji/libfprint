/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include "goodix5125-calib.h"

G_BEGIN_DECLS

gboolean goodix5125_fdt_reply_is_finger_off (const guint8 *reply,
                                             gsize         reply_length,
                                             gboolean     *finger_off,
                                             GError      **error);

gboolean goodix5125_fdt_down_base_from_reply (const guint8 *reply,
                                              gsize         reply_length,
                                              guint8        output[GOODIX5125_FDT_BASE_SIZE],
                                              GError      **error);

gboolean goodix5125_fdt_up_base_from_reply (const guint8 *reply,
                                            gsize         reply_length,
                                            guint8        delta,
                                            guint8        output[GOODIX5125_FDT_BASE_SIZE],
                                            GError      **error);

G_END_DECLS
