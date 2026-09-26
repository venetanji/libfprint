/*
 * TLS session management for Validity/Synaptics VCSFW fingerprint sensors
 *
 * Copyright (C) 2024 libfprint contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity_pack.h"
#include "validity.h"
#include "validity_tls.h"
#include "vcsfw_protocol.h"

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <string.h>

/* ================================================================
 * TLS PRF (P_SHA256) — Standard TLS 1.2 PRF with HMAC-SHA256
 * ================================================================ */

void
validity_tls_prf (const guint8 *secret,
                  gsize         secret_len,
                  const guint8 *seed,
                  gsize         seed_len,
                  guint8       *output,
                  gsize         output_len)
{
  guint n = (output_len + 31) / 32;
  guint8 a[32];         /* A(i) */
  guint8 p_hash[32];    /* P_hash iteration output */
  gsize pos = 0;
  size_t hmac_len;

  EVP_MAC *mac = EVP_MAC_fetch (NULL, "HMAC", NULL);
  OSSL_PARAM prf_params[] = {
    OSSL_PARAM_construct_utf8_string ("digest", (char *) "SHA256", 0),
    OSSL_PARAM_construct_end (),
  };

  /* A(1) = HMAC(secret, seed) */
  EVP_MAC_CTX *ctx = EVP_MAC_CTX_new (mac);

  EVP_MAC_init (ctx, secret, secret_len, prf_params);
  EVP_MAC_update (ctx, seed, seed_len);
  EVP_MAC_final (ctx, a, &hmac_len, sizeof (a));
  EVP_MAC_CTX_free (ctx);

  for (guint i = 0; i < n; i++)
    {
      /* P_hash = HMAC(secret, A(i) || seed) */
      ctx = EVP_MAC_CTX_new (mac);
      EVP_MAC_init (ctx, secret, secret_len, prf_params);
      EVP_MAC_update (ctx, a, 32);
      EVP_MAC_update (ctx, seed, seed_len);
      EVP_MAC_final (ctx, p_hash, &hmac_len, sizeof (p_hash));
      EVP_MAC_CTX_free (ctx);

      gsize to_copy = MIN (32, output_len - pos);
      memcpy (output + pos, p_hash, to_copy);
      pos += to_copy;

      /* A(i+1) = HMAC(secret, A(i)) */
      ctx = EVP_MAC_CTX_new (mac);
      EVP_MAC_init (ctx, secret, secret_len, prf_params);
      EVP_MAC_update (ctx, a, 32);
      EVP_MAC_final (ctx, a, &hmac_len, sizeof (a));
      EVP_MAC_CTX_free (ctx);
    }

  EVP_MAC_free (mac);

  OPENSSL_cleanse (a, sizeof (a));
  OPENSSL_cleanse (p_hash, sizeof (p_hash));
}

/* ================================================================
 * Init / Free
 * ================================================================ */

void
validity_tls_init (ValidityTlsState *tls)
{
  memset (tls, 0, sizeof (ValidityTlsState));
}

void
validity_tls_free (ValidityTlsState *tls)
{
  OPENSSL_cleanse (tls->sign_key, sizeof (tls->sign_key));
  OPENSSL_cleanse (tls->validation_key, sizeof (tls->validation_key));
  OPENSSL_cleanse (tls->encryption_key, sizeof (tls->encryption_key));
  OPENSSL_cleanse (tls->decryption_key, sizeof (tls->decryption_key));
  OPENSSL_cleanse (tls->psk_encryption_key, sizeof (tls->psk_encryption_key));
  OPENSSL_cleanse (tls->psk_validation_key, sizeof (tls->psk_validation_key));
  OPENSSL_cleanse (tls->master_secret, sizeof (tls->master_secret));

  g_clear_pointer (&tls->handshake_hash, g_checksum_free);
  g_clear_pointer (&tls->tls_cert, g_free);
  g_clear_pointer (&tls->priv_blob, g_free);
  g_clear_pointer (&tls->ecdh_blob, g_free);

  if (tls->session_key)
    EVP_PKEY_free (tls->session_key);
  tls->session_key = NULL;

  if (tls->priv_key)
    EVP_PKEY_free (tls->priv_key);
  tls->priv_key = NULL;

  if (tls->ecdh_q)
    EVP_PKEY_free (tls->ecdh_q);
  tls->ecdh_q = NULL;
}

/* ================================================================
 * PSK derivation from hardware identity (DMI)
 *
 * The two "PSK" keys are not TLS session keys. Pairing generates a
 * P-256 key pair on the host and stores the private key in the
 * sensor's flash (partition 1, TLS block 4), encrypted with
 * psk_encryption_key (AES-256-CBC) and authenticated with
 * psk_validation_key (HMAC-SHA256); see validity_pair_encrypt_key()
 * and handle_priv_block(). That stored key is only used to sign the
 * TLS CertificateVerify message. The session keys come from ECDH with
 * an ephemeral key generated for every handshake (see
 * validity_tls_build_client_finish()), not from the stored key.
 *
 * The inputs are the same as python-validity's (validitysensor/tls.py,
 * Tls.__init__ and Tls.set_hwkey):
 *   - SMBIOS type 1 "Product Name":  /sys/class/dmi/id/product_name
 *     (world-readable)
 *   - SMBIOS type 1 "Serial Number": /sys/class/dmi/id/product_serial
 *     (readable by root only)
 * with surrounding whitespace stripped, combined as name NUL serial
 * NUL. Nothing here is vendor-specific. The derivation has to stay
 * identical to python-validity's, and to the Windows driver's where it
 * pairs the same way, or sensors that are already paired stop opening.
 *
 * If either file cannot be read, both values are replaced with
 * "VirtualBox" and "0", as python-validity does. A blank SMBIOS string
 * reads back as a bare newline; that is not a read failure, and the
 * value is used as an empty string.
 *
 * The identity ties a pairing to the machine. If either value changes
 * (system board replaced without the serial reprogrammed, sensor moved
 * to another machine), the stored key no longer verifies and the
 * sensor has to be paired again, which erases its templates.
 * ================================================================ */

void
validity_tls_derive_psk_from_identity (ValidityTlsState *tls,
                                       const gchar      *product_name,
                                       const gchar      *product_serial)
{
  /* hw_key = product_name + '\0' + serial_number + '\0' */
  gsize name_len = strlen (product_name);
  gsize serial_len = strlen (product_serial);
  gsize hw_key_len = name_len + 1 + serial_len + 1;
  g_autofree guint8 *hw_key = g_malloc (hw_key_len);

  memcpy (hw_key, product_name, name_len);
  hw_key[name_len] = '\0';
  memcpy (hw_key + name_len + 1, product_serial, serial_len);
  hw_key[name_len + 1 + serial_len] = '\0';

  /* psk_encryption_key = PRF(password, "GWK" || hw_key, 0x20) */
  gsize seed_len = 3 + hw_key_len;
  g_autofree guint8 *seed = g_malloc (seed_len);
  memcpy (seed, "GWK", 3);
  memcpy (seed + 3, hw_key, hw_key_len);
  validity_tls_prf (tls->password, tls->password_len, seed, seed_len,
                    tls->psk_encryption_key, 32);

  /* psk_validation_key = PRF(psk_encryption_key, "GWK_SIGN" || gwk_sign, 0x20) */
  gsize seed2_len = 8 + tls->gwk_sign_len;
  g_autofree guint8 *seed2 = g_malloc (seed2_len);
  memcpy (seed2, "GWK_SIGN", 8);
  memcpy (seed2 + 8, tls->gwk_sign, tls->gwk_sign_len);
  validity_tls_prf (tls->psk_encryption_key, 32, seed2, seed2_len,
                    tls->psk_validation_key, 32);

  /* With the public password and gwk_sign, these are enough to
   * recompute the PSK. */
  OPENSSL_cleanse (hw_key, hw_key_len);
  OPENSSL_cleanse (seed, seed_len);
}

void
validity_tls_derive_psk_from_files (ValidityTlsState *tls,
                                    const gchar      *name_path,
                                    const gchar      *serial_path)
{
  g_autofree gchar *product_name = NULL;
  g_autofree gchar *product_serial = NULL;
  gsize serial_size = 0;

  g_autoptr(GError) error = NULL;

  /* python-validity reads both files in one try block and falls back
   * for both if either read fails; do the same so that both derive the
   * same key on the same machine. */
  if (!g_file_get_contents (name_path, &product_name, NULL, &error) ||
      !g_file_get_contents (serial_path, &product_serial, &serial_size,
                            &error))
    {
      fp_warn ("Cannot read the DMI product name and serial (%s); using "
               "python-validity's fallback identity \"VirtualBox\"/\"0\". "
               "A sensor paired under this machine's real identity cannot "
               "set up its TLS session.", error->message);
      validity_tls_derive_psk_from_identity (tls, "VirtualBox", "0");
      return;
    }

  g_strstrip (product_name);
  g_strstrip (product_serial);

  validity_tls_derive_psk_from_identity (tls, product_name, product_serial);

  /* Never log the serial itself, only whether there is one. */
  fp_info ("PSK derived from DMI: product=\"%s\", serial %s",
           product_name, *product_serial ? "present" : "empty");

  OPENSSL_cleanse (product_serial, serial_size);
}

