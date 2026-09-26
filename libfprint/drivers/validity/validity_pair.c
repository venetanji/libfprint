/*
 * Device pairing for Validity/Synaptics VCSFW fingerprint sensors
 *
 * Handles pairing of uninitialized devices: flash partitioning,
 * ECDH key exchange, certificate creation, and TLS flash persistence.
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
#include "fpi-byte-utils.h"
#include "validity.h"
#include "validity_pack.h"
#include "validity_data.h"
#include "validity_pair.h"
#include "validity_tls.h"
#include "vcsfw_protocol.h"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <string.h>

/* CA cert, partition signatures, and TLS keys are now loaded at runtime
 * from external .bin files via validity_data.c. See the
 * libfprint-validity-data package. */

/* ================================================================
 * Pairing state management
 * ================================================================ */

void
validity_pair_state_init (ValidityPairState *state)
{
  memset (state, 0, sizeof (*state));
}

void
validity_pair_state_free (ValidityPairState *state)
{
  g_clear_pointer (&state->client_key, EVP_PKEY_free);
  g_clear_pointer (&state->server_cert, g_free);
  g_clear_pointer (&state->ecdh_blob, g_free);
  g_clear_pointer (&state->priv_blob, g_free);
}

/* ================================================================
 * Flash info parsing (CMD 0x3e response)
 *
 * Response format (after 2-byte status):
 *   [jid0:2LE][jid1:2LE][blocks:2LE][unknown0:2LE]
 *   [blocksize:2LE][unknown1:2LE][partition_count:2LE]
 *   [partition_entries: count * 12 bytes each]
 * ================================================================ */

#define FLASH_INFO_HEADER_SIZE 14   /* 7 × guint16 */

gboolean
validity_pair_parse_flash_info (const guint8          *data,
                                gsize                  data_len,
                                ValidityFlashIcParams *ic_out,
                                guint16               *num_partitions_out)
{
  if (!data || data_len < FLASH_INFO_HEADER_SIZE)
    return FALSE;

  guint16 jid0 = 0, jid1 = 0, blocks = 0, blocksize = 0, pcnt = 0;
  validity_unpack (data, data_len, "hhhxxhxxh",
                   &jid0, &jid1, &blocks, &blocksize, &pcnt);

  (void) jid0;
  (void) jid1;

  /* Flash IC params for CMD 0x4f: we need size, sector_size, erase_cmd.
   * The actual IC lookup is done by the device firmware — we just pass
   * the size info through in serialize_flash_params format. */
  ic_out->size = (guint32) blocks * (guint32) blocksize;

  /* Default sector size and erase command for the common flash ICs
   * used in these devices (W25Q80B, MX25V8035F).
   * python-validity looks these up from flash_ic_table, but we only
   * need (size, sector_size, sector_erase_cmd) for serialize_flash_params. */
  ic_out->sector_size = 0x1000;
  ic_out->sector_erase_cmd = 0x20;

  *num_partitions_out = pcnt;

  fp_dbg ("Flash info: size=%u (blocks=%u × bs=%u), partitions=%u",
          ic_out->size, blocks, blocksize, pcnt);

  return TRUE;
}

/* ================================================================
 * Pairing decision
 *
 * Pairing sends reset_blob and partition_flash and erases flash partitions,
 * so the check fails closed, with the same rule as python-validity's
 * init_flash: only a sensor that reports no partitions is paired. On a
 * partitioned sensor, keys present needs nothing, and a blank, partial or
 * unreadable partition 1 fails the open instead.
 * ================================================================ */

/* READ_FLASH response header before the flash data: [size:4LE][unknown:2] */
#define TLS_FLASH_RESPONSE_HEADER_SIZE 6

ValidityTlsFlashClass
validity_pair_classify_tls_flash (guint16       status,
                                  const guint8 *data,
                                  gsize         len)
{
  FpiByteReader r;
  guint32 flash_sz = 0;
  gsize flash_avail;
  gboolean found_priv = FALSE, found_ecdh = FALSE, found_cert = FALSE;
  gboolean incomplete = FALSE, saw_end = FALSE;
  guint n_blocks = 0;

  if (status != VCSFW_STATUS_OK || data == NULL ||
      len < TLS_FLASH_RESPONSE_HEADER_SIZE)
    return VALIDITY_TLS_FLASH_UNREADABLE;

  flash_sz = FP_READ_UINT32_LE (data);
  flash_avail = len - TLS_FLASH_RESPONSE_HEADER_SIZE;
  if (flash_sz > flash_avail)
    {
      /* Short read: the sensor sent less than it declared */
      incomplete = TRUE;
      flash_sz = flash_avail;
    }

  fpi_byte_reader_init (&r, data + TLS_FLASH_RESPONSE_HEADER_SIZE, flash_sz);

  while (fpi_byte_reader_get_remaining (&r) >= TLS_FLASH_BLOCK_HEADER_SIZE)
    {
      guint16 block_id = 0, block_size = 0;

      fpi_byte_reader_get_uint16_le (&r, &block_id);
      fpi_byte_reader_get_uint16_le (&r, &block_size);

      if (block_id == TLS_FLASH_BLOCK_END)
        {
          saw_end = TRUE;
          break;
        }

      fpi_byte_reader_skip (&r, 32);  /* hash */

      if (block_size > fpi_byte_reader_get_remaining (&r))
        {
          incomplete = TRUE;
          break;
        }

      n_blocks++;
      if (block_id == TLS_FLASH_BLOCK_PRIVKEY)
        found_priv = TRUE;
      if (block_id == TLS_FLASH_BLOCK_ECDH)
        found_ecdh = TRUE;
      if (block_id == TLS_FLASH_BLOCK_CERT)
        found_cert = TRUE;

      fpi_byte_reader_skip (&r, block_size);
    }

  /* Same test as before this was split out: the TLS parse checks the
   * blocks' hashes and the private key's HMAC afterwards. */
  if (found_priv && found_ecdh && found_cert)
    return VALIDITY_TLS_FLASH_KEYS_PRESENT;

  /* We cannot tell what the unread part holds, so do not call it blank or
   * partial. */
  if (incomplete)
    return VALIDITY_TLS_FLASH_UNREADABLE;

  if (n_blocks == 0)
    /* Blank only if the end marker was actually seen; a read too short to
     * hold one block header proves nothing. */
    return saw_end ? VALIDITY_TLS_FLASH_BLANK : VALIDITY_TLS_FLASH_UNREADABLE;

  return VALIDITY_TLS_FLASH_PARTIAL;
}

