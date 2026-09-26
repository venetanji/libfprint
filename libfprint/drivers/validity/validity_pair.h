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

#pragma once

#include <glib.h>
#include <openssl/evp.h>
#include "validity_hal.h"

/* Forward declaration */
typedef struct _FpiDeviceValidity FpiDeviceValidity;
typedef struct _FpiSsm            FpiSsm;

/* Flash IC parameters — returned by CMD 0x3e (GET_FLASH_INFO) */
typedef struct
{
  guint32 size;
  guint32 sector_size;
  guint8  sector_erase_cmd;
} ValidityFlashIcParams;

/* What the partition-1 (host key store) read says about the pairing.
 * See validity_pair_classify_tls_flash(). */
typedef enum {
  VALIDITY_TLS_FLASH_KEYS_PRESENT = 0, /* cert (3), private key (4), ECDH (6) */
  VALIDITY_TLS_FLASH_BLANK,            /* complete read, end marker first */
  VALIDITY_TLS_FLASH_PARTIAL,          /* some blocks, but not all three keys */
  VALIDITY_TLS_FLASH_UNREADABLE,       /* bad status, or short/unparseable read */
} ValidityTlsFlashClass;

/* What the pairing check decided open may do. See validity_pair_decide().
 * PROVISION is the only value that starts pairing. */
typedef enum {
  VALIDITY_PAIR_NOT_NEEDED = 0,     /* keys present: nothing to do */
  VALIDITY_PAIR_PROVISION,          /* 0 partitions: provision from scratch */
  VALIDITY_PAIR_REFUSE_BLANK,       /* partitioned, but partition 1 blank */
  VALIDITY_PAIR_REFUSE_UNREADABLE,  /* could not read partition 1 */
  VALIDITY_PAIR_REFUSE_PARTIAL,     /* key data we cannot use */
  VALIDITY_PAIR_REFUSE_RECOVERY,    /* would provision, but in a reconnect */
} ValidityPairDecision;

/* Pairing state kept across SSM states */
typedef struct
{
  /* Flash IC params from CMD 0x3e */
  ValidityFlashIcParams flash_ic;
  guint16               num_partitions;  /* 0 = needs pairing */

  /* Set by the caller before starting the SSM: run the read-only pairing
   * check, but refuse to pair. Used by the in-driver reconnect. */
  gboolean check_only;

  /* Result of the pairing check (validity_pair_decide) */
  ValidityPairDecision decision;

  /* Set when the SSM refused to pair, or stopped pairing on a 0x0404 from
   * partition_flash. pair_ssm_done then fails the open with the SSM's error
   * instead of continuing. */
  gboolean refused;

  /* ECDH client key pair (generated during pairing) */
  EVP_PKEY *client_key;

  /* Server certificate returned by partition_flash (CMD 0x4f) */
  guint8 *server_cert;
  gsize   server_cert_len;

  /* ECDH blob from CMD 0x50 (server's DH params) */
  guint8 *ecdh_blob;
  gsize   ecdh_blob_len;

  /* Encrypted private key blob (client → flash) */
  guint8 *priv_blob;
  gsize   priv_blob_len;

  /* Device descriptor for this PID */
  const ValidityDeviceDesc *dev_desc;

  /* Flash erase progress counter */
  guint erase_step;

  /* Set TRUE when the reboot command at the end of pairing has been sent */
  gboolean reboot_pending;
} ValidityPairState;

/* Partition entry serialized format: 12 bytes data + 4 zero + 32 SHA-256 = 48 */
#define VALIDITY_PARTITION_ENTRY_SIZE 48

/* Client certificate size */
#define VALIDITY_CLIENT_CERT_SIZE 444

/* CMD 0x4f header IDs */
#define VALIDITY_HDR_FLASH_IC 0
#define VALIDITY_HDR_PARTITION_TABLE 1
#define VALIDITY_HDR_CA_CERT 3
#define VALIDITY_HDR_CLIENT_CERT 5