void
validity_tls_derive_psk (ValidityTlsState *tls)
{
  validity_tls_derive_psk_from_files (tls,
                                      "/sys/class/dmi/id/product_name",
                                      "/sys/class/dmi/id/product_serial");
}

/* ================================================================
 * Padding (VCSFW variant: pad byte = len-1, not len)
 * ================================================================ */

static guint8 *
tls_pad (const guint8 *data, gsize data_len, gsize *padded_len)
{
  gsize pad_count = TLS_AES_BLOCK_SIZE - (data_len % TLS_AES_BLOCK_SIZE);
  guint8 pad_byte = (guint8) (pad_count - 1);

  *padded_len = data_len + pad_count;
  guint8 *out = g_malloc (*padded_len);
  memcpy (out, data, data_len);
  memset (out + data_len, pad_byte, pad_count);
  return out;
}

static guint8 *
tls_unpad (const guint8 *data, gsize data_len, gsize *unpadded_len,
           GError **error)
{
  if (data_len == 0 || data_len % TLS_AES_BLOCK_SIZE != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS decrypt: invalid ciphertext length");
      return NULL;
    }

  gsize pad_count = 1 + data[data_len - 1];
  if (pad_count > data_len || pad_count > TLS_AES_BLOCK_SIZE)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS decrypt: invalid padding");
      return NULL;
    }

  *unpadded_len = data_len - pad_count;
  if (*unpadded_len == 0)
    {
      /* All-padding record carries no plaintext. Reject with an error set:
       * g_memdup2(data, 0) returns NULL, and a NULL return with *error unset
       * reaches fpi_ssm_mark_failed(NULL), which aborts the daemon. */
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS decrypt: empty plaintext record");
      return NULL;
    }
  guint8 *out = g_memdup2 (data, *unpadded_len);
  return out;
}

/* ================================================================
 * HMAC sign / validate
 * ================================================================ */

static void
tls_hmac_sign (const guint8 *key, guint8 content_type,
               const guint8 *data, gsize data_len,
               guint8 *mac_out)
{
  guint8 hdr[5];
  size_t mac_len;

  validity_pack (hdr, sizeof (hdr), "bbbH",
                 content_type, TLS_VERSION_MAJOR, TLS_VERSION_MINOR,
                 (guint16) data_len);

  /* HMAC(key, hdr || data) using EVP_MAC API (OpenSSL 3.0+) */
  EVP_MAC *mac = EVP_MAC_fetch (NULL, "HMAC", NULL);
  EVP_MAC_CTX *ctx = EVP_MAC_CTX_new (mac);
  OSSL_PARAM hmac_params[] = {
    OSSL_PARAM_construct_utf8_string ("digest", (char *) "SHA256", 0),
    OSSL_PARAM_construct_end (),
  };
  EVP_MAC_init (ctx, key, TLS_AES_KEY_SIZE, hmac_params);
  EVP_MAC_update (ctx, hdr, sizeof (hdr));
  EVP_MAC_update (ctx, data, data_len);
  EVP_MAC_final (ctx, mac_out, &mac_len, TLS_HMAC_SIZE);
  EVP_MAC_CTX_free (ctx);
  EVP_MAC_free (mac);
}

static gboolean
tls_hmac_validate (const guint8 *key, guint8 content_type,
                   const guint8 *data, gsize data_len,
                   const guint8 *expected_mac, GError **error)
{
  guint8 computed[TLS_HMAC_SIZE];

  tls_hmac_sign (key, content_type, data, data_len, computed);

  if (CRYPTO_memcmp (computed, expected_mac, TLS_HMAC_SIZE) != 0)
    {
      OPENSSL_cleanse (computed, sizeof (computed));
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS HMAC validation failed");
      return FALSE;
    }

  OPENSSL_cleanse (computed, sizeof (computed));
  return TRUE;
}

/* ================================================================
 * AES-256-CBC encrypt / decrypt
 * ================================================================ */

guint8 *
validity_tls_encrypt (ValidityTlsState *tls,
                      const guint8     *plaintext,
                      gsize             plaintext_len,
                      gsize            *out_len)
{
  g_autofree guint8 *padded = NULL;
  gsize padded_len;
  guint8 iv[TLS_IV_SIZE];
  int enc_len, final_len;

  padded = tls_pad (plaintext, plaintext_len, &padded_len);

  RAND_bytes (iv, TLS_IV_SIZE);

  *out_len = TLS_IV_SIZE + padded_len;
  guint8 *output = g_malloc (*out_len);
  memcpy (output, iv, TLS_IV_SIZE);

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  EVP_EncryptInit_ex (ctx, EVP_aes_256_cbc (), NULL,
                      tls->encryption_key, iv);
  EVP_CIPHER_CTX_set_padding (ctx, 0);  /* We handle padding ourselves */
  EVP_EncryptUpdate (ctx, output + TLS_IV_SIZE, &enc_len,
                     padded, padded_len);
  EVP_EncryptFinal_ex (ctx, output + TLS_IV_SIZE + enc_len, &final_len);
  EVP_CIPHER_CTX_free (ctx);

  return output;
}

guint8 *
validity_tls_decrypt (ValidityTlsState *tls,
                      const guint8     *ciphertext,
                      gsize             ciphertext_len,
                      gsize            *out_len,
                      GError          **error)
{
  if (!ciphertext || ciphertext_len < TLS_IV_SIZE + TLS_AES_BLOCK_SIZE)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS ciphertext too short");
      return NULL;
    }

  const guint8 *iv = ciphertext;
  const guint8 *enc_data = ciphertext + TLS_IV_SIZE;
  gsize enc_len = ciphertext_len - TLS_IV_SIZE;

  /* CBC ciphertext is always a whole number of blocks. Reject otherwise so a
   * partial final block can't leave the decrypt output length undefined. */
  if (enc_len % TLS_AES_BLOCK_SIZE != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS ciphertext not block-aligned");
      return NULL;
    }

  guint8 *decrypted = g_malloc (enc_len);
  int dec_len = 0, final_len = 0;
  gboolean ok = FALSE;

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  if (ctx &&
      EVP_DecryptInit_ex (ctx, EVP_aes_256_cbc (), NULL,
                          tls->decryption_key, iv) == 1)
    {
      EVP_CIPHER_CTX_set_padding (ctx, 0);
      ok = EVP_DecryptUpdate (ctx, decrypted, &dec_len, enc_data, enc_len) == 1 &&
           EVP_DecryptFinal_ex (ctx, decrypted + dec_len, &final_len) == 1;
    }
  EVP_CIPHER_CTX_free (ctx);

  if (!ok)
    {
      g_free (decrypted);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS decrypt: CBC decryption failed");
      return NULL;
    }

  gsize total_dec = dec_len + final_len;
  guint8 *unpadded = tls_unpad (decrypted, total_dec, out_len, error);
  g_free (decrypted);

  return unpadded;
}

/* ================================================================
 * App data wrap/unwrap
 * ================================================================ */

guint8 *
validity_tls_wrap_app_data (ValidityTlsState *tls,
                            const guint8     *cmd,
                            gsize             cmd_len,
                            gsize            *out_len)
{
  /* Sign: plaintext || HMAC(sign_key, hdr || plaintext) */
  gsize signed_len = cmd_len + TLS_HMAC_SIZE;
  guint8 *signed_data = g_malloc (signed_len);

  memcpy (signed_data, cmd, cmd_len);
  tls_hmac_sign (tls->sign_key, TLS_CONTENT_APP_DATA,
                 cmd, cmd_len, signed_data + cmd_len);

  /* Encrypt */
  gsize enc_len;
  guint8 *encrypted = validity_tls_encrypt (tls, signed_data, signed_len,
                                            &enc_len);
  g_free (signed_data);

  /* Wrap in TLS record: type(1) || version(2) || length(2) || encrypted */
  guint8 *record = validity_pack_new (out_len, "bbbHd",
                                      TLS_CONTENT_APP_DATA,
                                      TLS_VERSION_MAJOR,
                                      TLS_VERSION_MINOR,
                                      (guint16) enc_len,
                                      encrypted, (gsize) enc_len);
  g_free (encrypted);

  return record;
}

