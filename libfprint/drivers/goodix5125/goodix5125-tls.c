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
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/params.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#define EMULATION_PROVIDER_NAME "goodix5125-emulation"
#define EMULATION_RAND_NAME "GOODIX5125-EMULATION-RAND"

struct _Goodix5125Tls
{
  /* Deterministic mode only: library context with a fixed-output RNG */
  OSSL_LIB_CTX  *library;
  OSSL_PROVIDER *rand_provider;
  OSSL_PROVIDER *default_provider;

  SSL_CTX       *context;
  SSL           *connection;
  BIO           *read_bio;
  BIO           *write_bio;

  guint8        *psk;
  gsize          psk_length;
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

/* ---------------------------------------------------------------------
 * Deterministic randomness for device emulation
 *
 * umockdev replays a recorded USB session and compares every OUT transfer
 * with the recording, so in emulation mode the TLS server has to send the
 * same bytes as when the session was recorded (ServerHello random, session
 * ID, AES-GCM explicit nonces).  The TLS context then uses its own OpenSSL
 * library context whose random generator returns a fixed pattern.  Every
 * request starts the pattern anew, so the output neither depends on how
 * many random bytes OpenSSL draws nor on the order of the requests.
 * --------------------------------------------------------------------- */

typedef struct
{
  int state;
} EmulationRand;

static void *
emulation_rand_new (void                *provider_context,
                    void                *parent,
                    const OSSL_DISPATCH *parent_dispatch)
{
  EmulationRand *rand = g_new0 (EmulationRand, 1);

  rand->state = EVP_RAND_STATE_UNINITIALISED;
  return rand;
}

static void
emulation_rand_free (void *rand)
{
  g_free (rand);
}

static int
emulation_rand_instantiate (void                *rand,
                            unsigned int         strength,
                            int                  prediction_resistance,
                            const unsigned char *personalization,
                            size_t               personalization_length,
                            const OSSL_PARAM     params[])
{
  ((EmulationRand *) rand)->state = EVP_RAND_STATE_READY;
  return 1;
}

static int
emulation_rand_uninstantiate (void *rand)
{
  ((EmulationRand *) rand)->state = EVP_RAND_STATE_UNINITIALISED;
  return 1;
}

static int
emulation_rand_generate (void                *rand,
                         unsigned char       *output,
                         size_t               length,
                         unsigned int         strength,
                         int                  prediction_resistance,
                         const unsigned char *additional,
                         size_t               additional_length)
{
  for (size_t i = 0; i < length; i++)
    output[i] = (unsigned char) (0x5a + i * 0x3d);
  return 1;
}

static int
emulation_rand_enable_locking (void *rand)
{
  return 1;
}

static int
emulation_rand_get_ctx_params (void      *rand,
                               OSSL_PARAM params[])
{
  OSSL_PARAM *param;

  param = OSSL_PARAM_locate (params, OSSL_RAND_PARAM_STATE);
  if (param && !OSSL_PARAM_set_int (param, ((EmulationRand *) rand)->state))
    return 0;
  param = OSSL_PARAM_locate (params, OSSL_RAND_PARAM_STRENGTH);
  if (param && !OSSL_PARAM_set_uint (param, 256))
    return 0;
  param = OSSL_PARAM_locate (params, OSSL_RAND_PARAM_MAX_REQUEST);
  if (param && !OSSL_PARAM_set_size_t (param, G_MAXINT))
    return 0;
  return 1;
}

static const OSSL_PARAM *
emulation_rand_gettable_ctx_params (void *rand,
                                    void *provider_context)
{
  static const OSSL_PARAM gettable[] = {
    OSSL_PARAM_int (OSSL_RAND_PARAM_STATE, NULL),
    OSSL_PARAM_uint (OSSL_RAND_PARAM_STRENGTH, NULL),
    OSSL_PARAM_size_t (OSSL_RAND_PARAM_MAX_REQUEST, NULL),
    OSSL_PARAM_END
  };

  return gettable;
}

static const OSSL_DISPATCH emulation_rand_functions[] = {
  { OSSL_FUNC_RAND_NEWCTX, (void (*)(void)) emulation_rand_new },
  { OSSL_FUNC_RAND_FREECTX, (void (*)(void)) emulation_rand_free },
  { OSSL_FUNC_RAND_INSTANTIATE, (void (*)(void)) emulation_rand_instantiate },
  { OSSL_FUNC_RAND_UNINSTANTIATE,
    (void (*)(void)) emulation_rand_uninstantiate },
  { OSSL_FUNC_RAND_GENERATE, (void (*)(void)) emulation_rand_generate },
  { OSSL_FUNC_RAND_ENABLE_LOCKING,
    (void (*)(void)) emulation_rand_enable_locking },
  { OSSL_FUNC_RAND_GET_CTX_PARAMS,
    (void (*)(void)) emulation_rand_get_ctx_params },
  { OSSL_FUNC_RAND_GETTABLE_CTX_PARAMS,
    (void (*)(void)) emulation_rand_gettable_ctx_params },
  { 0, NULL }
};

static const OSSL_ALGORITHM emulation_rand_algorithms[] = {
  { EMULATION_RAND_NAME, "provider=" EMULATION_PROVIDER_NAME,
    emulation_rand_functions, NULL },
  { NULL, NULL, NULL, NULL }
};

static const OSSL_ALGORITHM *
emulation_provider_query (void *provider_context,
                          int   operation,
                          int  *no_cache)
{
  *no_cache = 0;
  return operation == OSSL_OP_RAND ? emulation_rand_algorithms : NULL;
}

static const OSSL_DISPATCH emulation_provider_functions[] = {
  { OSSL_FUNC_PROVIDER_QUERY_OPERATION,
    (void (*)(void)) emulation_provider_query },
  { 0, NULL }
};

static int
emulation_provider_init (const OSSL_CORE_HANDLE *handle,
                         const OSSL_DISPATCH    *core,
                         const OSSL_DISPATCH   **provider,
                         void                  **provider_context)
{
  *provider = emulation_provider_functions;
  *provider_context = NULL;
  return 1;
}

static gboolean
setup_deterministic_library (Goodix5125Tls *self,
                             GError       **error)
{
  self->library = OSSL_LIB_CTX_new ();
  if (!self->library ||
      !RAND_set_DRBG_type (self->library, EMULATION_RAND_NAME, NULL, NULL,
                           NULL) ||
      !OSSL_PROVIDER_add_builtin (self->library, EMULATION_PROVIDER_NAME,
                                  emulation_provider_init) ||
      !(self->rand_provider = OSSL_PROVIDER_load (self->library,
                                                  EMULATION_PROVIDER_NAME)) ||
      !(self->default_provider = OSSL_PROVIDER_load (self->library,
                                                     "default")))
    {
      set_openssl_error (error, "deterministic TLS setup");
      return FALSE;
    }
  return TRUE;
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

/* With @deterministic (device emulation), the handshake and the records
 * sent by the host only depend on what the peer sends. */
Goodix5125Tls *
goodix5125_tls_new (const guint8 *psk,
                    gsize         psk_length,
                    gboolean      deterministic,
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

  if (deterministic && !setup_deterministic_library (self, error))
    return NULL;

  self->context = SSL_CTX_new_ex (self->library, NULL, TLS_server_method ());
  if (!self->context)
    {
      set_openssl_error (error, "SSL_CTX_new");
      return NULL;
    }
  /* A session ticket carries the session time */
  if (deterministic)
    SSL_CTX_set_options (self->context, SSL_OP_NO_TICKET);

  if (!SSL_CTX_set_min_proto_version (self->context, TLS1_2_VERSION) ||
      !SSL_CTX_set_max_proto_version (self->context, TLS1_2_VERSION) ||
      !SSL_CTX_set_cipher_list (self->context, "PSK-AES128-GCM-SHA256"))
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
  if (self->default_provider)
    OSSL_PROVIDER_unload (self->default_provider);
  if (self->rand_provider)
    OSSL_PROVIDER_unload (self->rand_provider);
  OSSL_LIB_CTX_free (self->library);
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
