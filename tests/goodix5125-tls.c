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

  server = goodix5125_tls_new (test_psk, sizeof (test_psk), FALSE, &error);
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

static SSL *
new_client (SSL_CTX **context_out,
            BIO     **read_out,
            BIO     **write_out)
{
  SSL_CTX *context = SSL_CTX_new (TLS_client_method ());
  SSL *client;

  g_assert_nonnull (context);
  g_assert_true (SSL_CTX_set_min_proto_version (context, TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_max_proto_version (context, TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_cipher_list (context, "PSK-AES128-GCM-SHA256"));
  SSL_CTX_set_psk_client_callback (context, client_psk_cb);

  client = SSL_new (context);
  g_assert_nonnull (client);
  *read_out = BIO_new (BIO_s_mem ());
  *write_out = BIO_new (BIO_s_mem ());
  BIO_set_mem_eof_return (*read_out, -1);
  BIO_set_mem_eof_return (*write_out, -1);
  SSL_set_bio (client, *read_out, *write_out);
  SSL_set_connect_state (client);
  *context_out = context;
  return client;
}

static GBytes *
take_bio (BIO *bio)
{
  GByteArray *bytes = g_byte_array_new ();
  guint8 buffer[4096];
  int count;

  while ((count = BIO_read (bio, buffer, sizeof (buffer))) > 0)
    g_byte_array_append (bytes, buffer, count);
  return g_byte_array_free_to_bytes (bytes);
}

/* Feeds one flight of the client to @server and returns the server's
 * answer. */
static GBytes *
server_step (Goodix5125Tls *server,
             GBytes        *input,
             gboolean      *complete)
{
  g_autoptr(GError) error = NULL;
  gsize length = 0;
  const guint8 *data = g_bytes_get_data (input, &length);
  Goodix5125TlsResult result;

  g_assert_true (goodix5125_tls_feed_ciphertext (server, data, length,
                                                 &error));
  g_assert_no_error (error);
  result = goodix5125_tls_handshake (server, &error);
  g_assert_no_error (error);
  g_assert_cmpint (result, !=, GOODIX5125_TLS_ERROR);
  *complete = result == GOODIX5125_TLS_COMPLETE;
  return goodix5125_tls_take_ciphertext (server);
}

/* umockdev replays the sensor's (client's) recorded records against a new
 * server; in deterministic mode that server must answer with the recorded
 * bytes and be able to decrypt the recorded application data. */
static void
test_deterministic_handshake (void)
{
  static const guint8 message[] = "goodix5125-emulated-image";

  g_autoptr(GError) error = NULL;
  g_autoptr(Goodix5125Tls) recorded = NULL;
  g_autoptr(Goodix5125Tls) replayed = NULL;
  g_autoptr(Goodix5125Tls) random_server = NULL;
  g_autoptr(GPtrArray) client_flights = g_ptr_array_new_with_free_func (
    (GDestroyNotify) g_bytes_unref);
  g_autoptr(GPtrArray) server_flights = g_ptr_array_new_with_free_func (
    (GDestroyNotify) g_bytes_unref);
  g_autoptr(GBytes) record = NULL;
  g_autoptr(GBytes) random_answer = NULL;
  g_autoptr(GByteArray) plaintext = g_byte_array_new ();
  SSL_CTX *client_context;
  SSL *client;
  BIO *client_read;
  BIO *client_write;
  gboolean complete = FALSE;
  size_t written = 0;

  recorded = goodix5125_tls_new (test_psk, sizeof (test_psk), TRUE, &error);
  g_assert_no_error (error);
  client = new_client (&client_context, &client_read, &client_write);

  /* Recording: a regular client against a deterministic server */
  for (guint iteration = 0; iteration < 32 && !complete; iteration++)
    {
      g_autoptr(GBytes) answer = NULL;
      gsize length = 0;
      const guint8 *data;
      int result = SSL_do_handshake (client);

      if (result != 1)
        {
          int code = SSL_get_error (client, result);
          g_assert_true (code == SSL_ERROR_WANT_READ ||
                         code == SSL_ERROR_WANT_WRITE);
        }
      g_ptr_array_add (client_flights, take_bio (client_write));
      answer = server_step (recorded,
                            g_ptr_array_index (client_flights, iteration),
                            &complete);
      data = g_bytes_get_data (answer, &length);
      if (length)
        g_assert_cmpint (BIO_write (client_read, data, length), ==, length);
      g_ptr_array_add (server_flights, g_steal_pointer (&answer));
    }
  g_assert_true (complete);
  g_assert_cmpint (SSL_do_handshake (client), ==, 1);
  g_assert_true (SSL_write_ex (client, message, sizeof (message), &written));
  record = take_bio (client_write);

  /* Replay: the same client bytes against a new deterministic server */
  replayed = goodix5125_tls_new (test_psk, sizeof (test_psk), TRUE, &error);
  g_assert_no_error (error);
  complete = FALSE;
  for (guint iteration = 0; iteration < client_flights->len; iteration++)
    {
      g_autoptr(GBytes) answer = server_step (
        replayed, g_ptr_array_index (client_flights, iteration), &complete);

      g_assert_true (g_bytes_equal (answer,
                                    g_ptr_array_index (server_flights,
                                                       iteration)));
    }
  g_assert_true (complete);
  g_assert_true (goodix5125_tls_feed_ciphertext (
                   replayed, g_bytes_get_data (record, NULL),
                   g_bytes_get_size (record), &error));
  g_assert_no_error (error);
  g_assert_true (goodix5125_tls_read_plaintext (replayed, plaintext, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (plaintext->data, plaintext->len, message, sizeof (message));

  /* Without the deterministic mode, the ServerHello random differs */
  random_server = goodix5125_tls_new (test_psk, sizeof (test_psk), FALSE,
                                      &error);
  g_assert_no_error (error);
  random_answer = server_step (random_server,
                               g_ptr_array_index (client_flights, 0),
                               &complete);
  g_assert_false (g_bytes_equal (random_answer,
                                 g_ptr_array_index (server_flights, 0)));

  SSL_free (client);
  SSL_CTX_free (client_context);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix5125/tls/memory-bio", test_memory_bio_handshake);
  g_test_add_func ("/goodix5125/tls/deterministic",
                   test_deterministic_handshake);
  return g_test_run ();
}