guint8 *
validity_tls_unwrap_response (ValidityTlsState *tls,
                              const guint8     *response,
                              gsize             response_len,
                              gsize            *out_len,
                              GError          **error)
{
  GByteArray *app_data = g_byte_array_new ();
  FpiByteReader r;

  fpi_byte_reader_init (&r, response, response_len);

  while (fpi_byte_reader_get_remaining (&r) > 0)
    {
      if (fpi_byte_reader_get_remaining (&r) < 5)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS response: truncated record header");
          g_byte_array_free (app_data, TRUE);
          return NULL;
        }

      guint8 content_type = 0, ver_major = 0, ver_minor = 0;
      guint16 rec_len = 0;
      fpi_byte_reader_get_uint8 (&r, &content_type);
      fpi_byte_reader_get_uint8 (&r, &ver_major);
      fpi_byte_reader_get_uint8 (&r, &ver_minor);
      fpi_byte_reader_get_uint16_be (&r, &rec_len);

      if (ver_major != TLS_VERSION_MAJOR || ver_minor != TLS_VERSION_MINOR)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS response: unexpected version %d.%d",
                       ver_major, ver_minor);
          g_byte_array_free (app_data, TRUE);
          return NULL;
        }

      if (rec_len > fpi_byte_reader_get_remaining (&r))
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS response: record length exceeds data");
          g_byte_array_free (app_data, TRUE);
          return NULL;
        }

      const guint8 *rec_data = NULL;
      fpi_byte_reader_get_data (&r, rec_len, &rec_data);

      if (content_type == TLS_CONTENT_CHANGE_CIPHER)
        {
          if (rec_len != 1 || rec_data[0] != 0x01)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS response: bad ChangeCipherSpec");
              g_byte_array_free (app_data, TRUE);
              return NULL;
            }
          tls->secure_rx = TRUE;
        }
      else if (content_type == TLS_CONTENT_HANDSHAKE)
        {
          /* Handshake records during server finish — handled by caller */
          /* If secure_rx, this needs to be decrypted first */
        }
      else if (content_type == TLS_CONTENT_APP_DATA)
        {
          if (!tls->secure_rx)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS app data before secure channel");
              g_byte_array_free (app_data, TRUE);
              return NULL;
            }

          /* Decrypt */
          gsize dec_len;
          guint8 *decrypted = validity_tls_decrypt (tls, rec_data, rec_len,
                                                    &dec_len, error);
          if (!decrypted)
            {
              g_byte_array_free (app_data, TRUE);
              return NULL;
            }

          /* Validate HMAC: data is plaintext(n) || mac(32) */
          if (dec_len < TLS_HMAC_SIZE)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS app data too short for HMAC");
              g_free (decrypted);
              g_byte_array_free (app_data, TRUE);
              return NULL;
            }

          gsize plain_len = dec_len - TLS_HMAC_SIZE;
          if (!tls_hmac_validate (tls->validation_key, TLS_CONTENT_APP_DATA,
                                  decrypted, plain_len,
                                  decrypted + plain_len, error))
            {
              g_free (decrypted);
              g_byte_array_free (app_data, TRUE);
              return NULL;
            }

          g_byte_array_append (app_data, decrypted, plain_len);
          g_free (decrypted);
        }
      else
        {
          /* Content type 0x15 is a TLS alert: the chip tore down its session
           * (e.g. after an internal reset). Flag it so the recovery layer can
           * treat it as a recoverable session loss rather than a hard error. */
          if (content_type == 0x15)
            tls->session_broken = TRUE;
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS response: unknown content type 0x%02x",
                       content_type);
          g_byte_array_free (app_data, TRUE);
          return NULL;
        }
    }

  *out_len = app_data->len;
  return g_byte_array_free (app_data, FALSE);
}

/* ================================================================
 * Flash TLS data parsing
 * ================================================================ */

/* Helper: create EC public key from raw x,y coordinates (little-endian) */
static EVP_PKEY *
ec_pubkey_from_coords (const guint8 *x_le, const guint8 *y_le)
{
  /* Convert little-endian to big-endian */
  guint8 x_be[TLS_EC_COORD_SIZE];
  guint8 y_be[TLS_EC_COORD_SIZE];

  for (gsize i = 0; i < TLS_EC_COORD_SIZE; i++)
    {
      x_be[i] = x_le[TLS_EC_COORD_SIZE - 1 - i];
      y_be[i] = y_le[TLS_EC_COORD_SIZE - 1 - i];
    }

  /* Build uncompressed point: 0x04 || x || y */
  guint8 pubpoint[1 + 2 * TLS_EC_COORD_SIZE];
  pubpoint[0] = 0x04;
  memcpy (pubpoint + 1, x_be, TLS_EC_COORD_SIZE);
  memcpy (pubpoint + 1 + TLS_EC_COORD_SIZE, y_be, TLS_EC_COORD_SIZE);

  EVP_PKEY *pkey = NULL;
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new ();
  OSSL_PARAM_BLD_push_utf8_string (bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                   "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string (bld, OSSL_PKEY_PARAM_PUB_KEY,
                                    pubpoint, sizeof (pubpoint));
  OSSL_PARAM *params = OSSL_PARAM_BLD_to_param (bld);

  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  EVP_PKEY_fromdata_init (ctx);
  EVP_PKEY_fromdata (ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);

  EVP_PKEY_CTX_free (ctx);
  OSSL_PARAM_free (params);
  OSSL_PARAM_BLD_free (bld);

  return pkey;
}

/* Handle private key block (ID 4) — decrypt with PSK */
static gboolean
handle_priv_block (ValidityTlsState *tls,
                   const guint8     *body,
                   gsize             body_len,
                   GError          **error)
{
  if (body_len < 1 + TLS_IV_SIZE + TLS_AES_BLOCK_SIZE + TLS_HMAC_SIZE)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key block too short");
      return FALSE;
    }

  if (body[0] != 0x02)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: unknown private key prefix 0x%02x", body[0]);
      return FALSE;
    }

  /* Save raw blob for pairing reuse */
  g_free (tls->priv_blob);
  tls->priv_blob = g_memdup2 (body, body_len);
  tls->priv_blob_len = body_len;

  const guint8 *payload = body + 1;
  gsize payload_len = body_len - 1;

  /* payload = ciphertext || hmac(32) */
  const guint8 *ct = payload;
  gsize ct_len = payload_len - TLS_HMAC_SIZE;
  const guint8 *stored_mac = payload + ct_len;

  /* Verify HMAC with psk_validation_key */
  guint8 computed_mac[TLS_HMAC_SIZE];
  size_t hmac_out_len;
  {
    EVP_MAC *hmac_mac = EVP_MAC_fetch (NULL, "HMAC", NULL);
    EVP_MAC_CTX *hmac_ctx = EVP_MAC_CTX_new (hmac_mac);
    OSSL_PARAM hmac_params[] = {
      OSSL_PARAM_construct_utf8_string ("digest", (char *) "SHA256", 0),
      OSSL_PARAM_construct_end (),
    };
    EVP_MAC_init (hmac_ctx, tls->psk_validation_key, TLS_AES_KEY_SIZE,
                  hmac_params);
    EVP_MAC_update (hmac_ctx, ct, ct_len);
    EVP_MAC_final (hmac_ctx, computed_mac, &hmac_out_len, TLS_HMAC_SIZE);
    EVP_MAC_CTX_free (hmac_ctx);
    EVP_MAC_free (hmac_mac);
  }

  /* The block passed its SHA-256 check in parse_flash, so an HMAC failure
   * means it was wrapped under a different PSK. The PSK's other inputs are
   * fixed data-package constants, so that points to another host identity. */
  if (CRYPTO_memcmp (computed_mac, stored_mac, TLS_HMAC_SIZE) != 0)
    {
      tls->priv_key_mismatch = TRUE;
      OPENSSL_cleanse (computed_mac, sizeof (computed_mac));
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key HMAC failed — "
                   "device may be paired with another machine");
      return FALSE;
    }

  /* Decrypt with psk_encryption_key: iv(16) || encrypted */
  const guint8 *iv = ct;
  const guint8 *enc_data = ct + TLS_IV_SIZE;
  gsize enc_len = ct_len - TLS_IV_SIZE;

  if (enc_len % TLS_AES_BLOCK_SIZE != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key blob not block-aligned");
      return FALSE;
    }

  guint8 *decrypted = g_malloc (enc_len);
  int dec_len = 0, final_len = 0;
  gboolean dec_ok = FALSE;

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  if (ctx &&
      EVP_DecryptInit_ex (ctx, EVP_aes_256_cbc (), NULL,
                          tls->psk_encryption_key, iv) == 1)
    {
      EVP_CIPHER_CTX_set_padding (ctx, 0);
      dec_ok = EVP_DecryptUpdate (ctx, decrypted, &dec_len, enc_data, enc_len) == 1 &&
               EVP_DecryptFinal_ex (ctx, decrypted + dec_len, &final_len) == 1;
    }
  EVP_CIPHER_CTX_free (ctx);

  if (!dec_ok)
    {
      g_free (decrypted);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key CBC decryption failed");
      return FALSE;
    }

  gsize total_dec = dec_len + final_len;

  /* Standard unpad (last byte = pad count, not VCSFW variant) */
  if (total_dec == 0)
    {
      g_free (decrypted);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key decrypt yielded empty data");
      return FALSE;
    }
  gsize unpad = decrypted[total_dec - 1];
  if (unpad == 0 || unpad > TLS_AES_BLOCK_SIZE || unpad > total_dec)
    {
      g_free (decrypted);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key bad padding");
      return FALSE;
    }
  gsize key_data_len = total_dec - unpad;

  /* key_data = x(32 LE) || y(32 LE) || d(32 LE) [+ possible extra] */
  if (key_data_len < 3 * TLS_EC_COORD_SIZE)
    {
      g_free (decrypted);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: private key data too short");
      return FALSE;
    }

  const guint8 *d_le = decrypted + 2 * TLS_EC_COORD_SIZE;

  /* Use derive_private_key approach (ignoring x,y which may be zeros) */
  guint8 d_be[TLS_EC_COORD_SIZE];
  for (gsize i = 0; i < TLS_EC_COORD_SIZE; i++)
    d_be[i] = d_le[TLS_EC_COORD_SIZE - 1 - i];

  BIGNUM *d_bn = BN_bin2bn (d_be, TLS_EC_COORD_SIZE, NULL);

  /* Derive public key Q = d * G on P-256 */
  EC_GROUP *group = EC_GROUP_new_by_curve_name (NID_X9_62_prime256v1);
  EC_POINT *pub_pt = EC_POINT_new (group);
  EC_POINT_mul (group, pub_pt, d_bn, NULL, NULL, NULL);

  guint8 pub_uncompressed[1 + 2 * TLS_EC_COORD_SIZE]; /* 0x04 || x || y */
  size_t pt_len = EC_POINT_point2oct (group, pub_pt,
                                      POINT_CONVERSION_UNCOMPRESSED,
                                      pub_uncompressed,
                                      sizeof (pub_uncompressed), NULL);
  EC_POINT_free (pub_pt);
  EC_GROUP_free (group);

  EVP_PKEY *pkey = NULL;
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new ();
  OSSL_PARAM_BLD_push_utf8_string (bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                   "prime256v1", 0);
  OSSL_PARAM_BLD_push_BN (bld, OSSL_PKEY_PARAM_PRIV_KEY, d_bn);
  OSSL_PARAM_BLD_push_octet_string (bld, OSSL_PKEY_PARAM_PUB_KEY,
                                    pub_uncompressed, pt_len);

  OSSL_PARAM *params = OSSL_PARAM_BLD_to_param (bld);
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  EVP_PKEY_fromdata_init (pctx);

  int rc = EVP_PKEY_fromdata (pctx, &pkey, EVP_PKEY_KEYPAIR, params);

  EVP_PKEY_CTX_free (pctx);
  OSSL_PARAM_free (params);
  OSSL_PARAM_BLD_free (bld);
  BN_free (d_bn);
  OPENSSL_cleanse (decrypted, total_dec);
  g_free (decrypted);
  OPENSSL_cleanse (d_be, sizeof (d_be));

  if (rc != 1 || !pkey)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: failed to create EC private key");
      return FALSE;
    }

  if (tls->priv_key)
    EVP_PKEY_free (tls->priv_key);
  tls->priv_key = pkey;

  fp_dbg ("TLS flash: private key loaded");
  return TRUE;
}

