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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "goodixtls.h"

#define HANDSHAKE_TIMEOUT_US (5 * 1000000LL)
#define IO_TIMEOUT_US (2 * 1000000LL)
#define CLEANUP_TIMEOUT_US (2 * 1000000LL)

gboolean goodix_tls_load_psk_file(const char *path, guint8 psk[32], GError **error)
{
    guint8 buffer[33] = {0};
    struct stat st;
    if (!psk) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Missing Goodix reader key output");
        return FALSE;
    }
    memset(psk, 0, 32);
    if (!path || path[0] != '/') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Goodix reader key path must be absolute");
        return FALSE;
    }
    /* O_NONBLOCK also prevents opening a malicious FIFO from hanging before
     * fstat can reject it. No missing-file or zero-key fallback exists. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Cannot open Goodix reader key: %s", g_strerror(errno));
        return FALSE;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600 || st.st_size != 32) {
        close(fd);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Goodix reader key must be a root-owned 0600 file of 32 bytes");
        return FALSE;
    }
    ssize_t size;
    do { size = read(fd, buffer, sizeof buffer); } while (size < 0 && errno == EINTR);
    close(fd);
    if (size != 32) {
        OPENSSL_cleanse(buffer, sizeof buffer);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Cannot read complete Goodix reader key");
        return FALSE;
    }
    memcpy(psk, buffer, 32);
    OPENSSL_cleanse(buffer, sizeof buffer);
    return TRUE;
}

gboolean goodix_tls_load_521d_psk(guint8 psk[32], GError **error)
{
    return goodix_tls_load_psk_file("/etc/libfprint/goodix-521d.psk", psk, error);
}

static GError *tls_error(const char *operation)
{
    const char *reason = ERR_reason_error_string(ERR_get_error());
    return g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", operation,
                       reason ? reason : "TLS operation failed");
}

static gboolean fail(GoodixTlsServer *self, GError *reason, GError **error)
{
    g_mutex_lock(&self->mutex);
    if (!self->first_error) self->first_error = reason;
    else g_error_free(reason);
    self->authenticated = FALSE;
    if (error && !*error) *error = g_error_copy(self->first_error);
    g_cond_broadcast(&self->condition);
    g_mutex_unlock(&self->mutex);
    return FALSE;
}

static gboolean session_ready(GoodixTlsServer *self, GError **error)
{
    if (!self || !self->initialized) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "TLS session is closed");
        return FALSE;
    }
    g_mutex_lock(&self->mutex);
    gboolean ok = !self->closing && !self->first_error;
    if (!ok && error && !*error)
        *error = self->first_error ? g_error_copy(self->first_error) :
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "TLS session is closing");
    g_mutex_unlock(&self->mutex);
    return ok;
}

static gboolean wait_fd(GoodixTlsServer *self, int fd, short events,
                         gint64 deadline, GError **error)
{
    for (;;) {
        if (!session_ready(self, error)) return FALSE;
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0)
            return fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                  "TLS operation timed out"), error);
        struct pollfd p = {.fd = fd, .events = events};
        int result = poll(&p, 1, (int) MIN((remaining + 999) / 1000, INT_MAX));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0)
            return fail(self, g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                          "TLS poll failed: %s", g_strerror(errno)), error);
        if (result == 0) continue;
        if (p.revents & events) return TRUE;
        return fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                                              "TLS transport disconnected"), error);
    }
}

/* SSL's socket BIO may write an alert during accept/read. Contain SIGPIPE
 * within this thread; never change the application's process-wide handler. */
typedef struct { sigset_t old; gboolean pending; } PipeMask;
static PipeMask block_pipe(void)
{
    PipeMask mask;
    sigset_t set, pending;
    sigemptyset(&set); sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, &mask.old);
    sigpending(&pending); mask.pending = sigismember(&pending, SIGPIPE);
    return mask;
}
static void restore_pipe(PipeMask mask)
{
    if (!mask.pending && !sigismember(&mask.old, SIGPIPE)) {
        sigset_t set;
        struct timespec zero = {0};
        sigemptyset(&set); sigaddset(&set, SIGPIPE);
        while (sigtimedwait(&set, NULL, &zero) >= 0) {}
    }
    pthread_sigmask(SIG_SETMASK, &mask.old, NULL);
}

static unsigned int psk_callback(SSL *ssl, const char *identity,
                                 unsigned char *psk, unsigned int capacity)
{
    (void) identity;
    GoodixTlsServer *self = SSL_get_app_data(ssl);
    if (!self || capacity < sizeof self->psk) return 0;
    memcpy(psk, self->psk, sizeof self->psk);
    return sizeof self->psk;
}

static gboolean ssl_retry(GoodixTlsServer *self, int result, gint64 deadline,
                          const char *operation, GError **error)
{
    /* Must follow the failing SSL call in the same thread before other SSL
     * operations, with its error queue initially cleared. */
    int code = SSL_get_error(self->ssl_layer, result);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE)
        return wait_fd(self, self->sock_fd,
                       code == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline, error);
    return fail(self, tls_error(operation), error);
}

