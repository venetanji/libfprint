/*
 * Goodix 27c6:5125 host pairing
 *
 * Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "goodix5125-pairing.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* The Goodix white-box format was independently reduced to standard
 * AES-256-GCM and HMAC-SHA256. The format and the synthetic test vector are
 * shared with the free gdix51c0 implementation; no vendor binary is loaded. */
static const guint8 wb_key_material[48] = {
  0x58, 0xf0, 0x13, 0xee, 0xb7, 0xe2, 0x16, 0xe2,
  0xc1, 0xcd, 0x0e, 0x8f, 0xff, 0xa7, 0xdb, 0xd6,
  0x79, 0x9c, 0xd9, 0x2a, 0xa1, 0x49, 0x01, 0x3e,
  0xa0, 0xe9, 0xf9, 0xfd, 0x7d, 0xc6, 0xd9, 0x4e,
  0x3e, 0xf6, 0x3a, 0x75, 0x98, 0xc2, 0xa4, 0x93,
  0x31, 0x29, 0xe8, 0x71, 0xea, 0x03, 0x04, 0x3b,
};

static const guint8 wb_aad[16] = {
  0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07,
  0xf4, 0x7f, 0x37, 0xa3, 0x2a, 0x84, 0x42, 0x7d,
};

#define WB_AES_KEY (wb_key_material)
#define WB_HMAC_KEY (wb_key_material + 16)

#define WB_FRONT_OFFSET 0x00
#define WB_HEADER_OFFSET 0x20
#define WB_LENGTH_OFFSET 0x22
#define WB_IV_OFFSET 0x26
#define WB_CIPHERTEXT_OFFSET 0x36
#define WB_TAG_OFFSET 0x56

typedef struct
{
  gsize  length;
  guint8 data[];
} SensitiveBuffer;

static void
sensitive_buffer_free (gpointer user_data)
{
  SensitiveBuffer *buffer = user_data;

  OPENSSL_cleanse (buffer->data, buffer->length);
  g_free (buffer);
}

static guint32
read_le32 (const guint8 *data)
{
  guint32 value;

  memcpy (&value, data, sizeof (value));
  return GUINT32_FROM_LE (value);
}

static void
write_le32 (guint8 *data,
            guint32 value)
{
  value = GUINT32_TO_LE (value);
  memcpy (data, &value, sizeof (value));
}

static gboolean
sha256 (const guint8 *data,
        gsize         length,
        guint8        output[32],
        GError      **error)
{
  unsigned int output_length = 0;

  if (EVP_Digest (data, length, output, &output_length, EVP_sha256 (), NULL) != 1 ||
      output_length != 32)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix PSK SHA-256 calculation failed");
      return FALSE;
    }
  return TRUE;
}

static gboolean
encrypt_gcm (const guint8 iv[16],
             const guint8 plaintext[GOODIX5125_PSK_SIZE],
             guint8       ciphertext[GOODIX5125_PSK_SIZE],
             guint8       tag[16],
             GError     **error)
{
  EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new ();
  int output_length = 0;
  int final_length = 0;
  gboolean success = FALSE;

  if (!context ||
      EVP_EncryptInit_ex (context, EVP_aes_256_gcm (), NULL, NULL, NULL) != 1 ||
      EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) != 1 ||
      EVP_EncryptInit_ex (context, NULL, NULL, WB_AES_KEY, iv) != 1 ||
      EVP_EncryptUpdate (context, NULL, &output_length,
                         wb_aad, sizeof (wb_aad)) != 1 ||
      EVP_EncryptUpdate (context, ciphertext, &output_length,
                         plaintext, GOODIX5125_PSK_SIZE) != 1 ||
      output_length != GOODIX5125_PSK_SIZE ||
      EVP_EncryptFinal_ex (context, ciphertext + output_length,
                           &final_length) != 1 ||
      final_length != 0 ||
      EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix PSK AES-GCM encryption failed");
      goto out;
    }
  success = TRUE;

out:
  EVP_CIPHER_CTX_free (context);
  return success;
}

