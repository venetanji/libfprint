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

#pragma once

#include <glib.h>
#include <openssl/evp.h>
#include <openssl/ec.h>

/* TLS record content types */
#define TLS_CONTENT_CHANGE_CIPHER 0x14
#define TLS_CONTENT_HANDSHAKE 0x16
#define TLS_CONTENT_APP_DATA 0x17

/* TLS version 1.2 */
#define TLS_VERSION_MAJOR 0x03
#define TLS_VERSION_MINOR 0x03

/* TLS handshake message types */
#define TLS_HS_CLIENT_HELLO 0x01
#define TLS_HS_SERVER_HELLO 0x02
#define TLS_HS_CERTIFICATE 0x0B
#define TLS_HS_CERT_REQUEST 0x0D
#define TLS_HS_SERVER_HELLO_DONE 0x0E
#define TLS_HS_CERT_VERIFY 0x0F
#define TLS_HS_CLIENT_KEY_EXCHANGE 0x10
#define TLS_HS_FINISHED 0x14

/* Cipher suite */
#define TLS_CS_ECDH_ECDSA_AES256_CBC_SHA 0xC005

/* Key/block sizes */
#define TLS_AES_KEY_SIZE 32
#define TLS_IV_SIZE 16
#define TLS_HMAC_SIZE 32
#define TLS_AES_BLOCK_SIZE 16
#define TLS_MASTER_SECRET_SIZE 48
#define TLS_KEY_BLOCK_SIZE 0x120
#define TLS_RANDOM_SIZE 32
#define TLS_VERIFY_DATA_SIZE 12

/* VCSFW TLS command prefix */
#define TLS_CMD_PREFIX_SIZE 4

/* Flash block IDs */
#define TLS_FLASH_BLOCK_EMPTY0 0x0000
#define TLS_FLASH_BLOCK_EMPTY1 0x0001
#define TLS_FLASH_BLOCK_EMPTY2 0x0002
#define TLS_FLASH_BLOCK_CERT 0x0003
#define TLS_FLASH_BLOCK_PRIVKEY 0x0004
#define TLS_FLASH_BLOCK_CA_CERT 0x0005
#define TLS_FLASH_BLOCK_ECDH 0x0006
#define TLS_FLASH_BLOCK_END 0xFFFF

/* Flash block header: [id:2 LE][size:2 LE][sha256:32] */
#define TLS_FLASH_BLOCK_HEADER_SIZE (2 + 2 + 32)

/* ECDH key blob offsets */
#define TLS_ECDH_BLOB_SIZE 0x90
#define TLS_ECDH_X_OFFSET 0x08
#define TLS_ECDH_Y_OFFSET 0x4C
#define TLS_EC_COORD_SIZE 0x20

/* Forward declaration */
typedef struct _FpiDeviceValidity FpiDeviceValidity;

/* TLS session state */
typedef struct
{
  /* Session keys (derived during handshake) */
  guint8 sign_key[TLS_AES_KEY_SIZE];
  guint8 validation_key[TLS_AES_KEY_SIZE];
  guint8 encryption_key[TLS_AES_KEY_SIZE];
  guint8 decryption_key[TLS_AES_KEY_SIZE];

  /* Pre-shared keys (derived from hardware identity) */
  guint8 psk_encryption_key[TLS_AES_KEY_SIZE];
  guint8 psk_validation_key[TLS_AES_KEY_SIZE];

  /* Handshake state */
  GChecksum *handshake_hash;     /* running SHA-256 of handshake messages */
  guint8     client_random[TLS_RANDOM_SIZE];
  guint8     server_random[TLS_RANDOM_SIZE];
  guint8     master_secret[TLS_MASTER_SECRET_SIZE];

  /* ECDH session ephemeral key pair (generated per handshake) */
  EVP_PKEY *session_key;

  /* TLS client certificate from flash (block ID 3) */
  guint8 *tls_cert;
  gsize   tls_cert_len;

  /* Client private key from flash (block ID 4, decrypted with PSK) */
  EVP_PKEY *priv_key;

  /* ECDH server public key from flash (block ID 6) */
  EVP_PKEY *ecdh_q;

  /* Raw flash blobs (needed for pairing later) */
  guint8 *priv_blob;
  gsize   priv_blob_len;
  guint8 *ecdh_blob;
  gsize   ecdh_blob_len;

  /* TLS channel state */
  gboolean secure_rx;
  gboolean secure_tx;
  gboolean keys_loaded;
  gboolean session_broken;   /* chip sent a TLS alert -- session torn down */
  gboolean priv_key_mismatch; /* last flash parse: block-4 HMAC failed under
                               * this host's PSK (paired to another identity) */

  /* Runtime data keys (populated from data store during open) */
  const guint8 *password;       /* TLS password (32 bytes) */
  gsize         password_len;
  const guint8 *gwk_sign;       /* GWK signing key (32 bytes) */
  gsize         gwk_sign_len;
  const guint8 *fw_pubkey_x;    /* firmware ECDSA public key X (32 bytes) */
  gsize         fw_pubkey_x_len;
  const guint8 *fw_pubkey_y;    /* firmware ECDSA public key Y (32 bytes) */
  gsize         fw_pubkey_y_len;
} ValidityTlsState;

