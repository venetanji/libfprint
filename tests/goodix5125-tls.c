/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <glib.h>
#include <openssl/ssl.h>

#include "drivers/goodix5125/goodix5125-tls.h"

static const guint8 test_psk[32] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static unsigned int
client_psk_cb (SSL           *connection,
               const char    *hint,
               char          *identity,
               unsigned int   maximum_identity_length,
               unsigned char *psk,
               unsigned int   maximum_psk_length)
{
  const char client_identity[] = "Client_identity";

  (void) connection;
  (void) hint;

  g_assert_cmpuint (sizeof (client_identity), <=, maximum_identity_length);
  g_assert_cmpuint (sizeof (test_psk), <=, maximum_psk_length);
  memcpy (identity, client_identity, sizeof (client_identity));
  memcpy (psk, test_psk, sizeof (test_psk));
  return sizeof (test_psk);
}

static void
pump_bio_to_server (BIO           *source,
                    Goodix5125Tls *server)
{
  guint8 buffer[4096];
  int count;

  while ((count = BIO_read (source, buffer, sizeof (buffer))) > 0)
    {
      g_autoptr(GError) error = NULL;
      g_assert_true (goodix5125_tls_feed_ciphertext (server, buffer, count,
                                                     &error));
      g_assert_no_error (error);
    }
}

static void
pump_server_to_bio (Goodix5125Tls *server,
                    BIO           *destination)
{
  g_autoptr(GBytes) bytes = goodix5125_tls_take_ciphertext (server);
  gsize length = 0;
  gconstpointer data = g_bytes_get_data (bytes, &length);

  if (length)
    g_assert_cmpint (BIO_write (destination, data, length), ==, length);
}

static void
test_memory_bio_handshake (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Goodix5125Tls) server = NULL;
  SSL_CTX *client_context;
  SSL *client;
  BIO *client_read;
  BIO *client_write;
  gboolean server_complete = FALSE;

  server = goodix5125_tls_new (test_psk, sizeof (test_psk), &error);
  g_assert_no_error (error);
  g_assert_nonnull (server);

  client_context = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (client_context);
  g_assert_true (SSL_CTX_set_min_proto_version (client_context,
                                                TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_max_proto_version (client_context,
                                                TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_cipher_list (client_context,
                                          "PSK-AES128-GCM-SHA256"));
  SSL_CTX_set_psk_client_callback (client_context, client_psk_cb);

  client = SSL_new (client_context);
  g_assert_nonnull (client);
  client_read = BIO_new (BIO_s_mem ());
  client_write = BIO_new (BIO_s_mem ());
  g_assert_nonnull (client_read);
  g_assert_nonnull (client_write);
  BIO_set_mem_eof_return (client_read, -1);
  BIO_set_mem_eof_return (client_write, -1);
  SSL_set_bio (client, client_read, client_write);
  SSL_set_connect_state (client);

  for (guint iteration = 0; iteration < 32; iteration++)
    {
      int result = SSL_do_handshake (client);
      if (result != 1)
        {
          int code = SSL_get_error (client, result);
          g_assert_true (code == SSL_ERROR_WANT_READ ||
                         code == SSL_ERROR_WANT_WRITE);
        }

      pump_bio_to_server (client_write, server);

      switch (goodix5125_tls_handshake (server, &error))
        {
        case GOODIX5125_TLS_COMPLETE:
          server_complete = TRUE;
          break;

        case GOODIX5125_TLS_WANT_IO:
          break;

        case GOODIX5125_TLS_ERROR:
          g_assert_no_error (error);
          g_assert_not_reached ();
        }
      pump_server_to_bio (server, client_read);

      if (server_complete && SSL_is_init_finished (client))
        break;
    }

  g_assert_true (server_complete);
  g_assert_true (SSL_is_init_finished (client));

  {
    static const guint8 message[] = "goodix5125-memory-bio";
    size_t written = 0;
    g_autoptr(GByteArray) plaintext = g_byte_array_new ();

    g_assert_true (SSL_write_ex (client, message, sizeof (message), &written));
    g_assert_cmpuint (written, ==, sizeof (message));
    pump_bio_to_server (client_write, server);
    g_assert_true (goodix5125_tls_read_plaintext (server, plaintext, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (plaintext->data, plaintext->len,
                     message, sizeof (message));
  }

  SSL_free (client);
  SSL_CTX_free (client_context);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix5125/tls/memory-bio", test_memory_bio_handshake);
  return g_test_run ();
}