static void *goodix_tls_init_serve(void *data)
{
    GoodixTlsServer *self = data;
    PipeMask mask = block_pipe();
    GError *error = NULL;
    gboolean success = FALSE;
    for (;;) {
        if (!session_ready(self, &error)) break;
        if (g_get_monotonic_time() >= self->handshake_deadline) {
            fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                           "TLS authentication timed out"), &error);
            break;
        }
        ERR_clear_error();
        int result = SSL_accept(self->ssl_layer);
        if (result == 1) {
            /* Finished alone must not permit a non-PSK cipher to authenticate. */
            success = SSL_is_init_finished(self->ssl_layer) &&
                      SSL_get_psk_identity(self->ssl_layer) != NULL &&
                      SSL_version(self->ssl_layer)==TLS1_2_VERSION &&
                      SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(self->ssl_layer))==0x00ae;
            if (!success)
                fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                               "TLS peer did not authenticate with PSK"), &error);
            break;
        }
        if (!ssl_retry(self, result, self->handshake_deadline, "TLS handshake", &error)) break;
    }
    restore_pipe(mask);
    g_mutex_lock(&self->mutex);
    self->authenticated = success && !self->closing && !self->first_error &&
                          g_get_monotonic_time() < self->handshake_deadline;
    if (!self->authenticated && !self->first_error)
        self->first_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED,
                                                "TLS handshake did not complete");
    g_clear_error(&error);
    if (self->first_error) error = g_error_copy(self->first_error);
    g_mutex_unlock(&self->mutex);
    if (self->connection_callback) self->connection_callback(self, error, self->user_data);
    g_clear_error(&error);
    g_mutex_lock(&self->mutex);
    self->handshake_done = TRUE;
    g_cond_broadcast(&self->condition);
    g_mutex_unlock(&self->mutex);
    return NULL;
}

static gboolean join_worker(GoodixTlsServer *self, GError **error)
{
    if (!self->serve_thread_started) return TRUE;
    int code = pthread_join(self->serve_thread, NULL);
    if (code != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "TLS worker join failed: %s", g_strerror(code));
        return FALSE;
    }
    self->serve_thread_started = FALSE;
    return TRUE;
}

gboolean goodix_tls_server_wait_authenticated(GoodixTlsServer *self, GError **error)
{
    if (!self || !self->initialized) return session_ready(self, error);
    g_mutex_lock(&self->mutex);
    while (self->serve_thread_started && !self->handshake_done) {
        if (!g_cond_wait_until(&self->condition, &self->mutex, self->handshake_deadline)) break;
    }
    if (!self->handshake_done) {
        self->authenticated = FALSE;
        if (!self->first_error)
            self->first_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                    "TLS authentication timed out");
        /* Wake the nonblocking SSL worker. Deinit owns its eventual join. */
        shutdown(self->sock_fd, SHUT_RDWR);
        shutdown(self->client_fd, SHUT_RDWR);
        if (error && !*error) *error = g_error_copy(self->first_error);
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }
    gboolean ok = self->authenticated && !self->closing && !self->first_error;
    if (!ok && error && !*error)
        *error = self->first_error ? g_error_copy(self->first_error) :
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "TLS peer is not authenticated");
    g_mutex_unlock(&self->mutex);
    return join_worker(self, error) && ok;
}

gboolean goodix_tls_server_init(GoodixTlsServer *self, GError **error)
{
    if (!self || self->initialized) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Invalid or active TLS session");
        return FALSE;
    }
    self->sock_fd = self->client_fd = -1;
    self->ssl_ctx = NULL; self->ssl_layer = NULL; self->first_error = NULL;
    self->serve_thread_started = self->closing = self->handshake_done = self->authenticated = FALSE;
    g_mutex_init(&self->mutex); g_cond_init(&self->condition);
    self->primitives_initialized = self->initialized = TRUE;
    self->handshake_deadline = g_get_monotonic_time() + HANDSHAKE_TIMEOUT_US;
    ERR_clear_error();
    self->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!self->ssl_ctx ||
        !SSL_CTX_set_min_proto_version(self->ssl_ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(self->ssl_ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_cipher_list(self->ssl_ctx, "PSK-AES128-CBC-SHA256") ||
        !SSL_CTX_set_dh_auto(self->ssl_ctx, 1)) {
        fail(self, tls_error("TLS context initialization"), error);
        goto failed;
    }
    SSL_CTX_set_options(self->ssl_ctx,SSL_OP_NO_TICKET);
    SSL_CTX_set_psk_server_callback(self->ssl_ctx, psk_callback);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) != 0) {
        fail(self, g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                              "TLS socketpair: %s", g_strerror(errno)), error);
        goto failed;
    }
    self->sock_fd = pair[0]; self->client_fd = pair[1];
    self->ssl_layer = SSL_new(self->ssl_ctx);
    if (!self->ssl_layer || !SSL_set_fd(self->ssl_layer, self->sock_fd)) {
        fail(self, tls_error("TLS SSL initialization"), error);
        goto failed;
    }
    SSL_set_app_data(self->ssl_layer, self);
    int code = pthread_create(&self->serve_thread, NULL, goodix_tls_init_serve, self);
    if (code != 0) {
        fail(self, g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                              "TLS worker creation: %s", g_strerror(code)), error);
        goto failed;
    }
    self->serve_thread_started = TRUE;
    return TRUE;