/* Handle ECDH block (ID 6) — extract sensor's ECDH public key */
static gboolean
handle_ecdh_block (ValidityTlsState *tls,
                   const guint8     *body,
                   gsize             body_len,
                   GError          **error)
{
  if (body_len < TLS_ECDH_BLOB_SIZE + 4)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: ECDH block too short");
      return FALSE;
    }

  /* Save raw blob */
  g_free (tls->ecdh_blob);
  tls->ecdh_blob = g_memdup2 (body, body_len);
  tls->ecdh_blob_len = body_len;

  const guint8 *key_blob = body;
  const guint8 *sig_section = body + TLS_ECDH_BLOB_SIZE;
  gsize sig_section_len = body_len - TLS_ECDH_BLOB_SIZE;

  /* Extract x,y from key blob (little-endian) */
  const guint8 *x_le = key_blob + TLS_ECDH_X_OFFSET;
  const guint8 *y_le = key_blob + TLS_ECDH_Y_OFFSET;

  EVP_PKEY *ecdh_pubkey = ec_pubkey_from_coords (x_le, y_le);
  if (!ecdh_pubkey)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: failed to create ECDH public key");
      return FALSE;
    }

  /* Verify key blob signature with firmware public key */
  guint32 sig_len_field;
  if (sig_section_len < 4)
    {
      EVP_PKEY_free (ecdh_pubkey);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: ECDH signature section too short");
      return FALSE;
    }
  validity_unpack (sig_section, sig_section_len, "w", &sig_len_field);
  const guint8 *signature = sig_section + 4;

  if (sig_len_field > sig_section_len - 4)
    {
      EVP_PKEY_free (ecdh_pubkey);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: ECDH signature length invalid");
      return FALSE;
    }

  /* Create firmware verification public key */
  EVP_PKEY *fw_pubkey = ec_pubkey_from_coords (tls->fw_pubkey_x, tls->fw_pubkey_y);
  if (!fw_pubkey)
    {
      EVP_PKEY_free (ecdh_pubkey);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: failed to create firmware pubkey");
      return FALSE;
    }

  /* Note: fw_pubkey_x/y are loaded at runtime from data files (little-endian coords) */

  /* Verify: fwpub.verify(signature, key_blob, ECDSA(SHA256)) */
  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new ();
  int verify_ok = 0;
  if (EVP_DigestVerifyInit (md_ctx, NULL, EVP_sha256 (), NULL, fw_pubkey) == 1 &&
      EVP_DigestVerify (md_ctx, signature, sig_len_field,
                        key_blob, TLS_ECDH_BLOB_SIZE) == 1)
    verify_ok = 1;

  EVP_MD_CTX_free (md_ctx);
  EVP_PKEY_free (fw_pubkey);

  if (!verify_ok)
    {
      /* Do NOT free ecdh_pubkey here: it is stored below and used for the
       * ECDH handshake. Freeing it (then storing the dangling pointer) is a
       * use-after-free / double-free. The signature check is non-fatal to
       * match python-validity, which also proceeds on some devices whose
       * blobs do not verify against the shipped firmware public key.
       * SECURITY: leaving this non-fatal means the sensor's ECDH key is not
       * authenticated; see the driver notes on the fw_pubkey data files. */
      fp_warn ("TLS flash: ECDH blob signature verification failed "
               "(non-fatal, continuing)");
    }

  if (tls->ecdh_q)
    EVP_PKEY_free (tls->ecdh_q);
  tls->ecdh_q = ecdh_pubkey;

  fp_dbg ("TLS flash: ECDH public key loaded");
  return TRUE;
}

/* Handle cert block (ID 3) */
static gboolean
handle_cert_block (ValidityTlsState *tls,
                   const guint8     *body,
                   gsize             body_len)
{
  g_free (tls->tls_cert);
  tls->tls_cert = g_memdup2 (body, body_len);
  tls->tls_cert_len = body_len;
  fp_dbg ("TLS flash: certificate loaded (%zu bytes)", body_len);
  return TRUE;
}

gboolean
validity_tls_parse_flash (ValidityTlsState *tls,
                          const guint8     *data,
                          gsize             data_len,
                          GError          **error)
{
  FpiByteReader r;

  tls->priv_key_mismatch = FALSE;
  fpi_byte_reader_init (&r, data, data_len);

  while (fpi_byte_reader_get_remaining (&r) >= TLS_FLASH_BLOCK_HEADER_SIZE)
    {
      guint16 block_id = 0, block_size = 0;
      const guint8 *stored_hash = NULL;
      fpi_byte_reader_get_uint16_le (&r, &block_id);
      fpi_byte_reader_get_uint16_le (&r, &block_size);
      fpi_byte_reader_get_data (&r, 32, &stored_hash);

      if (block_id == TLS_FLASH_BLOCK_END)
        break;

      if (block_size > fpi_byte_reader_get_remaining (&r))
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS flash: block 0x%04x size %u exceeds remaining %u",
                       block_id, block_size, fpi_byte_reader_get_remaining (&r));
          return FALSE;
        }

      const guint8 *body = NULL;
      fpi_byte_reader_get_data (&r, block_size, &body);

      /* Verify SHA-256 hash */
      guint8 computed_hash[32];
      GChecksum *cs = g_checksum_new (G_CHECKSUM_SHA256);
      gsize hash_len = 32;
      g_checksum_update (cs, body, block_size);
      g_checksum_get_digest (cs, computed_hash, &hash_len);
      g_checksum_free (cs);

      if (memcmp (computed_hash, stored_hash, 32) != 0)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS flash: hash mismatch for block 0x%04x", block_id);
          return FALSE;
        }

      switch (block_id)
        {
        case TLS_FLASH_BLOCK_EMPTY0:
        case TLS_FLASH_BLOCK_EMPTY1:
        case TLS_FLASH_BLOCK_EMPTY2:
        case TLS_FLASH_BLOCK_CA_CERT:
          /* Empty or CA cert — skip */
          break;

        case TLS_FLASH_BLOCK_CERT:
          handle_cert_block (tls, body, block_size);
          break;

        case TLS_FLASH_BLOCK_PRIVKEY:
          if (!handle_priv_block (tls, body, block_size, error))
            return FALSE;
          break;

        case TLS_FLASH_BLOCK_ECDH:
          if (!handle_ecdh_block (tls, body, block_size, error))
            return FALSE;
          break;

        default:
          fp_dbg ("TLS flash: skipping unknown block 0x%04x (%u bytes)",
                  block_id, block_size);
          break;
        }

    }

  tls->keys_loaded = (tls->priv_key != NULL && tls->ecdh_q != NULL &&
                      tls->tls_cert != NULL);

  if (!tls->keys_loaded)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS flash: incomplete key data (priv=%d ecdh=%d cert=%d)",
                   tls->priv_key != NULL, tls->ecdh_q != NULL,
                   tls->tls_cert != NULL);
      return FALSE;
    }

  fp_dbg ("TLS flash: all keys loaded successfully");
  return TRUE;
}

