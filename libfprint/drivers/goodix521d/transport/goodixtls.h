// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#pragma once

#include <gio/gio.h>
#include <openssl/ssl.h>
#include <pthread.h>

struct _GoodixTlsServer;
typedef void (*GoodixTlsServerConnectionCallback)(struct _GoodixTlsServer *self,
                                                  GError *error,
                                                  gpointer user_data);

/* Zero-initialize before first use. The owner serializes all public calls.
 * The worker callback is informational, borrows error, must return promptly,
 * and must not call these APIs or free the session. */
typedef struct _GoodixTlsServer {
    guint8 psk[32];
    GoodixTlsServerConnectionCallback connection_callback;
    gpointer user_data;
    SSL_CTX *ssl_ctx;
    int sock_fd;
    SSL *ssl_layer;
    int client_fd;
    pthread_t serve_thread;
    GMutex mutex;
    GCond condition;
    gboolean primitives_initialized;
    gboolean initialized;
    gboolean serve_thread_started;
    gboolean handshake_done;
    gboolean authenticated;
    gboolean closing;
    gint64 handshake_deadline;
    GError *first_error;
} GoodixTlsServer;

/* The path must be absolute. The opened inode must be a root-owned regular
 * file with mode 0600 and exactly 32 bytes. Final symlinks are not followed.
 * Any failure zeroes psk; neither loader reads or writes pairing material. */
gboolean goodix_tls_load_psk_file(const char *path, guint8 psk[32], GError **error);
gboolean goodix_tls_load_521d_psk(guint8 psk[32], GError **error);
gboolean goodix_tls_server_init(GoodixTlsServer *self, GError **error);

/* Required before reporting TLS success to the reader. Waits for the worker
 * including its callback, joins it, and accepts only an authenticated PSK
 * TLS Finished. The whole handshake has a 5-second deadline from init. */
gboolean goodix_tls_server_wait_authenticated(GoodixTlsServer *self, GError **error);

/* Each transport/plaintext operation has a 2-second absolute deadline.
 * Send returns exactly length or -1; recv may return a positive short read.
 * Plaintext is unavailable until wait_authenticated has joined the worker. */
int goodix_tls_client_send(GoodixTlsServer *self, guint8 *data, guint16 length);
int goodix_tls_client_recv(GoodixTlsServer *self, guint8 *data, guint16 length);
int goodix_tls_server_receive(GoodixTlsServer *self, guint8 *data,
                              guint32 length, GError **error);

/* Shutdown transport, then join, then free. No TLS close_notify exchange.
 * On a 2-second cleanup timeout returns FALSE with resources retained:
 * the owner MUST NOT free self and may retry cleanup later. */
gboolean goodix_tls_server_deinit(GoodixTlsServer *self, GError **error);

/* Nonblocking, mutex-protected observation only; busy is unhealthy. The owner
 * must serialize this call with initialization/destruction of the server. */
gboolean goodix_tls_server_health(GoodixTlsServer *self, GError **error);
