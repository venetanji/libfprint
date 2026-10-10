/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gio/gio.h>
gboolean goodix521d_read_private (const char *path, guint8 *out, gsize length, GError **error);
gboolean goodix521d_configure (const guint8 factory[64], guint8 config[256], GError **error);