/* ================================================================
 * TLS Handshake message builders
 * ================================================================ */

/* Helper: append handshake message header (type + 3-byte length) and
 * update running SHA-256 hash */
static void
hs_append_msg (GByteArray *buf, GChecksum *hash,
               guint8 type, const guint8 *body, gsize body_len)
{
  guint8 hdr[4];

  validity_pack (hdr, sizeof (hdr), "bt", type, (guint32) body_len);

  g_byte_array_append (buf, hdr, 4);
  g_byte_array_append (buf, body, body_len);

  /* Update handshake hash */
  g_checksum_update (hash, hdr, 4);
  g_checksum_update (hash, body, body_len);

  {
    static const char *names[] = {
      [0x01] = "ClientHello", [0x02] = "ServerHello",
      [0x0b] = "Certificate", [0x0d] = "CertRequest",
      [0x0e] = "ServerHelloDone", [0x10] = "ClientKEX",
      [0x0f] = "CertVerify", [0x14] = "Finished"
    };
    const char *n = (type < 0x15 && names[type]) ? names[type] : "unknown";
    fp_dbg ("hs_hash UPDATE %s (type=0x%02x, %zu bytes fed)", n, type, 4 + body_len);
  }
}

/* Build ClientHello */
guint8 *
validity_tls_build_client_hello (ValidityTlsState *tls, gsize *out_len)
{
  /* Reset handshake state */
  g_clear_pointer (&tls->handshake_hash, g_checksum_free);
  tls->handshake_hash = g_checksum_new (G_CHECKSUM_SHA256);
  tls->secure_rx = FALSE;
  tls->secure_tx = FALSE;

  /* Generate client_random */
  RAND_bytes (tls->client_random, TLS_RANDOM_SIZE);

  /* Build ClientHello body */
  GByteArray *hello = g_byte_array_new ();

  /* TLS version */
  guint8 ver[] = { TLS_VERSION_MAJOR, TLS_VERSION_MINOR };
  g_byte_array_append (hello, ver, 2);

  /* client_random */
  g_byte_array_append (hello, tls->client_random, TLS_RANDOM_SIZE);

  /* session_id (7 zero bytes) */
  guint8 sess_id[] = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  g_byte_array_append (hello, sess_id, 8);

  /* cipher suites */
  guint8 suites[] = {
    0x00, 0x06,           /* length = 6 */
    0xC0, 0x05,           /* TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA */
    0x00, 0x3D,           /* TLS_RSA_WITH_AES_256_CBC_SHA256 */
    0x00, 0x8D,           /* TLS_PSK_DHE_WITH_AES_256_CBC_SHA256 */
  };
  g_byte_array_append (hello, suites, sizeof (suites));

  /* compression (none — python-validity sends length=0, no methods) */
  guint8 comp[] = { 0x00 };
  g_byte_array_append (hello, comp, 1);

  /* extensions */
  guint8 ext_truncated_hmac[] = {
    0x00, 0x04,           /* extension type: truncated_hmac(4) */
    0x00, 0x02,           /* length */
    0x00, 0x17,           /* value */
  };
  guint8 ext_ec_points[] = {
    0x00, 0x0B,           /* extension type: ec_point_formats(11) */
    0x00, 0x02,           /* length */
    0x01, 0x00,           /* uncompressed */
  };

  /* extensions length (quirk from python-validity: len(exts) - 2) */
  gsize ext_total = sizeof (ext_truncated_hmac) + sizeof (ext_ec_points);
  guint8 ext_len_hdr[2];
  validity_pack (ext_len_hdr, sizeof (ext_len_hdr), "H",
                 (guint16) (ext_total - 2));
  g_byte_array_append (hello, ext_len_hdr, 2);
  g_byte_array_append (hello, ext_truncated_hmac, sizeof (ext_truncated_hmac));
  g_byte_array_append (hello, ext_ec_points, sizeof (ext_ec_points));

  /* Wrap as handshake message (type 0x01) */
  GByteArray *hs_msg = g_byte_array_new ();
  hs_append_msg (hs_msg, tls->handshake_hash,
                 TLS_HS_CLIENT_HELLO, hello->data, hello->len);
  g_byte_array_free (hello, TRUE);

  /* Wrap in TLS record + 0x44000000 prefix */
  guint8 *output = validity_pack_new (out_len, "WbbbHd",
                                      (guint32) 0x44000000,
                                      TLS_CONTENT_HANDSHAKE,
                                      TLS_VERSION_MAJOR,
                                      TLS_VERSION_MINOR,
                                      (guint16) hs_msg->len,
                                      hs_msg->data, (gsize) hs_msg->len);

  g_byte_array_free (hs_msg, TRUE);
  return output;
}

/* Parse ServerHello response */
gboolean
validity_tls_parse_server_hello (ValidityTlsState *tls,
                                 const guint8     *data,
                                 gsize             data_len,
                                 GError          **error)
{
  FpiByteReader r;

  fpi_byte_reader_init (&r, data, data_len);

  while (fpi_byte_reader_get_remaining (&r) >= 5)
    {
      guint8 content_type = 0;
      guint16 rec_len = 0;
      fpi_byte_reader_get_uint8 (&r, &content_type);
      fpi_byte_reader_skip (&r, 2);  /* version bytes */
      fpi_byte_reader_get_uint16_be (&r, &rec_len);

      if (rec_len > fpi_byte_reader_get_remaining (&r))
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS ServerHello: record exceeds data");
          return FALSE;
        }

      if (content_type == TLS_CONTENT_HANDSHAKE)
        {
          /* Parse handshake messages within this record */
          const guint8 *rec_body = NULL;
          if (!fpi_byte_reader_get_data (&r, rec_len, &rec_body) || !rec_body)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerHello: failed to read record body");
              return FALSE;
            }

          FpiByteReader hs_r;
          fpi_byte_reader_init (&hs_r, rec_body, rec_len);

          while (fpi_byte_reader_get_remaining (&hs_r) >= 4)
            {
              guint hs_msg_start = fpi_byte_reader_get_pos (&hs_r);
              guint8 hs_type = 0;
              guint32 hs_len = 0;
              fpi_byte_reader_get_uint8 (&hs_r, &hs_type);
              fpi_byte_reader_get_uint24_be (&hs_r, &hs_len);

              if (hs_len > fpi_byte_reader_get_remaining (&hs_r))
                {
                  g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                               "TLS ServerHello: handshake msg exceeds record");
                  return FALSE;
                }

              const guint8 *hs_body = NULL;
              if (!fpi_byte_reader_get_data (&hs_r, hs_len, &hs_body) || !hs_body)
                {
                  g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                               "TLS ServerHello: failed to read handshake body");
                  return FALSE;
                }

              /* Update handshake hash */
              const guint8 *hs_raw = rec_body + hs_msg_start;
              g_checksum_update (tls->handshake_hash, hs_raw, 4 + hs_len);

              {
                static const char *names[] = {
                  [0x01] = "ClientHello", [0x02] = "ServerHello",
                  [0x0b] = "Certificate", [0x0d] = "CertRequest",
                  [0x0e] = "ServerHelloDone", [0x10] = "ClientKEX",
                  [0x0f] = "CertVerify", [0x14] = "Finished"
                };
                const char *n = (hs_type < 0x15 && names[hs_type]) ? names[hs_type] : "unknown";
                fp_dbg ("hs_hash UPDATE(srv) %s (type=0x%02x, %u bytes fed)",
                        n, hs_type, (unsigned) (4 + hs_len));
              }

              switch (hs_type)
                {
                case TLS_HS_SERVER_HELLO:
                  {
                    if (hs_len < 2 + TLS_RANDOM_SIZE + 1)
                      {
                        g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                     "TLS ServerHello: message too short");
                        return FALSE;
                      }

                    FpiByteReader sh_r;
                    fpi_byte_reader_init (&sh_r, hs_body, hs_len);

                    guint8 sh_ver_major, sh_ver_minor;
                    fpi_byte_reader_get_uint8 (&sh_r, &sh_ver_major);
                    fpi_byte_reader_get_uint8 (&sh_r, &sh_ver_minor);

                    /* Check version */
                    if (sh_ver_major != TLS_VERSION_MAJOR ||
                        sh_ver_minor != TLS_VERSION_MINOR)
                      {
                        g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                     "TLS ServerHello: unexpected version %d.%d",
                                     sh_ver_major, sh_ver_minor);
                        return FALSE;
                      }

                    const guint8 *server_random_data;
                    fpi_byte_reader_get_data (&sh_r, TLS_RANDOM_SIZE, &server_random_data);
                    memcpy (tls->server_random, server_random_data, TLS_RANDOM_SIZE);

                    guint8 sess_id_len;
                    fpi_byte_reader_get_uint8 (&sh_r, &sess_id_len);
                    fpi_byte_reader_skip (&sh_r, sess_id_len);

                    guint16 suite = 0;
                    fpi_byte_reader_get_uint16_be (&sh_r, &suite);
                    if (suite != TLS_CS_ECDH_ECDSA_AES256_CBC_SHA)
                      {
                        g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                     "TLS ServerHello: unsupported cipher 0x%04x",
                                     suite);
                        return FALSE;
                      }

                    fp_dbg ("TLS ServerHello: cipher 0xC005, version 1.2");
                  }
                  break;

                case TLS_HS_CERT_REQUEST:
                  /* Just validate format */
                  fp_dbg ("TLS CertificateRequest received");
                  break;

                case TLS_HS_SERVER_HELLO_DONE:
                  fp_dbg ("TLS ServerHelloDone received");
                  break;

                default:
                  fp_dbg ("TLS handshake: ignoring type 0x%02x", hs_type);
                  break;
                }
            }
        }
      else
        {
          fpi_byte_reader_skip (&r, rec_len);
        }
    }

  return TRUE;
}