gboolean
goodix5125_pairing_wrap (const guint8 psk[GOODIX5125_PSK_SIZE],
                         guint8       wb[GOODIX5125_WB_SIZE],
                         GError     **error)
{
  static const guint8 padding[4] = { 0x03, 0x00, 0x00, 0x00 };
  guint8 hash_input[2 + 4 + 8 + 16 * sizeof (padding)];
  guint8 digest[32];
  guint8 mac_input[2 + 4 + 32 + 16];
  unsigned int mac_length = 0;
  gsize offset = 0;
  gboolean success = FALSE;

  g_return_val_if_fail (psk != NULL, FALSE);
  g_return_val_if_fail (wb != NULL, FALSE);

  memset (wb, 0, GOODIX5125_WB_SIZE);
  wb[WB_HEADER_OFFSET] = 0x02;
  wb[WB_HEADER_OFFSET + 1] = 0xff;
  write_le32 (wb + WB_LENGTH_OFFSET, GOODIX5125_PSK_SIZE);

  memcpy (hash_input + offset, wb + WB_HEADER_OFFSET, 2);
  offset += 2;
  memcpy (hash_input + offset, wb + WB_LENGTH_OFFSET, 4);
  offset += 4;
  memcpy (hash_input + offset, psk, 8);
  offset += 8;
  for (guint i = 0; i < 16; i++)
    {
      memcpy (hash_input + offset, padding, sizeof (padding));
      offset += sizeof (padding);
    }
  if (!sha256 (hash_input, offset, digest, error))
    goto out;
  memcpy (wb + WB_IV_OFFSET, digest, 16);

  if (!encrypt_gcm (wb + WB_IV_OFFSET, psk,
                    wb + WB_CIPHERTEXT_OFFSET,
                    wb + WB_TAG_OFFSET, error))
    goto out;

  memcpy (mac_input, wb + WB_HEADER_OFFSET, 2 + 4);
  memcpy (mac_input + 6, wb + WB_CIPHERTEXT_OFFSET, 32 + 16);
  if (!HMAC (EVP_sha256 (), WB_HMAC_KEY, 32,
             mac_input, sizeof (mac_input),
             wb + WB_FRONT_OFFSET, &mac_length) ||
      mac_length != 32)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix PSK HMAC-SHA256 calculation failed");
      goto out;
    }

  success = TRUE;

out:
  OPENSSL_cleanse (hash_input, sizeof (hash_input));
  OPENSSL_cleanse (digest, sizeof (digest));
  OPENSSL_cleanse (mac_input, sizeof (mac_input));
  if (!success)
    OPENSSL_cleanse (wb, GOODIX5125_WB_SIZE);
  return success;
}

gboolean
goodix5125_pairing_hash (const guint8 wb[GOODIX5125_WB_SIZE],
                         guint8       hash[GOODIX5125_PSK_SIZE],
                         GError     **error)
{
  g_return_val_if_fail (wb != NULL, FALSE);
  g_return_val_if_fail (hash != NULL, FALSE);

  return sha256 (wb, GOODIX5125_WB_SIZE, hash, error);
}

GBytes *
goodix5125_pairing_build_write_payload (
  const guint8 psk[GOODIX5125_PSK_SIZE],
  GError     **error)
{
  const gsize raw_length = 8 + GOODIX5125_WB_SIZE;
  const gsize padded_length = (raw_length + 3) & ~((gsize) 3);
  guint8 wb[GOODIX5125_WB_SIZE];
  SensitiveBuffer *buffer;
  guint8 *payload;

  g_return_val_if_fail (psk != NULL, NULL);

  if (!goodix5125_pairing_wrap (psk, wb, error))
    return NULL;

  buffer = g_malloc0 (sizeof (*buffer) + padded_length);
  buffer->length = padded_length;
  payload = buffer->data;
  write_le32 (payload, GOODIX5125_PSK_WB_ADDRESS);
  write_le32 (payload + 4, GOODIX5125_WB_SIZE);
  memcpy (payload + 8, wb, sizeof (wb));
  OPENSSL_cleanse (wb, sizeof (wb));
  return g_bytes_new_with_free_func (payload, padded_length,
                                     sensitive_buffer_free, buffer);
}