failed:
    goodix_tls_server_deinit(self, NULL);
    return FALSE;
}

int goodix_tls_client_send(GoodixTlsServer *self, guint8 *data, guint16 length)
{
    if (!data || !length || !session_ready(self, NULL)) return -1;
    gint64 deadline = g_get_monotonic_time() + IO_TIMEOUT_US;
    size_t offset = 0;
    while (offset < length) {
        if (!wait_fd(self, self->client_fd, POLLOUT, deadline, NULL)) return -1;
        ssize_t count = send(self->client_fd, data + offset, length - offset, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) {
            fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                                           "TLS transport send failed"), NULL);
            return -1;
        }
        offset += count;
    }
    return session_ready(self, NULL) ? (int) offset : -1;
}

int goodix_tls_client_recv(GoodixTlsServer *self, guint8 *data, guint16 length)
{
    if (!data || !length || !session_ready(self, NULL)) return -1;
    gint64 deadline = g_get_monotonic_time() + IO_TIMEOUT_US;
    for (;;) {
        if (!wait_fd(self, self->client_fd, POLLIN, deadline, NULL)) return -1;
        ssize_t count = recv(self->client_fd, data, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) {
            fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                                           "TLS transport receive failed"), NULL);
            return -1;
        }
        return session_ready(self, NULL) ? (int) count : -1;
    }
}

int goodix_tls_server_receive(GoodixTlsServer *self, guint8 *data,
                              guint32 length, GError **error)
{
    if (!data || !length || length > INT_MAX) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid TLS plaintext buffer length");
        return -1;
    }
    if (!session_ready(self, error)) return -1;
    /* The main-loop owner must have joined the sole SSL_accept user first. */
    g_mutex_lock(&self->mutex);
    gboolean authenticated = self->authenticated && self->handshake_done &&
                             !self->serve_thread_started;
    g_mutex_unlock(&self->mutex);
    if (!authenticated) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "TLS peer has not completed authenticated handshake");
        return -1;
    }
    gint64 deadline = g_get_monotonic_time() + IO_TIMEOUT_US;
    PipeMask mask = block_pipe();
    int result = -1;
    for (;;) {
        if (!session_ready(self, error)) break;
        if (g_get_monotonic_time() >= deadline) {
            fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                           "TLS plaintext read timed out"), error);
            break;
        }
        ERR_clear_error();
        int count = SSL_read(self->ssl_layer, data, (int) length);
        if (count > 0) { result = count; break; }
        if (!ssl_retry(self, count, deadline, "TLS plaintext read", error)) break;
    }
    restore_pipe(mask);
    return result;
}

gboolean goodix_tls_server_deinit(GoodixTlsServer *self, GError **error)
{
    if (!self || !self->initialized) return TRUE;
    gint64 deadline = g_get_monotonic_time() + CLEANUP_TIMEOUT_US;
    g_mutex_lock(&self->mutex);
    self->closing = TRUE;
    self->authenticated = FALSE;
    if (self->sock_fd >= 0) shutdown(self->sock_fd, SHUT_RDWR);
    if (self->client_fd >= 0) shutdown(self->client_fd, SHUT_RDWR);
    while (self->serve_thread_started && !self->handshake_done) {
        if (!g_cond_wait_until(&self->condition, &self->mutex, deadline)) {
            g_mutex_unlock(&self->mutex);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "TLS cleanup incomplete; resources retained");
            return FALSE;
        }
    }
    g_mutex_unlock(&self->mutex);
    if (!join_worker(self, error)) return FALSE;
    /* No SSL_shutdown: the USB peer may already have gone away. */
    SSL_free(self->ssl_layer); self->ssl_layer = NULL;
    SSL_CTX_free(self->ssl_ctx); self->ssl_ctx = NULL;
    if (self->sock_fd >= 0) close(self->sock_fd);
    if (self->client_fd >= 0) close(self->client_fd);
    self->sock_fd = self->client_fd = -1;
    OPENSSL_cleanse(self->psk, sizeof self->psk);
    g_clear_error(&self->first_error);
    self->initialized = FALSE;
    g_cond_clear(&self->condition); g_mutex_clear(&self->mutex);
    self->primitives_initialized = FALSE;
    return TRUE;
}

gboolean goodix_tls_server_health(GoodixTlsServer *self, GError **error)
{
    if (!self || !self->primitives_initialized) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "TLS session unavailable");
        return FALSE;
    }
    if (!g_mutex_trylock(&self->mutex)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "TLS session busy");
        return FALSE;
    }
    gboolean healthy = self->initialized && self->handshake_done &&
        self->authenticated && !self->serve_thread_started &&
        !self->closing && !self->first_error;
    g_mutex_unlock(&self->mutex);
    if (!healthy)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                            "TLS session is not authenticated and idle");
    return healthy;
}
/* End TLS health */