/* Helper: get public key point bytes from EVP_PKEY as uncompressed */
static gboolean
get_ec_pubpoint_bytes (EVP_PKEY *pkey, guint8 *out, gsize out_len)
{
  size_t len = 0;

  if (EVP_PKEY_get_octet_string_param (pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                       NULL, 0, &len) != 1)
    return FALSE;

  if (len > out_len)
    return FALSE;

  return EVP_PKEY_get_octet_string_param (pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                          out, out_len, &len) == 1;
}

/* Build client finish message (Certificate + KeyExchange + CertVerify +
 * ChangeCipherSpec + Finished) */
guint8 *
validity_tls_build_client_finish (ValidityTlsState *tls, gsize *out_len)
{
  GByteArray *output = g_byte_array_new ();

  /* ---- Generate ephemeral ECDH key pair ---- */
  EVP_PKEY *params_key = NULL;
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);

  EVP_PKEY_keygen_init (pctx);
  OSSL_PARAM gen_params[] = {
    OSSL_PARAM_utf8_string (OSSL_PKEY_PARAM_GROUP_NAME, (char *) "prime256v1", 0),
    OSSL_PARAM_END,
  };
  EVP_PKEY_CTX_set_params (pctx, gen_params);
  EVP_PKEY_generate (pctx, &params_key);
  EVP_PKEY_CTX_free (pctx);

  if (tls->session_key)
    EVP_PKEY_free (tls->session_key);
  tls->session_key = params_key;

  /* ---- Derive pre-master secret via ECDH ---- */
  guint8 pre_master_secret[32];
  size_t pms_len = 0;

  EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new (tls->session_key, NULL);
  EVP_PKEY_derive_init (derive_ctx);
  EVP_PKEY_derive_set_peer (derive_ctx, tls->ecdh_q);
  EVP_PKEY_derive (derive_ctx, NULL, &pms_len);
  EVP_PKEY_derive (derive_ctx, pre_master_secret, &pms_len);
  EVP_PKEY_CTX_free (derive_ctx);

  /* ---- Derive master_secret and key_block ---- */
  {
    GString *hex = g_string_new ("TLS server_random: ");
    for (gsize i = 0; i < TLS_RANDOM_SIZE; i++)
      g_string_append_printf (hex, "%02x", tls->server_random[i]);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }
  {
    GChecksum *hc = g_checksum_copy (tls->handshake_hash);
    guint8 hd[32];
    gsize hl = 32;
    g_checksum_get_digest (hc, hd, &hl);
    g_checksum_free (hc);
    GString *hex = g_string_new ("TLS hash after srv: ");
    for (gsize i = 0; i < 32; i++)
      g_string_append_printf (hex, "%02x", hd[i]);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }

  guint8 seed[2 * TLS_RANDOM_SIZE];
  memcpy (seed, tls->client_random, TLS_RANDOM_SIZE);
  memcpy (seed + TLS_RANDOM_SIZE, tls->server_random, TLS_RANDOM_SIZE);

  gsize ms_seed_len = 13 + sizeof (seed);  /* "master secret" + randoms */
  g_autofree guint8 *ms_seed = g_malloc (ms_seed_len);
  memcpy (ms_seed, "master secret", 13);
  memcpy (ms_seed + 13, seed, sizeof (seed));
  validity_tls_prf (pre_master_secret, pms_len, ms_seed, ms_seed_len,
                    tls->master_secret, TLS_MASTER_SECRET_SIZE);

  gsize ke_seed_len = 13 + sizeof (seed);  /* "key expansion" + randoms */
  g_autofree guint8 *ke_seed = g_malloc (ke_seed_len);
  memcpy (ke_seed, "key expansion", 13);
  memcpy (ke_seed + 13, seed, sizeof (seed));
  guint8 key_block[TLS_KEY_BLOCK_SIZE];
  validity_tls_prf (tls->master_secret, TLS_MASTER_SECRET_SIZE,
                    ke_seed, ke_seed_len,
                    key_block, TLS_KEY_BLOCK_SIZE);

  memcpy (tls->sign_key, key_block, TLS_AES_KEY_SIZE);
  memcpy (tls->validation_key, key_block + 0x20, TLS_AES_KEY_SIZE);
  memcpy (tls->encryption_key, key_block + 0x40, TLS_AES_KEY_SIZE);
  memcpy (tls->decryption_key, key_block + 0x60, TLS_AES_KEY_SIZE);

  OPENSSL_cleanse (pre_master_secret, sizeof (pre_master_secret));
  OPENSSL_cleanse (key_block, sizeof (key_block));

  /* ---- Build handshake messages ---- */
  GByteArray *hs_msgs = g_byte_array_new ();

  /* 1. Certificate (type 0x0B) */
  {
    fp_dbg ("TLS cert_len=%zu, cert first 20 bytes:", tls->tls_cert_len);
    {
      GString *hex = g_string_new ("  cert: ");
      gsize dump_len = MIN (tls->tls_cert_len, 40);
      for (gsize i = 0; i < dump_len; i++)
        g_string_append_printf (hex, "%02x", tls->tls_cert[i]);
      fp_dbg ("%s", hex->str);
      g_string_free (hex, TRUE);
    }

    GByteArray *cert_body = g_byte_array_new ();
    guint8 cert_prefix[] = { 0xac, 0x16 };
    g_byte_array_append (cert_body, cert_prefix, 2);
    g_byte_array_append (cert_body, tls->tls_cert, tls->tls_cert_len);

    /* Add two size wrappers (quirk from python-validity: uses tls_cert_len not cert_body->len) */
    guint8 sz1[3];
    validity_pack (sz1, sizeof (sz1), "t", (guint32) tls->tls_cert_len);
    GByteArray *wrapped = g_byte_array_new ();
    g_byte_array_append (wrapped, sz1, 3);
    g_byte_array_append (wrapped, cert_body->data, cert_body->len);
    g_byte_array_free (cert_body, TRUE);

    guint8 sz2[3];
    validity_pack (sz2, sizeof (sz2), "t", (guint32) tls->tls_cert_len);
    GByteArray *wrapped2 = g_byte_array_new ();
    g_byte_array_append (wrapped2, sz2, 3);
    g_byte_array_append (wrapped2, wrapped->data, wrapped->len);
    g_byte_array_free (wrapped, TRUE);

    hs_append_msg (hs_msgs, tls->handshake_hash,
                   TLS_HS_CERTIFICATE, wrapped2->data, wrapped2->len);
    g_byte_array_free (wrapped2, TRUE);
    {
      GChecksum *hc = g_checksum_copy (tls->handshake_hash);
      guint8 hd[32];
      gsize hl = 32;
      g_checksum_get_digest (hc, hd, &hl);
      g_checksum_free (hc);
      GString *hex = g_string_new ("TLS hash after cert: ");
      for (gsize i = 0; i < 32; i++)
        g_string_append_printf (hex, "%02x", hd[i]);
      fp_dbg ("%s", hex->str);
      g_string_free (hex, TRUE);
    }
  }

  /* 2. ClientKeyExchange (type 0x10) */
  {
    guint8 pubpoint[65]; /* 0x04 + 32 + 32 */
    get_ec_pubpoint_bytes (tls->session_key, pubpoint, sizeof (pubpoint));

    /* python-validity sends: 0x04 || to_bytes(x)[::-1] || to_bytes(y)[::-1]
     * to_bytes() returns LE, [::-1] converts back to BE.
     * So python-validity sends BE coordinates.
     * OpenSSL gives us: 0x04 || x_be || y_be — already correct! */
    guint8 *kex_body = pubpoint;  /* Use as-is, it's already BE */
    gsize kex_body_len = 65;

    {
      GString *hex = g_string_new ("TLS kex_body: ");
      for (gsize i = 0; i < 65; i++)
        g_string_append_printf (hex, "%02x", kex_body[i]);
      fp_dbg ("%s", hex->str);
      g_string_free (hex, TRUE);
    }

    hs_append_msg (hs_msgs, tls->handshake_hash,
                   TLS_HS_CLIENT_KEY_EXCHANGE, kex_body, kex_body_len);
    {
      GChecksum *hc = g_checksum_copy (tls->handshake_hash);
      guint8 hd[32];
      gsize hl = 32;
      g_checksum_get_digest (hc, hd, &hl);
      g_checksum_free (hc);
      GString *hex = g_string_new ("TLS hash after kex: ");
      for (gsize i = 0; i < 32; i++)
        g_string_append_printf (hex, "%02x", hd[i]);
      fp_dbg ("%s", hex->str);
      g_string_free (hex, TRUE);
    }
  }

  /* 3. CertificateVerify (type 0x0F) */
  {
    /* Sign SHA-256 of all handshake messages so far */
    GChecksum *hash_copy = g_checksum_copy (tls->handshake_hash);
    guint8 hs_hash[32];
    gsize hash_len = 32;
    g_checksum_get_digest (hash_copy, hs_hash, &hash_len);
    g_checksum_free (hash_copy);

    {
      GString *hex = g_string_new ("TLS hs_hash for CertVerify: ");
      for (gsize i = 0; i < 32; i++)
        g_string_append_printf (hex, "%02x", hs_hash[i]);
      fp_dbg ("%s", hex->str);
      g_string_free (hex, TRUE);
    }

    /* ECDSA sign pre-hashed value */
    size_t sig_len = 0;
    EVP_PKEY_CTX *raw_ctx = EVP_PKEY_CTX_new (tls->priv_key, NULL);
    EVP_PKEY_sign_init (raw_ctx);
    EVP_PKEY_sign (raw_ctx, NULL, &sig_len, hs_hash, 32);
    guint8 *signature = g_malloc (sig_len);
    EVP_PKEY_sign (raw_ctx, signature, &sig_len, hs_hash, 32);
    EVP_PKEY_CTX_free (raw_ctx);

    /* Self-verify the CertVerify signature */
    {
      EVP_PKEY_CTX *vfy_ctx = EVP_PKEY_CTX_new (tls->priv_key, NULL);
      EVP_PKEY_verify_init (vfy_ctx);
      int vrc = EVP_PKEY_verify (vfy_ctx, signature, sig_len, hs_hash, 32);
      fp_dbg ("TLS CertVerify self-verify: %s (rc=%d, sig_len=%zu)",
              vrc == 1 ? "OK" : "FAILED", vrc, sig_len);
      EVP_PKEY_CTX_free (vfy_ctx);
    }

    hs_append_msg (hs_msgs, tls->handshake_hash,
                   TLS_HS_CERT_VERIFY, signature, sig_len);
    g_free (signature);
  }

  /* Wrap handshake messages in TLS record */
  guint8 hs_hdr[5];
  validity_pack (hs_hdr, sizeof (hs_hdr), "bbbH",
                 TLS_CONTENT_HANDSHAKE, TLS_VERSION_MAJOR,
                 TLS_VERSION_MINOR, (guint16) hs_msgs->len);

  /* Start building output with 0x44000000 prefix */
  guint8 prefix[] = { 0x44, 0x00, 0x00, 0x00 };
  g_byte_array_append (output, prefix, TLS_CMD_PREFIX_SIZE);
  g_byte_array_append (output, hs_hdr, 5);
  g_byte_array_append (output, hs_msgs->data, hs_msgs->len);
  g_byte_array_free (hs_msgs, TRUE);

  /* ChangeCipherSpec record */
  guint8 ccs[] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
  g_byte_array_append (output, ccs, sizeof (ccs));

  /* Finished message (encrypted) */
  tls->secure_tx = TRUE;

  GChecksum *hash_copy = g_checksum_copy (tls->handshake_hash);
  guint8 hs_hash[32];
  gsize hash_len = 32;
  g_checksum_get_digest (hash_copy, hs_hash, &hash_len);
  g_checksum_free (hash_copy);

  guint8 verify_data[TLS_VERIFY_DATA_SIZE];
  gsize vd_seed_len = 15 + 32;  /* "client finished" + hash */
  g_autofree guint8 *vd_seed = g_malloc (vd_seed_len);
  memcpy (vd_seed, "client finished", 15);
  memcpy (vd_seed + 15, hs_hash, 32);
  validity_tls_prf (tls->master_secret, TLS_MASTER_SECRET_SIZE,
                    vd_seed, vd_seed_len,
                    verify_data, TLS_VERIFY_DATA_SIZE);

  {
    GString *hex = g_string_new ("TLS Finished hs_hash: ");
    for (gsize i = 0; i < 32; i++)
      g_string_append_printf (hex, "%02x", hs_hash[i]);
    g_string_append_printf (hex, " verify_data: ");
    for (gsize i = 0; i < TLS_VERIFY_DATA_SIZE; i++)
      g_string_append_printf (hex, "%02x", verify_data[i]);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }

  /* Build Finished handshake message: type(1) || 3-byte-len || verify_data */
  guint8 fin_msg[4 + TLS_VERIFY_DATA_SIZE];
  validity_pack (fin_msg, sizeof (fin_msg), "btd",
                 TLS_HS_FINISHED, (guint32) TLS_VERIFY_DATA_SIZE,
                 verify_data, (gsize) TLS_VERIFY_DATA_SIZE);

  /* NOTE: Do NOT update handshake hash with client Finished.
   * python-validity's make_finish() doesn't call update_neg(), and the
   * device's server Finished verify_data is computed WITHOUT including
   * the client Finished in the hash (non-standard TLS behavior). */

  /* Encrypt Finished as handshake record */
  gsize signed_len = sizeof (fin_msg) + TLS_HMAC_SIZE;
  guint8 *signed_data = g_malloc (signed_len);
  memcpy (signed_data, fin_msg, sizeof (fin_msg));
  tls_hmac_sign (tls->sign_key, TLS_CONTENT_HANDSHAKE,
                 fin_msg, sizeof (fin_msg),
                 signed_data + sizeof (fin_msg));

  gsize enc_len;
  guint8 *encrypted = validity_tls_encrypt (tls, signed_data, signed_len,
                                            &enc_len);
  g_free (signed_data);

  /* Wrap encrypted Finished in TLS handshake record */
  guint8 fin_hdr[5];
  validity_pack (fin_hdr, sizeof (fin_hdr), "bbbH",
                 TLS_CONTENT_HANDSHAKE, TLS_VERSION_MAJOR,
                 TLS_VERSION_MINOR, (guint16) enc_len);
  g_byte_array_append (output, fin_hdr, 5);
  g_byte_array_append (output, encrypted, enc_len);
  g_free (encrypted);

  *out_len = output->len;

  /* Debug: hex dump the full client finish */
  {
    GString *hex = g_string_new ("TLS_CF:");
    for (gsize i = 0; i < output->len; i++)
      g_string_append_printf (hex, "%02x", output->data[i]);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }

  return g_byte_array_free (output, FALSE);
}