ValidityPairDecision
validity_pair_decide (guint16               num_partitions,
                      ValidityTlsFlashClass flash,
                      gboolean              check_only)
{
  /* No partition table: by the firmware's own report there is no key store
   * and no template partition, so there is nothing on the sensor to lose.
   * This is the only case python-validity's init_flash provisions; it
   * returns without pairing whenever any partition exists. */
  if (num_partitions == 0)
    return check_only ? VALIDITY_PAIR_REFUSE_RECOVERY : VALIDITY_PAIR_PROVISION;

  /* Partitioned: never paired, whatever partition 1 holds */
  switch (flash)
    {
    case VALIDITY_TLS_FLASH_KEYS_PRESENT:
      return VALIDITY_PAIR_NOT_NEEDED;

    case VALIDITY_TLS_FLASH_BLANK:
      /* Partitioned, but partition 1 was never written, for example after
       * an interrupted pairing (not confirmed on hardware). Pairing again
       * would mean sending reset_blob and partition_flash to a partitioned
       * sensor, which python-validity never does. */
      return VALIDITY_PAIR_REFUSE_BLANK;

    case VALIDITY_TLS_FLASH_PARTIAL:
      return VALIDITY_PAIR_REFUSE_PARTIAL;

    case VALIDITY_TLS_FLASH_UNREADABLE:
    default:
      return VALIDITY_PAIR_REFUSE_UNREADABLE;
    }
}

/* ================================================================
 * Partition serialization
 *
 * Each entry: [id:1][type:1][access:2LE][offset:4LE][size:4LE]
 *   + 4 zero bytes
 *   + SHA-256(12-byte entry) = 32 bytes
 *   Total: 48 bytes per partition
 * ================================================================ */

void
validity_pair_serialize_partition (const ValidityPartition *part,
                                   guint8                  *out)
{
  guint8 entry[12];

  validity_pack (entry, sizeof (entry), "bbhww",
                 part->id, part->type, part->access_lvl,
                 part->offset, part->size);

  /* Copy 12-byte entry to output */
  memcpy (out, entry, 12);

  /* 4 zero bytes */
  memset (out + 12, 0, 4);

  /* SHA-256 of the 12-byte entry */
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, entry, 12);
  gsize hash_len = 32;
  g_checksum_get_digest (checksum, out + 16, &hash_len);
}

/* ================================================================
 * Header builder: [id:2LE][len:2LE][body]
 * Matches python-validity with_hdr()
 * ================================================================ */

static guint8 *
build_header (guint16 id, const guint8 *body, gsize body_len, gsize *out_len)
{
  return validity_pack_new (out_len, "hhd",
                            id, (guint16) body_len,
                            body, body_len);
}

/* ================================================================
 * Flash IC params serialization
 *
 * Format: [size:4LE][sector_size:4LE][pad:2][erase_cmd:1][pad:1]
 * Matches python-validity serialize_flash_params()
 * ================================================================ */

static void
serialize_flash_params (const ValidityFlashIcParams *ic, guint8 *out)
{
  validity_pack (out, 12, "wwxxbx",
                 ic->size, ic->sector_size,
                 ic->sector_erase_cmd);
}

/* ================================================================
 * HS key derivation
 *
 * The handshake signing key is derived from the hardcoded password
 * using the same PRF as TLS:
 *   key = password[0:16]
 *   seed = password[16:32] + 0xaa 0xaa
 *   hs_key = PRF(key, "HS_KEY_PAIR_GEN" + seed, 32)
 *
 * This key is used to ECDSA-sign the client certificate.
 * ================================================================ */

static EVP_PKEY *
derive_hs_signing_key (const guint8 *password, gsize password_len)
{
  if (!password || password_len < 32)
    return NULL;

  const guint8 *key = password;
  guint8 prf_seed[15 + 16 + 2]; /* "HS_KEY_PAIR_GEN" + password[16:32] + 0xaa*2 */
  guint8 hs_key_bytes[32];

  /* Build PRF seed: label + password_tail + 0xaa padding */
  memcpy (prf_seed, "HS_KEY_PAIR_GEN", 15);
  memcpy (prf_seed + 15, password + 16, 16);
  prf_seed[31] = 0xaa;
  prf_seed[32] = 0xaa;

  /* PRF(key=password[0:16], seed=prf_seed, output_len=32) */
  validity_tls_prf (key, 16, prf_seed, 33, hs_key_bytes, 32);

  /* Convert to big-endian scalar (python-validity does [::-1] reversal) */
  guint8 be_scalar[32];
  for (int i = 0; i < 32; i++)
    be_scalar[i] = hs_key_bytes[31 - i];

  /* Build EC private key from scalar */
  BIGNUM *priv_bn = BN_bin2bn (be_scalar, 32, NULL);
  if (!priv_bn)
    return NULL;

  /* Derive public key from private scalar on P-256 */
  EC_GROUP *group = EC_GROUP_new_by_curve_name (NID_X9_62_prime256v1);
  EC_POINT *pub_point = EC_POINT_new (group);
  EC_POINT_mul (group, pub_point, priv_bn, NULL, NULL, NULL);

  /* Extract public key coordinates */
  BIGNUM *pub_x = BN_new ();
  BIGNUM *pub_y = BN_new ();
  EC_POINT_get_affine_coordinates (group, pub_point, pub_x, pub_y, NULL);

  guint8 pub_x_bytes[32], pub_y_bytes[32];
  BN_bn2binpad (pub_x, pub_x_bytes, 32);
  BN_bn2binpad (pub_y, pub_y_bytes, 32);

  /* Build EVP_PKEY via OSSL_PARAM_BLD */
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new ();
  OSSL_PARAM_BLD_push_utf8_string (bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);

  guint8 pub_uncompressed[65];
  pub_uncompressed[0] = 0x04;
  memcpy (pub_uncompressed + 1, pub_x_bytes, 32);
  memcpy (pub_uncompressed + 33, pub_y_bytes, 32);
  OSSL_PARAM_BLD_push_octet_string (bld, OSSL_PKEY_PARAM_PUB_KEY, pub_uncompressed, 65);
  OSSL_PARAM_BLD_push_BN (bld, OSSL_PKEY_PARAM_PRIV_KEY, priv_bn);

  OSSL_PARAM *params = OSSL_PARAM_BLD_to_param (bld);
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  EVP_PKEY *pkey = NULL;
  EVP_PKEY_fromdata_init (ctx);
  EVP_PKEY_fromdata (ctx, &pkey, EVP_PKEY_KEYPAIR, params);

  EVP_PKEY_CTX_free (ctx);
  OSSL_PARAM_free (params);
  OSSL_PARAM_BLD_free (bld);
  BN_free (pub_x);
  BN_free (pub_y);
  EC_POINT_free (pub_point);
  EC_GROUP_free (group);
  BN_free (priv_bn);

  return pkey;
}