gboolean
goodix5125_pairing_self_test (GError **error)
{
  static const guint8 psk[GOODIX5125_PSK_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  };
  static const guint8 expected[GOODIX5125_WB_SIZE] = {
    0x45, 0xa3, 0x73, 0x2a, 0xcf, 0x29, 0x90, 0xf4,
    0xce, 0xb3, 0x0e, 0x89, 0x96, 0xe4, 0xad, 0xd1,
    0x2e, 0x88, 0x71, 0x60, 0xd6, 0xdc, 0xaa, 0x99,
    0xc8, 0xbc, 0xd8, 0x33, 0x8c, 0x77, 0x43, 0xf2,
    0x02, 0xff, 0x20, 0x00, 0x00, 0x00, 0xe7, 0xbc,
    0x09, 0x96, 0x2a, 0xed, 0xfb, 0x47, 0x0f, 0xac,
    0x38, 0x87, 0xde, 0x32, 0x46, 0x5c, 0x5a, 0x71,
    0xc8, 0x8f, 0xe0, 0x1d, 0xd7, 0x76, 0xcf, 0xa5,
    0x19, 0xe7, 0x0c, 0x63, 0xf8, 0x24, 0x01, 0xf8,
    0x24, 0x73, 0x24, 0xfd, 0xe9, 0xc6, 0x56, 0x23,
    0x21, 0xd4, 0xd8, 0x2b, 0x0d, 0xd5, 0x52, 0xc0,
    0xbf, 0x1f, 0x74, 0x71, 0x60, 0x85, 0xb8, 0x52,
    0x65, 0x8b, 0xfe, 0x77, 0x29, 0x64,
  };
  guint8 actual[GOODIX5125_WB_SIZE];

  if (!goodix5125_pairing_wrap (psk, actual, error))
    return FALSE;
  if (CRYPTO_memcmp (actual, expected, sizeof (actual)) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix PSK white-box self-test failed");
      return FALSE;
    }
  return TRUE;
}

gboolean
goodix5125_pairing_parse_hash_reply (
  const guint8 *reply,
  gsize         reply_length,
  gboolean     *present,
  guint8        hash[GOODIX5125_PSK_SIZE],
  GError      **error)
{
  guint32 address;
  guint32 data_length;

  g_return_val_if_fail (present != NULL, FALSE);
  g_return_val_if_fail (hash != NULL, FALSE);

  *present = FALSE;
  memset (hash, 0, GOODIX5125_PSK_SIZE);
  if (!reply || reply_length < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix PSK-hash reply is empty");
      return FALSE;
    }
  if (reply[0] != 0)
    return TRUE;
  if (reply_length < 9)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix PSK-hash reply is shorter than 9 bytes");
      return FALSE;
    }

  address = read_le32 (reply + 1);
  data_length = read_le32 (reply + 5);
  if (address != GOODIX5125_PSK_HASH_ADDRESS ||
      data_length != GOODIX5125_PSK_SIZE ||
      reply_length < 9 + data_length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Goodix PSK-hash reply has address 0x%08x and length %u",
                   address, data_length);
      return FALSE;
    }

  memcpy (hash, reply + 9, GOODIX5125_PSK_SIZE);
  *present = TRUE;
  return TRUE;
}

static gboolean
parse_psk_hex (const gchar *contents,
               gsize        length,
               guint8       psk[GOODIX5125_PSK_SIZE])
{
  gsize start = 0;
  gsize end = length;

  while (start < end && g_ascii_isspace (contents[start]))
    start++;
  while (end > start && g_ascii_isspace (contents[end - 1]))
    end--;
  if (end - start != GOODIX5125_PSK_SIZE * 2)
    return FALSE;

  for (guint i = 0; i < GOODIX5125_PSK_SIZE; i++)
    {
      int high = g_ascii_xdigit_value (contents[start + i * 2]);
      int low = g_ascii_xdigit_value (contents[start + i * 2 + 1]);

      if (high < 0 || low < 0)
        return FALSE;
      psk[i] = (high << 4) | low;
    }
  return TRUE;
}