/* Parse server ChangeCipherSpec + Finished response */
gboolean
validity_tls_parse_server_finish (ValidityTlsState *tls,
                                  const guint8     *data,
                                  gsize             data_len,
                                  GError          **error)
{
  FpiByteReader r;

  fpi_byte_reader_init (&r, data, data_len);
  gboolean got_ccs = FALSE;
  gboolean got_finished = FALSE;

  while (fpi_byte_reader_get_remaining (&r) >= 5)
    {
      guint8 content_type = 0;
      guint16 rec_len = 0;
      fpi_byte_reader_get_uint8 (&r, &content_type);
      fpi_byte_reader_skip (&r, 2);  /* version bytes */
      fpi_byte_reader_get_uint16_be (&r, &rec_len);

      if (rec_len > fpi_byte_reader_get_remaining (&r))
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "TLS ServerFinish: record exceeds data");
          return FALSE;
        }

      const guint8 *rec_data = NULL;
      fpi_byte_reader_get_data (&r, rec_len, &rec_data);

      if (content_type == TLS_CONTENT_CHANGE_CIPHER)
        {
          if (rec_len != 1 || rec_data[0] != 0x01)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinish: bad ChangeCipherSpec");
              return FALSE;
            }
          tls->secure_rx = TRUE;
          got_ccs = TRUE;
        }
      else if (content_type == TLS_CONTENT_HANDSHAKE)
        {
          if (!tls->secure_rx)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinish: encrypted handshake before CCS");
              return FALSE;
            }

          /* Decrypt */
          gsize dec_len;
          guint8 *decrypted = validity_tls_decrypt (tls, rec_data, rec_len,
                                                    &dec_len, error);
          if (!decrypted)
            return FALSE;

          /* Validate HMAC */
          if (dec_len < TLS_HMAC_SIZE)
            {
              g_free (decrypted);
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinished: too short for HMAC");
              return FALSE;
            }
          gsize plain_len = dec_len - TLS_HMAC_SIZE;
          if (!tls_hmac_validate (tls->validation_key, TLS_CONTENT_HANDSHAKE,
                                  decrypted, plain_len,
                                  decrypted + plain_len, error))
            {
              g_free (decrypted);
              return FALSE;
            }

          /* Parse Finished message: type(1) || len(3) || verify_data(12) */
          if (plain_len < 4)
            {
              g_free (decrypted);
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinished: message too short");
              return FALSE;
            }

          FpiByteReader fin_r;
          fpi_byte_reader_init (&fin_r, decrypted, plain_len);

          guint8 fin_type = 0;
          fpi_byte_reader_get_uint8 (&fin_r, &fin_type);
          if (fin_type != TLS_HS_FINISHED)
            {
              g_free (decrypted);
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinished: expected Finished (0x14), got 0x%02x",
                           fin_type);
              return FALSE;
            }

          guint32 vd_len = 0;
          fpi_byte_reader_get_uint24_be (&fin_r, &vd_len);
          if (vd_len != TLS_VERIFY_DATA_SIZE || plain_len < 4 + vd_len)
            {
              g_free (decrypted);
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinished: invalid verify_data length");
              return FALSE;
            }

          const guint8 *received_vd = NULL;
          fpi_byte_reader_get_data (&fin_r, vd_len, &received_vd);

          /* Verify server finished */
          GChecksum *hash_copy = g_checksum_copy (tls->handshake_hash);
          guint8 hs_hash[32];
          gsize hash_size = 32;
          g_checksum_get_digest (hash_copy, hs_hash, &hash_size);
          g_checksum_free (hash_copy);

          guint8 expected_vd[TLS_VERIFY_DATA_SIZE];
          gsize sf_seed_len = 15 + 32;
          g_autofree guint8 *sf_seed = g_malloc (sf_seed_len);
          memcpy (sf_seed, "server finished", 15);
          memcpy (sf_seed + 15, hs_hash, 32);
          validity_tls_prf (tls->master_secret, TLS_MASTER_SECRET_SIZE,
                            sf_seed, sf_seed_len,
                            expected_vd, TLS_VERIFY_DATA_SIZE);

          {
            GString *hex = g_string_new ("TLS ServerFinished hs_hash: ");
            for (gsize i = 0; i < 32; i++)
              g_string_append_printf (hex, "%02x", hs_hash[i]);
            g_string_append_printf (hex, " expected_vd: ");
            for (gsize i = 0; i < TLS_VERIFY_DATA_SIZE; i++)
              g_string_append_printf (hex, "%02x", expected_vd[i]);
            g_string_append_printf (hex, " received_vd: ");
            for (gsize i = 0; i < TLS_VERIFY_DATA_SIZE; i++)
              g_string_append_printf (hex, "%02x", received_vd[i]);
            fp_dbg ("%s", hex->str);
            g_string_free (hex, TRUE);
          }

          if (memcmp (received_vd, expected_vd, TLS_VERIFY_DATA_SIZE) != 0)
            {
              g_free (decrypted);
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS ServerFinished: verify_data mismatch");
              return FALSE;
            }

          /* Update handshake hash with server's Finished */
          g_checksum_update (tls->handshake_hash, decrypted, plain_len);

          g_free (decrypted);
          got_finished = TRUE;
        }
    }

  if (!got_ccs || !got_finished)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "TLS ServerFinish: missing CCS or Finished (ccs=%d fin=%d)",
                   got_ccs, got_finished);
      return FALSE;
    }

  fp_dbg ("TLS handshake completed successfully");
  return TRUE;
}

