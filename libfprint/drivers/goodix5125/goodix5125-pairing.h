/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GOODIX5125_PSK_SIZE 32
#define GOODIX5125_WB_SIZE 102
#define GOODIX5125_PSK_WB_ADDRESS 0xbb010003u
#define GOODIX5125_PSK_HASH_ADDRESS 0xbb020003u

typedef enum {
  GOODIX5125_PSK_STATE_ERROR = -1,
  GOODIX5125_PSK_STATE_MISSING,
  GOODIX5125_PSK_STATE_FOUND,
} Goodix5125PskState;

gboolean goodix5125_pairing_wrap (const guint8 psk[GOODIX5125_PSK_SIZE],
                                  guint8       wb[GOODIX5125_WB_SIZE],
                                  GError     **error);
gboolean goodix5125_pairing_hash (const guint8 wb[GOODIX5125_WB_SIZE],
                                  guint8       hash[GOODIX5125_PSK_SIZE],
                                  GError     **error);
GBytes *goodix5125_pairing_build_write_payload (const guint8 psk[GOODIX5125_PSK_SIZE],
                                                GError     **error);
gboolean goodix5125_pairing_self_test (GError **error);

gboolean goodix5125_pairing_parse_hash_reply (const guint8 *reply,
                                              gsize         reply_length,
                                              gboolean     *present,
                                              guint8        hash[GOODIX5125_PSK_SIZE],
                                              GError      **error);

Goodix5125PskState goodix5125_pairing_load_psk (const gchar *path,
                                                guint8       psk[GOODIX5125_PSK_SIZE],
                                                GError     **error);
gboolean goodix5125_pairing_save_psk (const gchar *path,
                                      const guint8 psk[GOODIX5125_PSK_SIZE],
                                      GError     **error);

G_END_DECLS