Goodix5125PskState
goodix5125_pairing_load_psk (const gchar *path,
                             guint8       psk[GOODIX5125_PSK_SIZE],
                             GError     **error)
{
  struct stat stat_buffer;
  g_autofree gchar *contents = NULL;
  int fd;
  gsize length;
  gsize offset = 0;
  gboolean valid = FALSE;

  g_return_val_if_fail (path != NULL, GOODIX5125_PSK_STATE_ERROR);
  g_return_val_if_fail (psk != NULL, GOODIX5125_PSK_STATE_ERROR);

  fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
  if (fd < 0)
    {
      if (errno == ENOENT)
        return GOODIX5125_PSK_STATE_MISSING;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot open Goodix PSK state %s: %s",
                   path, g_strerror (errno));
      return GOODIX5125_PSK_STATE_ERROR;
    }
  if (fstat (fd, &stat_buffer) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot inspect Goodix PSK state %s: %s",
                   path, g_strerror (errno));
      close (fd);
      return GOODIX5125_PSK_STATE_ERROR;
    }
  if (!S_ISREG (stat_buffer.st_mode) || stat_buffer.st_size < 0 ||
      stat_buffer.st_size > 256 ||
      stat_buffer.st_uid != geteuid () ||
      (stat_buffer.st_mode & 0077) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   "Goodix PSK state %s must be a private regular file owned "
                   "by the service user", path);
      close (fd);
      return GOODIX5125_PSK_STATE_ERROR;
    }
  length = stat_buffer.st_size;
  contents = g_malloc (length + 1);
  while (offset < length)
    {
      ssize_t count = read (fd, contents + offset, length - offset);

      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          g_set_error (error, G_IO_ERROR,
                       count < 0 ? g_io_error_from_errno (errno) :
                       G_IO_ERROR_INVALID_DATA,
                       "Cannot read Goodix PSK state %s: %s", path,
                       count < 0 ? g_strerror (errno) : "unexpected EOF");
          close (fd);
          OPENSSL_cleanse (contents, length);
          return GOODIX5125_PSK_STATE_ERROR;
        }
      offset += count;
    }
  close (fd);
  contents[length] = '\0';

  if (length == GOODIX5125_PSK_SIZE)
    {
      memcpy (psk, contents, GOODIX5125_PSK_SIZE);
      valid = TRUE;
    }
  else
    {
      valid = parse_psk_hex (contents, length, psk);
    }

  OPENSSL_cleanse (contents, length);
  if (!valid)
    {
      OPENSSL_cleanse (psk, GOODIX5125_PSK_SIZE);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s does not contain a 32-byte or 64-hex-digit PSK", path);
      return GOODIX5125_PSK_STATE_ERROR;
    }
  return GOODIX5125_PSK_STATE_FOUND;
}

static gboolean
write_all (int           fd,
           const guint8 *data,
           gsize         length,
           GError      **error)
{
  while (length > 0)
    {
      ssize_t written = write (fd, data, length);

      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Could not write Goodix PSK state: %s",
                       g_strerror (errno));
          return FALSE;
        }
      data += written;
      length -= written;
    }
  return TRUE;
}

gboolean
goodix5125_pairing_save_psk (const gchar *path,
                             const guint8 psk[GOODIX5125_PSK_SIZE],
                             GError     **error)
{
  g_autofree gchar *directory = NULL;
  g_autofree gchar *temporary = NULL;
  int fd = -1;
  int directory_fd = -1;
  gboolean success = FALSE;

  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (psk != NULL, FALSE);

  directory = g_path_get_dirname (path);
  if (g_mkdir_with_parents (directory, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot create Goodix state directory %s: %s",
                   directory, g_strerror (errno));
      return FALSE;
    }

  temporary = g_build_filename (directory, ".psk-XXXXXX", NULL);
  fd = g_mkstemp_full (temporary, O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot create temporary Goodix PSK state in %s: %s",
                   directory, g_strerror (errno));
      return FALSE;
    }

  if (!write_all (fd, psk, GOODIX5125_PSK_SIZE, error) ||
      fsync (fd) != 0)
    {
      if (error && *error == NULL)
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot sync temporary Goodix PSK state: %s",
                     g_strerror (errno));
      goto out;
    }
  if (close (fd) != 0)
    {
      fd = -1;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot close temporary Goodix PSK state: %s",
                   g_strerror (errno));
      goto out;
    }
  fd = -1;
  if (g_rename (temporary, path) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot install Goodix PSK state %s: %s",
                   path, g_strerror (errno));
      goto out;
    }

  directory_fd = g_open (directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY, 0);
  if (directory_fd < 0 || fsync (directory_fd) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot sync Goodix state directory %s: %s",
                   directory, g_strerror (errno));
      goto out;
    }

  success = TRUE;

out:
  if (fd >= 0)
    close (fd);
  if (directory_fd >= 0)
    close (directory_fd);
  if (!success)
    g_unlink (temporary);
  return success;
}