/* Encrypted private key format: 0x02 prefix + IV(16) + ciphertext(112) + HMAC(32) = 161 */
#define VALIDITY_ENCRYPTED_KEY_PREFIX 0x02
#define VALIDITY_ENCRYPTED_KEY_IV_SIZE 16
#define VALIDITY_EC_COORD_SIZE 32

/* Flash partition IDs for erase during pairing */
static const guint8 pair_erase_partition_ids[] = { 1, 2, 5, 6, 4 };
#define VALIDITY_PAIR_NUM_ERASE_STEPS G_N_ELEMENTS (pair_erase_partition_ids)

/* ---- Helper functions (testable independently) ---- */

/**
 * validity_pair_serialize_partition:
 * @part: partition entry to serialize
 * @out: output buffer (must be at least VALIDITY_PARTITION_ENTRY_SIZE bytes)
 *
 * Serialize a partition entry to binary format:
 *   [id:1][type:1][access_lvl:2LE][offset:4LE][size:4LE][zeros:4][SHA256:32]
 */
void validity_pair_serialize_partition (const ValidityPartition *part,
                                        guint8                  *out);

/**
 * validity_pair_build_partition_flash_cmd:
 * @flash_ic: flash IC parameters from CMD 0x3e
 * @layout: flash layout for this device
 * @client_public_x: ECDH client public key X coordinate (32 bytes, little-endian)
 * @client_public_y: ECDH client public key Y coordinate (32 bytes, little-endian)
 * @out_len: output command length
 *
 * Build the CMD 0x4f (PARTITION_FLASH) payload:
 *   [4f 0000 0000]
 *   [hdr0: flash IC params]
 *   [hdr1: partition table + RSA signature]
 *   [hdr5: client certificate (444 bytes)]
 *   [hdr3: CA certificate]
 *
 * Returns: newly allocated command buffer, free with g_free().
 */
guint8 *validity_pair_build_partition_flash_cmd (const ValidityFlashIcParams *flash_ic,
                                                 const ValidityFlashLayout   *layout,
                                                 const guint8                *partition_sig,
                                                 gsize                        partition_sig_len,
                                                 const guint8                *client_public_x,
                                                 const guint8                *client_public_y,
                                                 const guint8                *password,
                                                 gsize                        password_len,
                                                 const guint8                *ca_cert,
                                                 gsize                        ca_cert_len,
                                                 gsize                       *out_len);

/**
 * validity_pair_make_cert:
 * @client_public_x: ECDH client public key X (32 bytes, little-endian)
 * @client_public_y: ECDH client public key Y (32 bytes, little-endian)
 * @out_len: set to VALIDITY_CLIENT_CERT_SIZE on success
 *
 * Build a 444-byte client certificate with ECDSA signature.
 * The HS key is derived from the hardcoded password.
 *
 * Returns: newly allocated cert buffer, or NULL on error. Free with g_free().
 */
guint8 *validity_pair_make_cert (const guint8 *client_public_x,
                                 const guint8 *client_public_y,
                                 const guint8 *password,
                                 gsize         password_len,
                                 gsize        *out_len);

/**
 * validity_pair_encrypt_key:
 * @client_private: ECDH client private key scalar (32 bytes, little-endian)
 * @client_public_x: ECDH client public key X (32 bytes, little-endian)
 * @client_public_y: ECDH client public key Y (32 bytes, little-endian)
 * @psk_encryption_key: PSK encryption key (32 bytes)
 * @psk_validation_key: PSK validation key (32 bytes)
 * @out_len: output blob length
 *
 * Encrypt the ECDH private key for flash storage:
 *   plaintext = x(32) + y(32) + d(32) + PKCS7 padding
 *   ciphertext = AES-256-CBC(psk_encryption_key, random_iv, plaintext)
 *   blob = 0x02 + iv(16) + ciphertext + HMAC-SHA256(psk_validation_key, iv+ciphertext)
 *
 * Returns: newly allocated blob, or NULL on error. Free with g_free().
 */
guint8 *validity_pair_encrypt_key (const guint8 *client_private,
                                   const guint8 *client_public_x,
                                   const guint8 *client_public_y,
                                   const guint8 *psk_encryption_key,
                                   const guint8 *psk_validation_key,
                                   gsize        *out_len);