/* ================================================================
 * Client certificate builder
 *
 * Format (444 bytes total):
 *   [0x17:4LE][0x20:4LE]
 *   [public_x:32 LE][zeros:36]
 *   [public_y:32 LE][zeros:76]
 *   [sig_len:4LE][DER signature]
 *   [zero-pad to 444 bytes]
 *
 * The certificate body (before signature) is signed with the HS key.
 * ================================================================ */

/* Certificate body size: 8 + 32 + 36 + 32 + 76 = 184 bytes */
#define CERT_BODY_SIZE 184

guint8 *
validity_pair_make_cert (const guint8 *client_public_x,
                         const guint8 *client_public_y,
                         const guint8 *password,
                         gsize         password_len,
                         gsize        *out_len)
{
  guint8 body[CERT_BODY_SIZE];

  memset (body, 0, sizeof (body));
  FpiByteWriter body_writer;
  fpi_byte_writer_init_with_data (&body_writer, body, sizeof (body), TRUE);
  fpi_byte_writer_put_uint32_le (&body_writer, 0x17);
  fpi_byte_writer_put_uint32_le (&body_writer, 0x20);
  fpi_byte_writer_put_data (&body_writer, client_public_x, 32);
  /* 36 zero bytes at offset 40..75 */
  fpi_byte_writer_fill (&body_writer, 0, 36);
  fpi_byte_writer_put_data (&body_writer, client_public_y, 32);
  /* 76 zero bytes at offset 108..183 */

  /* Sign body with HS key (ECDSA + SHA-256) */
  EVP_PKEY *hs_key = derive_hs_signing_key (password, password_len);
  if (!hs_key)
    {
      fp_warn ("Failed to derive HS signing key");
      return NULL;
    }

  guint8 sig_buf[128];
  size_t sig_len = sizeof (sig_buf);

  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new ();
  EVP_DigestSignInit (md_ctx, NULL, EVP_sha256 (), NULL, hs_key);
  EVP_DigestSignUpdate (md_ctx, body, sizeof (body));
  if (EVP_DigestSignFinal (md_ctx, sig_buf, &sig_len) != 1)
    {
      fp_warn ("ECDSA signing failed");
      EVP_MD_CTX_free (md_ctx);
      EVP_PKEY_free (hs_key);
      return NULL;
    }
  EVP_MD_CTX_free (md_ctx);
  EVP_PKEY_free (hs_key);

  /* Build output: body + sig_len(4LE) + sig + zero-pad to 444 */
  guint8 *cert = g_malloc0 (VALIDITY_CLIENT_CERT_SIZE);
  FpiByteWriter cert_writer;
  fpi_byte_writer_init_with_data (&cert_writer, cert, VALIDITY_CLIENT_CERT_SIZE, FALSE);
  fpi_byte_writer_put_data (&cert_writer, body, sizeof (body));
  fpi_byte_writer_put_uint32_le (&cert_writer, (guint32) sig_len);

  if (fpi_byte_writer_get_pos (&cert_writer) + sig_len <= VALIDITY_CLIENT_CERT_SIZE)
    fpi_byte_writer_put_data (&cert_writer, sig_buf, sig_len);

  *out_len = VALIDITY_CLIENT_CERT_SIZE;
  return cert;
}

/* ================================================================
 * Private key encryption
 *
 * Encrypts the ECDH client private key for flash storage:
 *   plaintext = x(32LE) + y(32LE) + d(32LE) + PKCS7 padding to 16-byte block
 *   iv = random 16 bytes
 *   ciphertext = AES-256-CBC(psk_encryption_key, iv, padded_plaintext)
 *   blob = 0x02 + iv + ciphertext + HMAC-SHA256(psk_validation_key, iv+ciphertext)
 * ================================================================ */

guint8 *
validity_pair_encrypt_key (const guint8 *client_private,
                           const guint8 *client_public_x,
                           const guint8 *client_public_y,
                           const guint8 *psk_encryption_key,
                           const guint8 *psk_validation_key,
                           gsize        *out_len)
{
  /* Build plaintext: x + y + d = 96 bytes */
  guint8 plaintext[96 + 16]; /* + max PKCS7 padding */

  memcpy (plaintext, client_public_x, 32);
  memcpy (plaintext + 32, client_public_y, 32);
  memcpy (plaintext + 64, client_private, 32);

  /* PKCS7 pad to 16-byte boundary: 96 bytes → pad_len = 16 */
  guint8 pad_len = 16 - (96 % 16);
  if (pad_len == 0)
    pad_len = 16;
  memset (plaintext + 96, pad_len, pad_len);
  gsize padded_len = 96 + pad_len;

  /* Generate random IV */
  guint8 iv[VALIDITY_ENCRYPTED_KEY_IV_SIZE];
  if (RAND_bytes (iv, sizeof (iv)) != 1)
    {
      fp_warn ("Failed to generate random IV");
      return NULL;
    }

  /* AES-256-CBC encrypt */
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  EVP_CIPHER_CTX_set_padding (ctx, 0); /* We handle padding manually */

  if (EVP_EncryptInit_ex (ctx, EVP_aes_256_cbc (), NULL,
                          psk_encryption_key, iv) != 1)
    {
      EVP_CIPHER_CTX_free (ctx);
      return NULL;
    }

  guint8 ciphertext[112 + 16]; /* padded_len + possible block */
  int ct_len = 0, final_len = 0;

  EVP_EncryptUpdate (ctx, ciphertext, &ct_len, plaintext, (int) padded_len);
  EVP_EncryptFinal_ex (ctx, ciphertext + ct_len, &final_len);
  ct_len += final_len;
  EVP_CIPHER_CTX_free (ctx);

  /* Build blob: 0x02 + iv(16) + ciphertext + HMAC-SHA256(iv + ciphertext) */
  gsize iv_ct_len = sizeof (iv) + ct_len;
  gsize blob_len = 1 + iv_ct_len + 32;  /* prefix + iv+ct + hmac */
  guint8 *blob = g_malloc (blob_len);

  validity_pack (blob, blob_len, "bdd",
                 VALIDITY_ENCRYPTED_KEY_PREFIX,
                 iv, (gsize) sizeof (iv),
                 ciphertext, (gsize) ct_len);

  /* HMAC-SHA256 over iv + ciphertext */
  unsigned int hmac_len = 32;
  HMAC (EVP_sha256 (),
        psk_validation_key, 32,
        blob + 1, iv_ct_len,
        blob + 1 + iv_ct_len, &hmac_len);

  *out_len = blob_len;

  /* Clear sensitive plaintext from stack */
  OPENSSL_cleanse (plaintext, sizeof (plaintext));

  return blob;
}

