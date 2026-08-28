/*
 * Goodix 27c6:5125 TLS-PSK transport
 *
 * The sensor is a TLS client and the host driver is the TLS server.  Memory
 * BIOs let the libfprint state machine exchange TLS records through the USB
 * protocol without sockets or subprocesses.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "goodix5125-tls.h"

#include <gio/gio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

struct _Goodix5125Tls
{
  SSL_CTX *context;
  SSL     *connection;
  BIO     *read_bio;
  BIO     *write_bio;

  guint8  *psk;
  gsize    psk_length;
};

static void
set_openssl_error (GError    **error,
                   const char *operation)
{
  unsigned long code = ERR_get_error ();
  char message[256] = { 0 };

  if (code)
    ERR_error_string_n (code, message, sizeof (message));
  else
    g_strlcpy (message, "unknown OpenSSL error", sizeof (message));

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s failed: %s", operation, message);
}

static unsigned int
psk_server_cb (SSL           *connection,
               const char    *identity,
               unsigned char *output,
               unsigned int   maximum_length)
{
  Goodix5125Tls *self = SSL_get_app_data (connection);

  (void) identity;

  if (!self || self->psk_length > maximum_length)
    return 0;

  memcpy (output, self->psk, self->psk_length);
  return self->psk_length;
}

Goodix5125Tls *
goodix5125_tls_new (const guint8 *psk,
                    gsize         psk_length,
                    GError      **error)
{
  g_autoptr(Goodix5125Tls) self = NULL;
  BIO *read_bio = NULL;
  BIO *write_bio = NULL;

  g_return_val_if_fail (psk != NULL, NULL);
  g_return_val_if_fail (psk_length > 0, NULL);

  self = g_new0 (Goodix5125Tls, 1);
  self->psk = g_malloc (psk_length);
  memcpy (self->psk, psk, psk_length);
  self->psk_length = psk_length;

  self->context = SSL_CTX_new (TLS_server_method ());
  if (!self->context)
    {
      set_openssl_error (error, "SSL_CTX_new");
      return NULL;
    }

  if (!SSL_CTX_set_min_proto_version (self->context, TLS1_2_VERSION) ||
      !SSL_CTX_set_max_proto_version (self->context, TLS1_2_VERSION) ||
      !SSL_CTX_set_cipher_list (self->context,
                                "PSK-AES128-GCM-SHA256:"
                                "PSK-AES256-GCM-SHA384:"
                                "PSK-AES128-CBC-SHA256:"
                                "PSK-AES128-CBC-SHA"))
    {
      set_openssl_error (error, "TLS-PSK configuration");
      return NULL;
    }

  SSL_CTX_set_psk_server_callback (self->context, psk_server_cb);

  self->connection = SSL_new (self->context);
  if (!self->connection)
    {
      set_openssl_error (error, "SSL_new");
      return NULL;
    }

  read_bio = BIO_new (BIO_s_mem ());
  write_bio = BIO_new (BIO_s_mem ());
  if (!read_bio || !write_bio)
    {
      BIO_free (read_bio);
      BIO_free (write_bio);
      set_openssl_error (error, "BIO_new");
      return NULL;
    }

  BIO_set_mem_eof_return (read_bio, -1);
  BIO_set_mem_eof_return (write_bio, -1);
  SSL_set_bio (self->connection, read_bio, write_bio);
  self->read_bio = read_bio;
  self->write_bio = write_bio;

  SSL_set_app_data (self->connection, self);
  SSL_set_accept_state (self->connection);

  return g_steal_pointer (&self);
}

void
goodix5125_tls_free (Goodix5125Tls *self)
{
  if (!self)
    return;

  SSL_free (self->connection);
  SSL_CTX_free (self->context);
  if (self->psk)
    {
      OPENSSL_cleanse (self->psk, self->psk_length);
      g_free (self->psk);
    }
  g_free (self);
}

gboolean
goodix5125_tls_feed_ciphertext (Goodix5125Tls *self,
                                const guint8  *data,
                                gsize          length,
                                GError       **error)
{
  gsize offset = 0;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (data != NULL || length == 0, FALSE);

  while (offset < length)
    {
      int written = BIO_write (self->read_bio, data + offset,
                               MIN (length - offset, G_MAXINT));
      if (written <= 0)
        {
          set_openssl_error (error, "BIO_write");
          return FALSE;
        }
      offset += written;
    }

  return TRUE;
}

GBytes *
goodix5125_tls_take_ciphertext (Goodix5125Tls *self)
{
  gsize pending;
  guint8 *data;
  gsize offset = 0;

  g_return_val_if_fail (self != NULL, NULL);

  pending = BIO_ctrl_pending (self->write_bio);
  if (!pending)
    return g_bytes_new_static ("", 0);

  data = g_malloc (pending);
  while (offset < pending)
    {
      int count = BIO_read (self->write_bio, data + offset,
                            MIN (pending - offset, G_MAXINT));
      if (count <= 0)
        break;
      offset += count;
    }

  return g_bytes_new_take (data, offset);
}

Goodix5125TlsResult
goodix5125_tls_handshake (Goodix5125Tls *self,
                          GError       **error)
{
  int result;
  int ssl_error;

  g_return_val_if_fail (self != NULL, GOODIX5125_TLS_ERROR);

  result = SSL_do_handshake (self->connection);
  if (result == 1)
    return GOODIX5125_TLS_COMPLETE;

  ssl_error = SSL_get_error (self->connection, result);
  if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
    return GOODIX5125_TLS_WANT_IO;

  set_openssl_error (error, "TLS handshake");
  return GOODIX5125_TLS_ERROR;
}

gboolean
goodix5125_tls_read_plaintext (Goodix5125Tls *self,
                               GByteArray    *output,
                               GError       **error)
{
  guint8 buffer[16384];

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (output != NULL, FALSE);

  while (TRUE)
    {
      size_t count = 0;
      int result = SSL_read_ex (self->connection, buffer, sizeof (buffer),
                                &count);

      if (result == 1)
        {
          g_byte_array_append (output, buffer, count);
          continue;
        }

      switch (SSL_get_error (self->connection, result))
        {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_ZERO_RETURN:
          return TRUE;

        default:
          set_openssl_error (error, "TLS plaintext read");
          return FALSE;
        }
    }
}
