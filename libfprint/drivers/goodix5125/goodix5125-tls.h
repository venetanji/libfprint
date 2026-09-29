/*
 * Goodix 27c6:5125 TLS-PSK transport
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

typedef struct _Goodix5125Tls Goodix5125Tls;

typedef enum {
  GOODIX5125_TLS_COMPLETE,
  GOODIX5125_TLS_WANT_IO,
  GOODIX5125_TLS_ERROR,
} Goodix5125TlsResult;

Goodix5125Tls *goodix5125_tls_new (const guint8 *psk,
                                   gsize         psk_length,
                                   gboolean      deterministic,
                                   GError      **error);
void           goodix5125_tls_free (Goodix5125Tls *self);
gboolean       goodix5125_tls_feed_ciphertext (Goodix5125Tls *self,
                                               const guint8  *data,
                                               gsize          length,
                                               GError       **error);
GBytes        *goodix5125_tls_take_ciphertext (Goodix5125Tls *self);
Goodix5125TlsResult goodix5125_tls_handshake (Goodix5125Tls *self,
                                              GError       **error);
gboolean       goodix5125_tls_read_plaintext (Goodix5125Tls *self,
                                              GByteArray    *output,
                                              GError       **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Goodix5125Tls, goodix5125_tls_free)