/* ================================================================
 * CMD 0x4f (PARTITION_FLASH) command builder
 *
 * Payload format:
 *   [0x4f][0x00 0x00][0x00 0x00]              — 5-byte command prefix
 *   [hdr 0: flash IC params (12 bytes)]        — serialize_flash_params
 *   [hdr 1: partition table + RSA signature]   — serialized partitions + sig
 *   [hdr 5: client certificate (444 bytes)]    — make_cert output
 *   [hdr 3: CA certificate]                    — hardcoded
 *
 * Each header: [id:2LE][body_len:2LE][body]
 * ================================================================ */

guint8 *
validity_pair_build_partition_flash_cmd (const ValidityFlashIcParams *flash_ic,
                                         const ValidityFlashLayout   *layout,
                                         const guint8                *partition_sig,
                                         gsize                        partition_sig_len,
                                         const guint8                *client_public_x,
                                         const guint8                *client_public_y,
                                         const guint8                *password,
                                         gsize                        password_len,
                                         const guint8                *ca_cert,
                                         gsize                        ca_cert_len,
                                         gsize                       *out_len)
{
  /* Build flash IC params body (hdr 0) */
  guint8 ic_body[12];

  serialize_flash_params (flash_ic, ic_body);

  gsize hdr0_len;
  g_autofree guint8 *hdr0 = build_header (VALIDITY_HDR_FLASH_IC,
                                          ic_body, sizeof (ic_body),
                                          &hdr0_len);

  /* Build partition table body (hdr 1):
   *   [partition entries (48 bytes each)] + [RSA signature (256 bytes)] */
  gsize ptbl_body_len = (layout->num_partitions * VALIDITY_PARTITION_ENTRY_SIZE) +
                        partition_sig_len;
  g_autofree guint8 *ptbl_body = g_malloc0 (ptbl_body_len);

  for (gsize i = 0; i < layout->num_partitions; i++)
    validity_pair_serialize_partition (&layout->partitions[i],
                                       ptbl_body + (i * VALIDITY_PARTITION_ENTRY_SIZE));
  memcpy (ptbl_body + (layout->num_partitions * VALIDITY_PARTITION_ENTRY_SIZE),
          partition_sig, partition_sig_len);

  gsize hdr1_len;
  g_autofree guint8 *hdr1 = build_header (VALIDITY_HDR_PARTITION_TABLE,
                                          ptbl_body, ptbl_body_len,
                                          &hdr1_len);

  /* Build client certificate (hdr 5) */
  gsize cert_len;
  g_autofree guint8 *cert = validity_pair_make_cert (client_public_x,
                                                     client_public_y,
                                                     password, password_len,
                                                     &cert_len);
  if (!cert)
    return NULL;

  gsize hdr5_len;
  g_autofree guint8 *hdr5 = build_header (VALIDITY_HDR_CLIENT_CERT,
                                          cert, cert_len,
                                          &hdr5_len);

  /* CA certificate (hdr 3) — from runtime data files */
  gsize hdr3_len;
  g_autofree guint8 *hdr3 = build_header (VALIDITY_HDR_CA_CERT,
                                          ca_cert, ca_cert_len,
                                          &hdr3_len);

  /* Assemble: [4f 00 00 00 00] + hdr0 + hdr1 + hdr5 + hdr3 */
  gsize cmd_prefix_len = 5;
  gsize total = cmd_prefix_len + hdr0_len + hdr1_len + hdr5_len + hdr3_len;
  guint8 *cmd = g_malloc0 (total);
  validity_pack (cmd, total, "bxxxxdddd",
                 (guint8) 0x4f,
                 hdr0, hdr0_len, hdr1, hdr1_len,
                 hdr5, hdr5_len, hdr3, hdr3_len);

  *out_len = total;
  return cmd;
}

/* ================================================================
 * TLS flash image builder
 *
 * Builds the 4096-byte flash image that contains all TLS data.
 * Format: sequence of blocks [id:2LE][size:2LE][SHA256:32][body]
 * padded with 0xff to 4096 bytes.
 *
 * Block order (from python-validity make_tls_flash):
 *   block 0: single zero byte (empty marker)
 *   block 4: encrypted private key (priv_blob)
 *   block 3: server certificate (from partition_flash response)
 *   block 5: CA certificate (hardcoded)
 *   block 1: 256 zero bytes (empty placeholder)
 *   block 2: 256 zero bytes (empty placeholder)
 *   block 6: ECDH blob (from CMD 0x50 response)
 *   Remaining: 0xff padding to 0x1000
 * ================================================================ */

#define TLS_FLASH_IMAGE_SIZE 0x1000

/* Append one flash block (header + SHA-256 + body) at *offset, advancing it.
 * Returns FALSE (writing nothing) if the block would not fit in buf_size or if
 * body_len exceeds the 16-bit size field. body_len is device-controlled for
 * some blocks (server cert, ECDH blob), so this bound is a hard requirement. */
static gboolean
append_flash_block (guint8 *buf, gsize buf_size, gsize *offset, guint16 id,
                    const guint8 *body, gsize body_len)
{
  if (body_len > G_MAXUINT16 ||
      *offset > buf_size ||
      body_len + 4 + 32 > buf_size - *offset)
    return FALSE;

  /* Header: [id:2LE][size:2LE] */
  validity_pack (buf + *offset, 4, "hh", id, (guint16) body_len);
  *offset += 4;

  /* SHA-256 of body */
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, body, body_len);
  gsize hash_len = 32;
  g_checksum_get_digest (checksum, buf + *offset, &hash_len);
  *offset += 32;

  /* Body */
  memcpy (buf + *offset, body, body_len);
  *offset += body_len;

  return TRUE;
}