/**
 * validity_pair_parse_flash_info:
 * @data: response payload from CMD 0x3e (after 2-byte status)
 * @data_len: length of payload
 * @ic_out: output flash IC params
 * @num_partitions_out: output number of existing partitions
 *
 * Parse GET_FLASH_INFO (CMD 0x3e) response.
 * If num_partitions_out is 0, device needs pairing.
 *
 * Returns: TRUE on success, FALSE on parse error.
 */
gboolean validity_pair_parse_flash_info (const guint8          *data,
                                         gsize                  data_len,
                                         ValidityFlashIcParams *ic_out,
                                         guint16               *num_partitions_out);

/**
 * validity_pair_classify_tls_flash:
 * @status: VCSFW status of the partition-1 READ_FLASH (CMD 0x40)
 * @data: response payload after the status: [size:4LE][unknown:2][flash]
 * @len: length of @data
 *
 * Classify the host key store. KEYS_PRESENT needs blocks 3, 4 and 6 (as
 * before; the TLS parse validates them later). BLANK needs a complete read
 * whose first block header is the 0xFFFF end marker (erased flash reads as
 * 0xFF). A read that is short, truncated or runs off the end of its data
 * before finding the keys is UNREADABLE, never BLANK, so a bad read is not
 * reported as an empty partition 1.
 *
 * Returns: the #ValidityTlsFlashClass.
 */
ValidityTlsFlashClass validity_pair_classify_tls_flash (guint16       status,
                                                        const guint8 *data,
                                                        gsize         len);

/**
 * validity_pair_decide:
 * @num_partitions: partition count from GET_FLASH_INFO
 * @flash: partition-1 class; ignored when @num_partitions is 0 (partition 1
 *   is not read then)
 * @check_only: %TRUE when pairing must not start (in-driver reconnect)
 *
 * Only a sensor with no partitions may be paired (PROVISION, as
 * python-validity's init_flash does), and never when @check_only is set.
 * On a partitioned sensor, keys present needs nothing and every other class
 * is refused, whatever @check_only is.
 *
 * Returns: the #ValidityPairDecision.
 */
ValidityPairDecision validity_pair_decide (guint16               num_partitions,
                                           ValidityTlsFlashClass flash,
                                           gboolean              check_only);

/**
 * validity_pair_state_init:
 * @state: pairing state to initialize
 *
 * Zero-initialize the pairing state.
 */
void validity_pair_state_init (ValidityPairState *state);

/**
 * validity_pair_state_free:
 * @state: pairing state to free
 *
 * Free any allocated resources in the pairing state.
 */
void validity_pair_state_free (ValidityPairState *state);

/* ================================================================
 * Pairing SSM
 *
 * The pairing SSM runs on every open. It does nothing more when
 * partition 1 holds the host keys, pairs only an uninitialized device
 * (CMD 0x3e returns 0 partitions), and refuses otherwise (see
 * validity_pair_decide). Pairing formats the flash, exchanges ECDH keys,
 * establishes a TLS session, persists the TLS keys to flash, and reboots
 * the device. It never sends a factory reset: a 0x0404 from
 * PARTITION_FLASH fails the open.
 *
 * Phase 1 (pre-TLS, raw USB):
 *   - GET_FLASH_INFO  → check if pairing needed
 *   - SEND_RESET_BLOB → reset device state
 *   - GENERATE_KEYS   → ECDH key generation
 *   - PARTITION_FLASH  → format flash + write certs (CMD 0x4f)
 *   - RECV_PARTITION   → parse server cert from response
 *   - CMD50_SEND       → request ECDH server params
 *   - CMD50_RECV       → parse ECDH blob + encrypt private key
 *   - CLEANUPS_SEND    → call cleanups (0x1a) after CMD 0x50
 *   - CLEANUPS_RECV    → receive cleanups response
 *
 * Phase 2 (TLS):
 *   - TLS_HANDSHAKE    → establish TLS session
 *   - ERASE_DBE_SEND   → send db_write_enable before erase
 *   - ERASE_DBE_RECV   → receive dbe response
 *   - ERASE_SEND       → erase partition (CMD 0x3f)
 *   - ERASE_RECV       → receive erase response
 *   - ERASE_CLEAN_SEND → call cleanups after erase
 *   - ERASE_CLEAN_RECV → receive cleanups response
 *   - ERASE_LOOP       → loop over 5 partitions
 *   - WRITE_DBE_SEND   → send db_write_enable before write
 *   - WRITE_DBE_RECV   → receive dbe response
 *   - WRITE_FLASH_SEND → write TLS flash (CMD 0x41)
 *   - WRITE_FLASH_RECV → receive write response
 *   - WRITE_CLEAN_SEND → call cleanups after write
 *   - WRITE_CLEAN_RECV → receive cleanups response
 *   - REBOOT_SEND      → reboot device (0x05 0x02 0x00)
 *   - REBOOT_RECV      → receive reboot response
 *   - DONE             → pairing complete
 * ================================================================ */