/* ================================================================
 * TLS Flash Read SSM (reads flash partition 1 over USB)
 * ================================================================ */

static void
tls_flash_read_cmd (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  /* READ_FLASH(partition=1, offset=0, size=0x1000)
   * Format from python-validity: pack('<BBBHLL', 0x40, partition, 1, 0, addr, size) */
  guint8 cmd[13];

  validity_pack (cmd, sizeof (cmd), "bbbhww",
                 VCSFW_CMD_READ_FLASH,
                 0x01,           /* partition */
                 0x01,           /* access flag */
                 (guint16) 0,    /* reserved */
                 (guint32) 0,    /* offset */
                 (guint32) 0x1000);  /* size */
  vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
}

void
validity_tls_flash_read_run_state (FpiSsm   *ssm,
                                   FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case TLS_FLASH_READ_CMD:
      tls_flash_read_cmd (ssm, self);
      break;

    case TLS_FLASH_READ_RECV:
      /* vcsfw_cmd_send sub-SSM stored response in self->cmd_response_data.
       * We do NOT parse here — PSK derivation must happen first.
       * Parsing is done in OPEN_TLS_DERIVE_PSK after the PSK keys are
       * available for private key decryption. */
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          /* Do not hand an error response to the flash parser */
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "TLS flash read: status 0x%04x",
                                                         self->cmd_response_status));
          return;
        }

      if (!self->cmd_response_data || self->cmd_response_len == 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "TLS flash read: empty response"));
          return;
        }

      fp_dbg ("TLS flash read: got %zu bytes", self->cmd_response_len);
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

/* ================================================================
 * TLS Handshake SSM
 * ================================================================
 *
 * TLS handshake messages use 0x44000000 prefix and return raw TLS
 * records (NOT VCSFW status + payload). We use direct USB transfers
 * rather than vcsfw_cmd_send to avoid the 2-byte status stripping.
 */

/* Receive callback for TLS: stores FULL response without any stripping */
static void
tls_raw_recv_cb (FpiUsbTransfer *transfer,
                 FpDevice       *device,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Store complete response — no status byte stripping */
  g_clear_pointer (&self->cmd_response_data, g_free);
  self->cmd_response_len = transfer->actual_length;
  if (transfer->actual_length > 0)
    self->cmd_response_data = g_memdup2 (transfer->buffer,
                                         transfer->actual_length);
  else
    self->cmd_response_data = NULL;

  fp_dbg ("TLS recv: %zu bytes", self->cmd_response_len);

  /* Hex dump first bytes for debugging */
  if (self->cmd_response_data && self->cmd_response_len > 0)
    {
      gsize dump_len = self->cmd_response_len;
      g_autofree gchar *hex = g_malloc (dump_len * 3 + 1);
      for (gsize i = 0; i < dump_len; i++)
        g_snprintf (hex + i * 3, 4, "%02x ",
                    self->cmd_response_data[i]);
      hex[dump_len * 3] = '\0';
      fp_dbg ("TLS recv hex: %s", hex);
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
tls_hs_send_client_hello (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  gsize cmd_len;
  guint8 *cmd = validity_tls_build_client_hello (&self->tls, &cmd_len);

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT, cmd_len);
  memcpy (transfer->buffer, cmd, cmd_len);
  g_free (cmd);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
tls_hs_recv_server_hello (FpiSsm            *ssm,
                          FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  /* Receive raw TLS records (ServerHello + CertReq + ServerHelloDone) */
  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           tls_raw_recv_cb, NULL);
}

static void
tls_hs_send_client_finish (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  /* Parse the stored ServerHello response (synchronous) */
  GError *error = NULL;

  if (!self->cmd_response_data)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "TLS handshake: no ServerHello response"));
      return;
    }

  if (!validity_tls_parse_server_hello (&self->tls,
                                        self->cmd_response_data,
                                        self->cmd_response_len,
                                        &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* Build and send Certificate + KeyExchange + CertVerify + CCS + Finished */
  gsize cmd_len;
  guint8 *cmd = validity_tls_build_client_finish (&self->tls, &cmd_len);

  transfer = fpi_usb_transfer_new (dev);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT, cmd_len);
  memcpy (transfer->buffer, cmd, cmd_len);
  g_free (cmd);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
tls_hs_recv_server_finish (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *transfer;

  /* Receive raw TLS records (CCS + encrypted Finished) */
  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                              VALIDITY_MAX_TRANSFER_LEN);
  fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                           tls_raw_recv_cb, NULL);
}

static void
tls_hs_parse_server_finish (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  GError *error = NULL;

  if (!self->cmd_response_data)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "TLS handshake: no ServerFinish response"));
      return;
    }

  if (!validity_tls_parse_server_finish (&self->tls,
                                         self->cmd_response_data,
                                         self->cmd_response_len,
                                         &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_info ("TLS session established (secure_rx=%d secure_tx=%d)",
           self->tls.secure_rx, self->tls.secure_tx);
  fpi_ssm_mark_completed (ssm);
}

void
validity_tls_handshake_run_state (FpiSsm   *ssm,
                                  FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case TLS_HS_SEND_CLIENT_HELLO:
      tls_hs_send_client_hello (ssm, self);
      break;

    case TLS_HS_RECV_SERVER_HELLO:
      tls_hs_recv_server_hello (ssm, self);
      break;

    case TLS_HS_SEND_CLIENT_FINISH:
      tls_hs_send_client_finish (ssm, self);
      break;

    case TLS_HS_RECV_SERVER_FINISH:
      tls_hs_recv_server_finish (ssm, self);
      break;

    case TLS_HS_PARSE_SERVER_FINISH:
      tls_hs_parse_server_finish (ssm, self);
      break;

    }
}