guint8 *
validity_pair_build_tls_flash (const ValidityPairState *state,
                               const guint8            *ca_cert,
                               gsize                    ca_cert_len,
                               gsize                   *out_len)
{
  guint8 *buf = g_malloc (TLS_FLASH_IMAGE_SIZE);

  /* Fill with 0xff initially (padding) */
  memset (buf, 0xff, TLS_FLASH_IMAGE_SIZE);

  guint8 zero_byte = 0x00;
  guint8 empty_block[256];
  memset (empty_block, 0, sizeof (empty_block));

  gsize offset = 0;
  gboolean ok = TRUE;

  /* Block 0: empty marker */
  ok = ok && append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 0, &zero_byte, 1);

  /* Block 4: encrypted private key */
  if (ok && state->priv_blob && state->priv_blob_len > 0)
    ok = append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 4,
                             state->priv_blob, state->priv_blob_len);

  /* Block 3: server certificate (length is device-controlled) */
  if (ok && state->server_cert && state->server_cert_len > 0)
    ok = append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 3,
                             state->server_cert, state->server_cert_len);

  /* Block 5: CA certificate (from runtime data files) */
  ok = ok && append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 5, ca_cert, ca_cert_len);

  /* Block 1: empty placeholder (256 zeros) */
  ok = ok && append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 1, empty_block, sizeof (empty_block));

  /* Block 2: empty placeholder (256 zeros) */
  ok = ok && append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 2, empty_block, sizeof (empty_block));

  /* Block 6: ECDH blob (length is device-controlled) */
  if (ok && state->ecdh_blob && state->ecdh_blob_len > 0)
    ok = append_flash_block (buf, TLS_FLASH_IMAGE_SIZE, &offset, 6,
                             state->ecdh_blob, state->ecdh_blob_len);

  if (!ok)
    {
      /* A device-supplied block did not fit in the fixed flash image. */
      fp_warn ("TLS flash image build overflowed -- refusing to pair");
      g_free (buf);
      *out_len = 0;
      return NULL;
    }

  /* Remaining bytes stay 0xff from initial memset */
  *out_len = TLS_FLASH_IMAGE_SIZE;
  return buf;
}

/* ================================================================
 * Pairing SSM runner
 *
 * Drives the full pairing sequence through USB commands.
 * Requires self->pair_state to be initialized.
 * Uses raw USB for pre-TLS phase and TLS-wrapped for post-TLS.
 * ================================================================ */


/* Fail the pair SSM because pairing was refused or stopped on a 0x0404. The
 * refused flag makes pair_ssm_done fail the open with @error instead of
 * continuing as it does for other pairing errors. */
static void
pair_refuse (FpiSsm            *ssm,
             FpiDeviceValidity *self,
             GError            *error)
{
  self->pair_state.refused = TRUE;
  fpi_ssm_mark_failed (ssm, error);
}

/* Act on self->pair_state.decision: skip pairing, start it, or refuse with a
 * message that says what state the sensor is in. Only PROVISION starts
 * pairing; any other value, including one this switch does not know, ends
 * the SSM before reset_blob is sent. */
static void
pair_apply_decision (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  switch (ps->decision)
    {
    case VALIDITY_PAIR_NOT_NEEDED:
      fp_info ("TLS keys verified on flash — pairing not needed");
      fpi_ssm_jump_to_state (ssm, PAIR_DONE);
      return;

    case VALIDITY_PAIR_PROVISION:
      fp_info ("Flash has 0 partitions — device needs pairing");
      break;

    case VALIDITY_PAIR_REFUSE_BLANK:
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "The sensor's flash is partitioned, "
                                             "but partition 1 holds no pairing "
                                             "keys (for example after an "
                                             "interrupted pairing). Provisioning "
                                             "it again needs a factory reset, "
                                             "which erases the whole sensor, "
                                             "including any fingerprints stored "
                                             "on it, so it is not done "
                                             "automatically"));
      return;

    case VALIDITY_PAIR_REFUSE_UNREADABLE:
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                             "Could not get a complete read of the "
                                             "sensor's pairing data (partition 1, "
                                             "status 0x%04x, %" G_GSIZE_FORMAT " bytes); not pairing",
                                             self->cmd_response_status,
                                             self->cmd_response_len));
      return;

    case VALIDITY_PAIR_REFUSE_PARTIAL:
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "The sensor holds pairing data this "
                                             "driver cannot use (partition 1 has no "
                                             "complete key set). Re-pairing it would "
                                             "need a factory reset, which erases the "
                                             "fingerprints stored on the sensor, so "
                                             "it is not done automatically"));
      return;

    case VALIDITY_PAIR_REFUSE_RECOVERY:
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "Sensor needs pairing, which is never "
                                             "started from an in-driver reconnect"));
      return;

    default:
      /* Fail closed: only an explicit PROVISION may start pairing */
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "Unknown pairing decision %d; "
                                             "not pairing",
                                             (gint) ps->decision));
      return;
    }

  /* Only VALIDITY_PAIR_PROVISION gets here. Look up device descriptor */
  ps->dev_desc = validity_hal_device_lookup (self->dev_type);
  if (!ps->dev_desc)
    {
      fp_warn ("No HAL descriptor for dev_type=%u", self->dev_type);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  fpi_ssm_jump_to_state (ssm, PAIR_SEND_RESET_BLOB);
}