/* TLS handshake SSM states */
typedef enum {
  TLS_HS_SEND_CLIENT_HELLO = 0,
  TLS_HS_RECV_SERVER_HELLO,
  TLS_HS_SEND_CLIENT_FINISH,
  TLS_HS_RECV_SERVER_FINISH,
  TLS_HS_PARSE_SERVER_FINISH,
  TLS_HS_NUM_STATES,
} ValidityTlsHandshakeState;

/* TLS flash read SSM states */
typedef enum {
  TLS_FLASH_READ_CMD = 0,
  TLS_FLASH_READ_RECV,
  TLS_FLASH_READ_NUM_STATES,
} ValidityTlsFlashReadState;

/* ---- Public API ---- */

void     validity_tls_init (ValidityTlsState *tls);
void     validity_tls_free (ValidityTlsState *tls);

void     validity_tls_derive_psk (ValidityTlsState *tls);

/* The steps of validity_tls_derive_psk() -- exported for testing */
void     validity_tls_derive_psk_from_files (ValidityTlsState *tls,
                                             const gchar      *name_path,
                                             const gchar      *serial_path);
void     validity_tls_derive_psk_from_identity (ValidityTlsState *tls,
                                                const gchar      *product_name,
                                                const gchar      *product_serial);

gboolean validity_tls_parse_flash (ValidityTlsState *tls,
                                   const guint8     *data,
                                   gsize             data_len,
                                   GError          **error);

/* PRF — exported for testing */
void     validity_tls_prf (const guint8 *secret,
                           gsize         secret_len,
                           const guint8 *seed,
                           gsize         seed_len,
                           guint8       *output,
                           gsize         output_len);

/* Encrypt/decrypt for TLS app data */
guint8  *validity_tls_encrypt (ValidityTlsState *tls,
                               const guint8     *plaintext,
                               gsize             plaintext_len,
                               gsize            *out_len);

guint8  *validity_tls_decrypt (ValidityTlsState *tls,
                               const guint8     *ciphertext,
                               gsize             ciphertext_len,
                               gsize            *out_len,
                               GError          **error);

/* Build TLS app_data record wrapping a VCSFW command */
guint8  *validity_tls_wrap_app_data (ValidityTlsState *tls,
                                     const guint8     *cmd,
                                     gsize             cmd_len,
                                     gsize            *out_len);

/* Parse TLS response, returning decrypted app_data */
guint8  *validity_tls_unwrap_response (ValidityTlsState *tls,
                                       const guint8     *response,
                                       gsize             response_len,
                                       gsize            *out_len,
                                       GError          **error);

/* Build the full handshake USB commands.
 * Returns the first USB command (ClientHello with 0x44000000 prefix). */
guint8  *validity_tls_build_client_hello (ValidityTlsState *tls,
                                          gsize            *out_len);

/* Parse ServerHello + CertReq + ServerHelloDone from USB response */
gboolean validity_tls_parse_server_hello (ValidityTlsState *tls,
                                          const guint8     *data,
                                          gsize             data_len,
                                          GError          **error);

/* Build Certificate + ClientKeyExchange + CertVerify + ChangeCipherSpec + Finished */
guint8  *validity_tls_build_client_finish (ValidityTlsState *tls,
                                           gsize            *out_len);

/* Parse server ChangeCipherSpec + Finished */
gboolean validity_tls_parse_server_finish (ValidityTlsState *tls,
                                           const guint8     *data,
                                           gsize             data_len,
                                           GError          **error);

/* SSM runner for TLS handshake */
void     validity_tls_handshake_run_state (FpiSsm   *ssm,
                                           FpDevice *dev);

/* SSM runner for reading flash TLS data */
void     validity_tls_flash_read_run_state (FpiSsm   *ssm,
                                            FpDevice *dev);