typedef enum {
  PAIR_GET_FLASH_INFO = 0,
  PAIR_GET_FLASH_INFO_RECV,
  PAIR_CHECK_NEEDED,
  PAIR_VERIFY_TLS_SEND,
  PAIR_VERIFY_TLS_RECV,
  PAIR_SEND_RESET_BLOB,
  PAIR_SEND_RESET_BLOB_RECV,
  PAIR_GENERATE_KEYS,
  PAIR_PARTITION_FLASH_SEND,
  PAIR_PARTITION_FLASH_RECV,
  PAIR_CMD50_SEND,
  PAIR_CMD50_RECV,
  PAIR_CMD50_PROCESS,
  PAIR_CLEANUPS_SEND,
  PAIR_CLEANUPS_RECV,
  PAIR_TLS_HANDSHAKE,
  PAIR_ERASE_DBE_SEND,
  PAIR_ERASE_DBE_RECV,
  PAIR_ERASE_SEND,
  PAIR_ERASE_RECV,
  PAIR_ERASE_CLEAN_SEND,
  PAIR_ERASE_CLEAN_RECV,
  PAIR_ERASE_LOOP,
  PAIR_WRITE_DBE_SEND,
  PAIR_WRITE_DBE_RECV,
  PAIR_WRITE_FLASH_SEND,
  PAIR_WRITE_FLASH_RECV,
  PAIR_WRITE_CLEAN_SEND,
  PAIR_WRITE_CLEAN_RECV,
  PAIR_REBOOT_SEND,
  PAIR_REBOOT_RECV,
  PAIR_DONE,
  PAIR_NUM_STATES,
} ValidityPairSsmState;

/* CMD 0x1a (cleanups/commit) */
#define VCSFW_CMD_CLEANUPS 0x1a

/* CMD 0x50 (get ECDH server params after partition_flash) */
#define VCSFW_CMD_GET_ECDH 0x50

/* Reboot: 0x05 0x02 0x00 */
#define VCSFW_CMD_REBOOT 0x05

/**
 * validity_pair_ssm_new:
 * @dev: the FpDevice
 *
 * Create a new pairing SSM. The caller must set up
 * self->pair_state.dev_desc before starting the SSM.
 *
 * Returns: a new FpiSsm.
 */
FpiSsm *validity_pair_ssm_new (FpDevice *dev);

/**
 * validity_pair_run_state:
 * @ssm: the pairing SSM
 * @dev: the FpDevice
 *
 * State machine runner for the pairing SSM.
 */
void validity_pair_run_state (FpiSsm   *ssm,
                              FpDevice *dev);

/**
 * validity_pair_build_tls_flash:
 * @state: pairing state (must have priv_blob, server_cert, ecdh_blob set)
 * @out_len: output data length (always 0x1000 = 4096)
 *
 * Build the TLS flash image for persistence. Format matches
 * python-validity make_tls_flash(): a sequence of flash blocks
 * zero-padded to 4096 bytes.
 *
 * Returns: newly allocated 4096-byte buffer. Free with g_free().
 */
guint8 *validity_pair_build_tls_flash (const ValidityPairState *state,
                                       const guint8            *ca_cert,
                                       gsize                    ca_cert_len,
                                       gsize                   *out_len);