static void
pair_check_needed (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Parse CMD 0x3e response: status(2) + data */
  if (!self->cmd_response_data ||
      self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("GET_FLASH_INFO failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  if (!validity_pair_parse_flash_info (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &ps->flash_ic,
                                       &ps->num_partitions))
    {
      fp_warn ("Failed to parse flash info");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  if (ps->num_partitions > 0)
    {
      fp_info ("Flash has %u partitions — verifying TLS keys",
               ps->num_partitions);
      /* Read flash partition 1 to check if TLS keys exist */
      fpi_ssm_next_state (ssm);
      return;
    }

  /* No partitions: partition 1 is not read, so decide straight away */
  ps->decision = validity_pair_decide (0, VALIDITY_TLS_FLASH_UNREADABLE,
                                       ps->check_only);
  pair_apply_decision (ssm, self);
}

static void
pair_verify_tls_recv (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;
  ValidityTlsFlashClass flash;

  flash = validity_pair_classify_tls_flash (self->cmd_response_status,
                                            self->cmd_response_data,
                                            self->cmd_response_len);
  ps->decision = validity_pair_decide (ps->num_partitions, flash,
                                       ps->check_only);
  pair_apply_decision (ssm, self);
}

static void
pair_send_reset_blob (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  gsize reset_len;
  const guint8 *reset_data;

  /* Send reset_blob via raw USB (python-validity: usb.cmd(reset_blob)) */
  reset_data = validity_data_get_bytes (&self->device_data,
                                        VALIDITY_DATA_RESET, &reset_len);
  if (!reset_data || reset_len == 0)
    {
      fp_warn ("No reset_blob available for this device");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  vcsfw_cmd_send (self, ssm, reset_data, reset_len, NULL);
}

static void
pair_generate_keys (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Generate ECDH P-256 key pair */
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id (EVP_PKEY_EC, NULL);

  EVP_PKEY_keygen_init (pctx);
  EVP_PKEY_CTX_set_ec_paramgen_curve_nid (pctx, NID_X9_62_prime256v1);
  EVP_PKEY_keygen (pctx, &ps->client_key);
  EVP_PKEY_CTX_free (pctx);

  if (!ps->client_key)
    {
      fp_warn ("ECDH key generation failed");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  fp_info ("Generated ECDH client key pair");
  fpi_ssm_next_state (ssm);
}

static void
pair_partition_flash_send (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Extract public key coordinates (little-endian) */
  BIGNUM *pub_x_bn = NULL, *pub_y_bn = NULL;

  EVP_PKEY_get_bn_param (ps->client_key, OSSL_PKEY_PARAM_EC_PUB_X, &pub_x_bn);
  EVP_PKEY_get_bn_param (ps->client_key, OSSL_PKEY_PARAM_EC_PUB_Y, &pub_y_bn);

  guint8 pub_x_be[32], pub_y_be[32];
  guint8 pub_x_le[32], pub_y_le[32];

  BN_bn2binpad (pub_x_bn, pub_x_be, 32);
  BN_bn2binpad (pub_y_bn, pub_y_be, 32);
  BN_free (pub_x_bn);
  BN_free (pub_y_bn);

  /* Convert big-endian → little-endian (python-validity uses LE) */
  for (int i = 0; i < 32; i++)
    {
      pub_x_le[i] = pub_x_be[31 - i];
      pub_y_le[i] = pub_y_be[31 - i];
    }

  /* Build CMD 0x4f — get data from runtime data store */
  gsize partition_sig_len, password_len, ca_cert_len;
  const guint8 *partition_sig_data;
  const guint8 *password_data;
  const guint8 *ca_cert_data;

  /* Select partition signature based on PID */
  ValidityDataTag sig_tag = (ps->dev_desc->pid == 0x0090) ?
                            VALIDITY_DATA_PARTITION_SIG_0090 :
                            VALIDITY_DATA_PARTITION_SIG_STANDARD;

  partition_sig_data = validity_data_get_bytes (&self->common_data,
                                                sig_tag, &partition_sig_len);
  password_data = validity_data_get_bytes (&self->common_data,
                                           VALIDITY_DATA_TLS_PASSWORD,
                                           &password_len);
  ca_cert_data = validity_data_get_bytes (&self->common_data,
                                          VALIDITY_DATA_CA_PUBKEY,
                                          &ca_cert_len);

  if (!partition_sig_data || !password_data || !ca_cert_data)
    {
      fp_warn ("Missing runtime data for pairing");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }

  gsize cmd_len;
  g_autofree guint8 *cmd = validity_pair_build_partition_flash_cmd (
    &ps->flash_ic,
    ps->dev_desc->flash_layout,
    partition_sig_data, partition_sig_len,
    pub_x_le, pub_y_le,
    password_data, password_len,
    ca_cert_data, ca_cert_len,
    &cmd_len);

  if (!cmd)
    {
      fp_warn ("Failed to build partition_flash command");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  fp_info ("Sending partition_flash (CMD 0x4f): %" G_GSIZE_FORMAT " bytes",
           cmd_len);

  /* NOTE: partition_flash is sent via raw USB (TLS not yet active).
   * python-validity sends it through tls.cmd() which falls back to
   * raw USB when secure_rx/secure_tx are false. */
  vcsfw_cmd_send (self, ssm, cmd, cmd_len, NULL);
}

static void
pair_partition_flash_recv (FpiSsm            *ssm,
                           FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  if (self->cmd_response_status == 0x0404)
    {
      /* 0x0404 has been read both as "partitions already exist" and as
       * insufficient privilege; either way the firmware declined to
       * partition, although GET_FLASH_INFO reported no partitions. Fail the
       * open with a specific error. Never escalate to a factory reset
       * (cmd 0x10), which would wipe the whole sensor: python-validity's
       * init_flash also just fails on any partition_flash error, and it
       * factory-resets only from a separate script. */
      pair_refuse (ssm, self,
                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "The sensor reported no partitions "
                                             "but refused to partition its flash "
                                             "(partition_flash returned 0x0404), "
                                             "so it was not paired. The driver "
                                             "does not factory-reset the sensor "
                                             "to get past this"));
      return;
    }

  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("partition_flash failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Response: [cert_len:4LE][cert_data:cert_len][...] */
  if (self->cmd_response_data && self->cmd_response_len >= 4)
    {
      guint32 cert_len = 0;
      validity_unpack (self->cmd_response_data, self->cmd_response_len,
                       "w", &cert_len);
      if (cert_len <= self->cmd_response_len - 4)
        {
          ps->server_cert = g_memdup2 (self->cmd_response_data + 4,
                                       cert_len);
          ps->server_cert_len = cert_len;
          fp_info ("Received server certificate: %u bytes", cert_len);
        }
    }

  fpi_ssm_next_state (ssm);
}

static void
pair_cmd50_process (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Response: [length:4LE][zeros:...][ecdh_blob:400]
   * python-validity:
   *   l, = unpack('<L', rsp[:4])
   *   zeroes, rsp = rsp[4:-400], rsp[-400:] */
  if (!self->cmd_response_data || self->cmd_response_len < 404)
    {
      fp_warn ("CMD 0x50 response too short: %" G_GSIZE_FORMAT,
               self->cmd_response_len);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  guint32 resp_len = 0;
  validity_unpack (self->cmd_response_data, self->cmd_response_len,
                   "w", &resp_len);
  const guint8 *ecdh_data = self->cmd_response_data +
                            self->cmd_response_len - 400;

  fp_info ("CMD 0x50 response: declared_len=%u, actual=%" G_GSIZE_FORMAT,
           resp_len, self->cmd_response_len);

  /* Store ECDH blob: handle_ecdh stores raw blob, extracts pubkey.
   * We store it for TLS flash persistence and set up tls.ecdh_q
   * via the existing validity_tls code path. */
  ps->ecdh_blob = g_memdup2 (ecdh_data, 400);
  ps->ecdh_blob_len = 400;

  /* Parse ECDH blob to extract server public key.
   * This sets self->tls.ecdh_q and ecdh_blob.
   *
   * SECURITY: the sensor's ECDH public key is accepted here with NO signature
   * verification against the firmware public key (the open path in
   * validity_tls.c does attempt it, but only non-fatally). A malicious or
   * MITM'd sensor can therefore supply an attacker-chosen key and stand up a
   * TLS session it controls. This matches python-validity, which cannot verify
   * either because the required firmware pubkeys are not reliably available per
   * device. Making verification fatal needs per-model firmware-pubkey data
   * validated against real hardware; tracked as a known limitation. */
  self->tls.ecdh_blob = g_memdup2 (ecdh_data, 400);
  self->tls.ecdh_blob_len = 400;

  /* Extract X,Y coordinates from ECDH blob for ecdh_q.
   * Format: [header:8][x:32 LE][padding:36][y:32 LE][...] */
  const guint8 *x_le = ecdh_data + TLS_ECDH_X_OFFSET;
  const guint8 *y_le = ecdh_data + TLS_ECDH_Y_OFFSET;

  guint8 x_be[32], y_be[32];
  for (int i = 0; i < 32; i++)
    {
      x_be[i] = x_le[31 - i];
      y_be[i] = y_le[31 - i];
    }

  /* Build ECDH server public key */
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new ();
  OSSL_PARAM_BLD_push_utf8_string (bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                   "prime256v1", 0);
  guint8 pub_uncompressed[65];
  pub_uncompressed[0] = 0x04;
  memcpy (pub_uncompressed + 1, x_be, 32);
  memcpy (pub_uncompressed + 33, y_be, 32);
  OSSL_PARAM_BLD_push_octet_string (bld, OSSL_PKEY_PARAM_PUB_KEY,
                                    pub_uncompressed, 65);

  OSSL_PARAM *params = OSSL_PARAM_BLD_to_param (bld);
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  EVP_PKEY_fromdata_init (ctx);
  EVP_PKEY_fromdata (ctx, &self->tls.ecdh_q, EVP_PKEY_PUBLIC_KEY, params);
  EVP_PKEY_CTX_free (ctx);
  OSSL_PARAM_free (params);
  OSSL_PARAM_BLD_free (bld);

  if (!self->tls.ecdh_q)
    {
      fp_warn ("Failed to build ECDH server public key");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Encrypt client private key → priv_blob (handle_priv)
   * python-validity: tls.handle_priv(encrypt_key(client_private, client_public)) */

  /* First, derive PSK if not already done */
  validity_tls_derive_psk (&self->tls);

  /* Extract private key scalar (little-endian) */
  BIGNUM *priv_bn = NULL;
  EVP_PKEY_get_bn_param (ps->client_key, OSSL_PKEY_PARAM_PRIV_KEY, &priv_bn);

  guint8 priv_be[32], priv_le[32];
  BN_bn2binpad (priv_bn, priv_be, 32);
  BN_free (priv_bn);

  for (int i = 0; i < 32; i++)
    priv_le[i] = priv_be[31 - i];

  /* Get public key LE coords */
  BIGNUM *pub_x_bn = NULL, *pub_y_bn = NULL;
  EVP_PKEY_get_bn_param (ps->client_key, OSSL_PKEY_PARAM_EC_PUB_X, &pub_x_bn);
  EVP_PKEY_get_bn_param (ps->client_key, OSSL_PKEY_PARAM_EC_PUB_Y, &pub_y_bn);

  guint8 pub_x_be2[32], pub_y_be2[32];
  guint8 pub_x_le2[32], pub_y_le2[32];
  BN_bn2binpad (pub_x_bn, pub_x_be2, 32);
  BN_bn2binpad (pub_y_bn, pub_y_be2, 32);
  BN_free (pub_x_bn);
  BN_free (pub_y_bn);

  for (int i = 0; i < 32; i++)
    {
      pub_x_le2[i] = pub_x_be2[31 - i];
      pub_y_le2[i] = pub_y_be2[31 - i];
    }

  gsize priv_blob_len;
  ps->priv_blob = validity_pair_encrypt_key (priv_le, pub_x_le2, pub_y_le2,
                                             self->tls.psk_encryption_key,
                                             self->tls.psk_validation_key,
                                             &priv_blob_len);
  ps->priv_blob_len = priv_blob_len;

  /* Also store in TLS state for handle_priv path */
  self->tls.priv_blob = g_memdup2 (ps->priv_blob, ps->priv_blob_len);
  self->tls.priv_blob_len = ps->priv_blob_len;

  /* Store server cert in TLS state too */
  if (ps->server_cert)
    {
      self->tls.tls_cert = g_memdup2 (ps->server_cert, ps->server_cert_len);
      self->tls.tls_cert_len = ps->server_cert_len;
    }

  /* Set priv_key — the TLS handshake needs the actual EC private key
  * (EVP_PKEY*) to sign cert_verify. We have it as ps->client_key. */
  if (self->tls.priv_key)
    EVP_PKEY_free (self->tls.priv_key);
  self->tls.priv_key = EVP_PKEY_dup (ps->client_key);

  OPENSSL_cleanse (priv_le, sizeof (priv_le));
  OPENSSL_cleanse (priv_be, sizeof (priv_be));

  fp_info ("ECDH exchange complete, private key encrypted");
  fpi_ssm_next_state (ssm);
}

/* ---- Phase 2: TLS handshake ---- */

static void
pair_tls_handshake (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  /* Establish TLS session — python-validity: tls.open()
   * Uses subsm: tls_ssm completion/failure propagates to pair SSM.
   * NOTE: do NOT overwrite self->open_ssm here — it must remain
   * pointing to the open SSM for pair_ssm_done to work. */
  FpiSsm *tls_ssm = fpi_ssm_new (dev,
                                 validity_tls_handshake_run_state,
                                 TLS_HS_NUM_STATES);

  fpi_ssm_start_subsm (ssm, tls_ssm);

  return;
}

/* ---- Phase 3: Flash erase (TLS-wrapped) ---- */

static void
pair_erase_dbe_send (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Send db_write_enable before each erase
   * python-validity: erase_flash() → tls.cmd(db_write_enable) */
  fp_info ("Erasing partition %u (step %u/%u)",
           pair_erase_partition_ids[ps->erase_step],
           ps->erase_step + 1,
           (guint) VALIDITY_PAIR_NUM_ERASE_STEPS);

  gsize dbe_len;
  const guint8 *dbe_data = validity_data_get_bytes (&self->device_data,
                                                    VALIDITY_DATA_DB_WRITE_ENABLE,
                                                    &dbe_len);
  if (!dbe_data || dbe_len == 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }
  vcsfw_tls_cmd_send (self, ssm, dbe_data, dbe_len, NULL);
}

static void
pair_erase_loop (FpiSsm            *ssm,
                 FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  ps->erase_step++;
  if (ps->erase_step < VALIDITY_PAIR_NUM_ERASE_STEPS)
    {
      fpi_ssm_jump_to_state (ssm, PAIR_ERASE_DBE_SEND);
      return;
    }
  fpi_ssm_next_state (ssm);

  return;
}

/* ---- Phase 4: Write TLS flash (TLS-wrapped) ---- */

static void
pair_write_dbe_send (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  /* db_write_enable before write_flash */
  gsize dbe_len;
  const guint8 *dbe_data = validity_data_get_bytes (&self->device_data,
                                                    VALIDITY_DATA_DB_WRITE_ENABLE,
                                                    &dbe_len);

  if (!dbe_data || dbe_len == 0)
    {
      fp_warn ("No db_write_enable data for pairing");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }
  vcsfw_tls_cmd_send (self, ssm, dbe_data, dbe_len, NULL);
}

static void
pair_write_flash_send (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Build TLS flash image */
  gsize ca_len;
  const guint8 *ca_data = validity_data_get_bytes (&self->common_data,
                                                   VALIDITY_DATA_CA_PUBKEY,
                                                   &ca_len);
  gsize flash_len;
  g_autofree guint8 *flash_data =
    validity_pair_build_tls_flash (ps, ca_data, ca_len, &flash_len);

  if (!flash_data)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* CMD 0x41: WRITE_FLASH
   * Format: [0x41][partition:1][flag:1][reserved:2][offset:4LE][size:4LE][data]
   * python-validity: pack('<BBBHLL', 0x41, partition, 1, 0, addr, len(buf)) + buf */
  gsize cmd_len = 1 + 1 + 1 + 2 + 4 + 4 + flash_len;
  guint8 *cmd = g_malloc0 (cmd_len);

  validity_pack (cmd, cmd_len, "bbbxxwwd",
                 VCSFW_CMD_WRITE_FLASH,
                 (guint8) 1,   /* partition 1 (cert store) */
                 (guint8) 1,   /* flag */
                 (guint32) 0,  /* offset = 0 */
                 (guint32) flash_len,
                 flash_data, (gsize) flash_len);

  fp_info ("Writing TLS flash: %" G_GSIZE_FORMAT " bytes to partition 1",
           flash_len);
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

/* ---- Phase 5: Reboot ---- */
static void
pair_reboot_send (FpiSsm            *ssm,
                  FpiDeviceValidity *self)
{
  ValidityPairState *ps = &self->pair_state;

  /* Reboot: 0x05 0x02 0x00, sent over raw USB. python-validity sends it
   * with tls.cmd(unhex('050200')), which goes over the TLS session that
   * pairing set up earlier. */
  guint8 cmd[] = { VCSFW_CMD_REBOOT, 0x02, 0x00 };

  ps->reboot_pending = TRUE;
  vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
}

void
validity_pair_run_state (FpiSsm   *ssm,
                         FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case PAIR_GET_FLASH_INFO:
      {
        guint8 cmd[] = { VCSFW_CMD_GET_FLASH_INFO };
        vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_GET_FLASH_INFO_RECV:
      /* Response already in self->cmd_response_* from vcsfw_cmd_send sub-SSM.
       * Fall through to CHECK_NEEDED which parses it. */
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_CHECK_NEEDED:
      pair_check_needed (ssm, self);
      break;

    case PAIR_VERIFY_TLS_SEND:
      {
        guint8 cmd[13];
        validity_pack (cmd, sizeof (cmd), "bbbhww",
                       VCSFW_CMD_READ_FLASH,
                       (guint8) 0x01,
                       (guint8) 0x01,
                       (guint16) 0x0000,
                       (guint32) 0x0000,
                       (guint32) 0x1000);
        vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_VERIFY_TLS_RECV:
      pair_verify_tls_recv (ssm, self);
      break;

    case PAIR_SEND_RESET_BLOB:
      pair_send_reset_blob (ssm, self);
      break;

    case PAIR_SEND_RESET_BLOB_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("reset_blob failed: status=0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_GENERATE_KEYS:
      pair_generate_keys (ssm, self);
      break;

    case PAIR_PARTITION_FLASH_SEND:
      pair_partition_flash_send (ssm, self);
      break;

    case PAIR_PARTITION_FLASH_RECV:
      pair_partition_flash_recv (ssm, self);
      break;

    case PAIR_CMD50_SEND:
      {
        guint8 cmd[] = { VCSFW_CMD_GET_ECDH };
        vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_CMD50_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("CMD 0x50 failed: status=0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_CMD50_PROCESS:
      pair_cmd50_process (ssm, self);
      break;

    case PAIR_CLEANUPS_SEND:
      {
        guint8 cmd[] = { VCSFW_CMD_CLEANUPS };
        vcsfw_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_CLEANUPS_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK &&
          self->cmd_response_status != 0x0491)
        fp_warn ("cleanups failed: status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_TLS_HANDSHAKE:
      /* Starts an async TLS handshake sub-SSM; on completion the parent SSM
       * auto-advances to PAIR_ERASE_DBE_SEND. Must break here -- falling
       * through would emit TLS app-data with not-yet-derived session keys. */
      pair_tls_handshake (ssm, self);
      break;

    case PAIR_ERASE_DBE_SEND:
      pair_erase_dbe_send (ssm, self);
      break;

    case PAIR_ERASE_DBE_RECV:
      /* db_write_enable response — proceed regardless of status */
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_ERASE_SEND:
      {
        ValidityPairState *ps = &self->pair_state;
        guint8 cmd[2];
        validity_pack (cmd, sizeof (cmd), "bb",
                       VCSFW_CMD_ERASE_FLASH,
                       pair_erase_partition_ids[ps->erase_step]);
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_ERASE_RECV:
      {
        ValidityPairState *ps = &self->pair_state;
        if (self->cmd_response_status != VCSFW_STATUS_OK)
          fp_warn ("erase partition %u failed: status=0x%04x",
                   pair_erase_partition_ids[ps->erase_step],
                   self->cmd_response_status);
        fpi_ssm_next_state (ssm);
        break;
      }

    case PAIR_ERASE_CLEAN_SEND:
      {
        guint8 cmd[] = { VCSFW_CMD_CLEANUPS };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_ERASE_CLEAN_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK &&
          self->cmd_response_status != 0x0491)
        fp_warn ("post-erase cleanups status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_ERASE_LOOP:
      /* Self-advances (jump back to erase, or next state when done). Must
       * break -- falling through would fire a duplicate TLS command. */
      pair_erase_loop (ssm, self);
      break;

    case PAIR_WRITE_DBE_SEND:
      pair_write_dbe_send (ssm, self);
      break;

    case PAIR_WRITE_DBE_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_WRITE_FLASH_SEND:
      pair_write_flash_send (ssm, self);
      break;

    case PAIR_WRITE_FLASH_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("write_flash failed: status=0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_WRITE_CLEAN_SEND:
      {
        guint8 cmd[] = { VCSFW_CMD_CLEANUPS };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
        break;
      }

    case PAIR_WRITE_CLEAN_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK &&
          self->cmd_response_status != 0x0491)
        fp_warn ("post-write cleanups status=0x%04x",
                 self->cmd_response_status);
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_REBOOT_SEND:
      pair_reboot_send (ssm, self);
      break;

    case PAIR_REBOOT_RECV:
      fp_info ("Reboot command sent — device will re-enumerate");
      fpi_ssm_next_state (ssm);
      break;

    case PAIR_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

FpiSsm *
validity_pair_ssm_new (FpDevice *dev)
{
  return fpi_ssm_new (dev, validity_pair_run_state, PAIR_NUM_STATES);
}
