/*
 * Copyright (C) 2022-2023 Realtek Corp.
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

#define FP_COMPONENT "realtek"

#include "drivers_api.h"

#include "fpi-byte-reader.h"

#include "realtek.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

G_DEFINE_TYPE (FpiDeviceRealtek, fpi_device_realtek, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x0bda,  .pid = 0x5813,  },
  { .vid = 0x0bda,  .pid = 0x5816,  },
  { .vid = 0x2541,  .pid = 0xfa03,  },
  { .vid = 0x3274,  .pid = 0x9003,  },
  { .vid = 0x2df0,  .pid = 0x0007,  .driver_data = RTK_DEVICE_SDCP },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },   /* terminating entry */
};

static gboolean
parse_print_data (GVariant      *data,
                  guint8        *finger,
                  const guint8 **user_id,
                  gsize         *user_id_len)
{
  g_autoptr(GVariant) user_id_var = NULL;

  g_return_val_if_fail (data, FALSE);
  g_return_val_if_fail (finger, FALSE);
  g_return_val_if_fail (user_id, FALSE);
  g_return_val_if_fail (user_id_len, FALSE);

  *user_id = NULL;
  *user_id_len = 0;
  *finger = 0;

  if (!g_variant_check_format_string (data, "(y@ay)", FALSE))
    return FALSE;

  g_variant_get (data,
                 "(y@ay)",
                 finger,
                 &user_id_var);

  *user_id = g_variant_get_fixed_array (user_id_var, user_id_len, 1);

  if (*user_id_len == 0 || *user_id_len > DEFAULT_UID_LEN)
    return FALSE;

  if (*user_id[0] == '\0' || *user_id[0] == ' ')
    return FALSE;

  if (*finger != SUB_FINGER_01)
    return FALSE;

  return TRUE;
}

/* SDCP host side.
 *
 * Connect:   host -> 85 01  [20 00][r_h 32][41 00][pk_h 65]
 *            dev  -> 45 02  [20 00][r_d 32][len16][claim][20 00][mac 32]
 *            claim = cert_m (DER) | pk_d 65 | pk_f 65 | h_f 32 | s_m 64 | s_d 64
 *   a  = ECDH (sk_h, pk_f)
 *   ms = KDF (a, "master secret", r_h || r_d, 256)
 *   s  = KDF (ms, "application keys", -, 512), first 256 bits
 *   mac == HMAC (s, "connect\0" || SHA256 (claim))
 * Identify:  85 0b nonce, 45 0c -> status | finger | id 32 | HMAC (s, "identify\0" || nonce || id)
 * Enroll:    45 09 <slot> -> nonce, ..., 85 0a <0xf5 + slot> HMAC (s, "enroll\0" || nonce)
 *
 * KDF is SP800-108 counter mode with HMAC-SHA256. The model certificate and
 * the s_m / s_d signatures are not validated.
 */

static gboolean
sdcp_hmac (const guint8 *key,
           const char   *label,
           const guint8 *a,
           gsize         a_len,
           const guint8 *b,
           gsize         b_len,
           guint8        out[SDCP_MAC_LEN])
{
  g_autoptr(GByteArray) msg = g_byte_array_new ();
  size_t out_len = 0;

  /* SDCP MAC labels include their NUL terminator */
  g_byte_array_append (msg, (const guint8 *) label, strlen (label) + 1);
  if (a_len)
    g_byte_array_append (msg, a, a_len);
  if (b_len)
    g_byte_array_append (msg, b, b_len);

  return EVP_Q_mac (NULL, "HMAC", NULL, "SHA256", NULL, key, SDCP_MAC_LEN,
                    msg->data, msg->len, out, SDCP_MAC_LEN, &out_len) != NULL &&
         out_len == SDCP_MAC_LEN;
}

static gboolean
sdcp_kdf (const guint8 *key,
          const char   *label,
          const guint8 *context,
          gsize         context_len,
          guint32       bits,
          guint8        out[SDCP_MAC_LEN])
{
  g_autoptr(GByteArray) msg = g_byte_array_new ();
  const guint8 counter[4] = { 0, 0, 0, 1 };
  guint32 bits_be = GUINT32_TO_BE (bits);
  size_t out_len = 0;

  g_byte_array_append (msg, counter, sizeof (counter));
  g_byte_array_append (msg, (const guint8 *) label, strlen (label) + 1);
  if (context_len)
    g_byte_array_append (msg, context, context_len);
  g_byte_array_append (msg, (const guint8 *) &bits_be, sizeof (bits_be));

  return EVP_Q_mac (NULL, "HMAC", NULL, "SHA256", NULL, key, SDCP_MAC_LEN,
                    msg->data, msg->len, out, SDCP_MAC_LEN, &out_len) != NULL &&
         out_len == SDCP_MAC_LEN;
}

static gboolean
sdcp_ecdh (EVP_PKEY     *host_key,
           const guint8 *peer_point,
           guint8        out[SDCP_MAC_LEN])
{
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new ();
  OSSL_PARAM *params = NULL;
  EVP_PKEY_CTX *ctx = NULL;
  EVP_PKEY *peer = NULL;
  size_t out_len = SDCP_MAC_LEN;
  gboolean ok = FALSE;

  if (!bld ||
      !OSSL_PARAM_BLD_push_utf8_string (bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_octet_string (bld, OSSL_PKEY_PARAM_PUB_KEY, peer_point, SDCP_PUBKEY_LEN) ||
      !(params = OSSL_PARAM_BLD_to_param (bld)))
    goto out;

  ctx = EVP_PKEY_CTX_new_from_name (NULL, "EC", NULL);
  if (!ctx || EVP_PKEY_fromdata_init (ctx) <= 0 ||
      EVP_PKEY_fromdata (ctx, &peer, EVP_PKEY_PUBLIC_KEY, params) <= 0)
    goto out;
  EVP_PKEY_CTX_free (ctx);

  ctx = EVP_PKEY_CTX_new (host_key, NULL);
  ok = ctx &&
       EVP_PKEY_derive_init (ctx) > 0 &&
       EVP_PKEY_derive_set_peer (ctx, peer) > 0 &&
       EVP_PKEY_derive (ctx, out, &out_len) > 0 &&
       out_len == SDCP_MAC_LEN;

out:
  EVP_PKEY_CTX_free (ctx);
  EVP_PKEY_free (peer);
  OSSL_PARAM_free (params);
  OSSL_PARAM_BLD_free (bld);
  return ok;
}

static gboolean
sdcp_random (guint8 *buf, gsize len)
{
  return RAND_bytes (buf, len) == 1;
}

static void
sdcp_clear_host_key (FpiDeviceRealtek *self)
{
  g_clear_pointer (&self->sdcp_host_key, EVP_PKEY_free);
}

/* Builds the 85 01 payload and keeps the ephemeral key for the response. */
static guint8 *
sdcp_connect_payload (FpiDeviceRealtek *self, GError **error)
{
  g_autofree guint8 *payload = g_malloc0 (SDCP_CONNECT_LEN);
  EVP_PKEY *key;
  size_t pub_len = 0;

  sdcp_clear_host_key (self);
  key = EVP_PKEY_Q_keygen (NULL, NULL, "EC", "P-256");
  if (!key ||
      !EVP_PKEY_get_octet_string_param (key, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY,
                                        payload + 2 + SDCP_RANDOM_LEN + 2,
                                        SDCP_PUBKEY_LEN, &pub_len) ||
      pub_len != SDCP_PUBKEY_LEN)
    {
      EVP_PKEY_free (key);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "SDCP: host key generation failed");
      return NULL;
    }
  self->sdcp_host_key = key;

  if (!sdcp_random (self->sdcp_host_random, SDCP_RANDOM_LEN))
    {
      sdcp_clear_host_key (self);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "SDCP: random generation failed");
      return NULL;
    }

  payload[0] = SDCP_RANDOM_LEN;
  memcpy (payload + 2, self->sdcp_host_random, SDCP_RANDOM_LEN);
  payload[2 + SDCP_RANDOM_LEN] = SDCP_PUBKEY_LEN;

  return g_steal_pointer (&payload);
}

static gboolean
sdcp_process_connect_response (FpiDeviceRealtek *self,
                               const guint8     *resp,
                               GError          **error)
{
  FpiByteReader reader;
  const guint8 *r_d, *claim, *mac, *pk_f;
  guint16 r_d_len, claim_len, mac_len, cert_len;
  guint8 a[SDCP_MAC_LEN], ms[SDCP_MAC_LEN], s[SDCP_MAC_LEN];
  guint8 claim_hash[SDCP_MAC_LEN], expected[SDCP_MAC_LEN], rr[2 * SDCP_RANDOM_LEN];
  size_t hash_len = 0;

  fpi_byte_reader_init (&reader, resp, SDCP_CONNECT_RESP_LEN);
  if (!fpi_byte_reader_get_uint16_le (&reader, &r_d_len) || r_d_len != SDCP_RANDOM_LEN ||
      !fpi_byte_reader_get_data (&reader, r_d_len, &r_d) ||
      !fpi_byte_reader_get_uint16_le (&reader, &claim_len) ||
      !fpi_byte_reader_get_data (&reader, claim_len, &claim) ||
      !fpi_byte_reader_get_uint16_le (&reader, &mac_len) || mac_len != SDCP_MAC_LEN ||
      !fpi_byte_reader_get_data (&reader, mac_len, &mac))
    goto malformed;

  /* claim starts with the DER model certificate (30 82 <len16 BE>) */
  if (claim_len < 4 || claim[0] != 0x30 || claim[1] != 0x82)
    goto malformed;
  cert_len = 4 + ((claim[2] << 8) | claim[3]);
  if (claim_len < cert_len + 2 * SDCP_PUBKEY_LEN)
    goto malformed;
  pk_f = claim + cert_len + SDCP_PUBKEY_LEN;

  memcpy (rr, self->sdcp_host_random, SDCP_RANDOM_LEN);
  memcpy (rr + SDCP_RANDOM_LEN, r_d, SDCP_RANDOM_LEN);

  if (!self->sdcp_host_key ||
      !sdcp_ecdh (self->sdcp_host_key, pk_f, a) ||
      !sdcp_kdf (a, "master secret", rr, sizeof (rr), 256, ms) ||
      !sdcp_kdf (ms, "application keys", NULL, 0, 512, s) ||
      !EVP_Q_digest (NULL, "SHA256", NULL, claim, claim_len, claim_hash, &hash_len) ||
      !sdcp_hmac (s, "connect", claim_hash, sizeof (claim_hash), NULL, 0, expected))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "SDCP: key derivation failed");
      return FALSE;
    }

  sdcp_clear_host_key (self);

  if (CRYPTO_memcmp (expected, mac, SDCP_MAC_LEN) != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "SDCP: connect MAC mismatch");
      return FALSE;
    }

  memcpy (self->sdcp_mac_key, s, SDCP_MAC_LEN);
  self->sdcp_connected = TRUE;
  fp_dbg ("SDCP session established");
  return TRUE;

malformed:
  sdcp_clear_host_key (self);
  g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
               "SDCP: malformed connect response");
  return FALSE;
}

/* SDCP prints are stored as (finger byte, 32 byte enrollment id) */
static FpPrint *
sdcp_print_new (FpiDeviceRealtek *self, FpPrint *print, guint8 finger, const guint8 *id)
{
  g_autofree gchar *description = NULL;
  GVariant *id_var;
  GVariant *data;

  if (!print)
    print = fp_print_new (FP_DEVICE (self));

  id_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, id, SDCP_ID_LEN, 1);
  data = g_variant_new ("(y@ay)", finger, id_var);

  description = g_strdup_printf ("SDCP %02x %02x%02x%02x%02x%02x%02x%02x%02x",
                                 finger, id[0], id[1], id[2], id[3],
                                 id[4], id[5], id[6], id[7]);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
  if (!fp_print_get_description (print))
    g_object_set (print, "description", description, NULL);

  return print;
}

static gboolean
sdcp_parse_print (FpPrint *print, guint8 *finger, guint8 id[SDCP_ID_LEN])
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) id_var = NULL;
  const guint8 *id_data;
  gsize id_len = 0;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_check_format_string (data, "(y@ay)", FALSE))
    return FALSE;

  g_variant_get (data, "(y@ay)", finger, &id_var);
  id_data = g_variant_get_fixed_array (id_var, &id_len, 1);
  if (id_len != SDCP_ID_LEN || *finger < SDCP_FINGER_BASE)
    return FALSE;

  memcpy (id, id_data, SDCP_ID_LEN);
  return TRUE;
}

static void
fp_cmd_ssm_done_data_free (CommandData *data)
{
  g_free (data);
}

/* data callbacks */

static void
fp_task_ssm_generic_cb (FpiDeviceRealtek *self,
                        uint8_t          *buffer_in,
                        GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_get_device_info_cb (FpiDeviceRealtek *self,
                       uint8_t          *buffer_in,
                       GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  self->template_len = TEMPLATE_LEN_COMMON;

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_update_template_cb (FpiDeviceRealtek *self,
                       uint8_t          *buffer_in,
                       GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
}

static void
fp_enroll_commit_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_NUM_STATES);
}

static void
fp_finish_capture_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  /* We hope this polling CMD can be completed before the action is cancelled */
  GCancellable *cancellable = fpi_device_get_cancellable (FP_DEVICE (self));
  if (g_cancellable_is_cancelled (cancellable))
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Action is cancelled!"));
      return;
    }

  gint capture_status = buffer_in[0];
  if (capture_status == 0)
    {
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NEEDED);
      fpi_ssm_next_state (self->task_ssm);
    }
  else
    {
      fpi_ssm_jump_to_state (self->task_ssm,
                             fpi_ssm_get_cur_state (self->task_ssm));
    }
}

static void
fp_accept_sample_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                           FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_PRESENT);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];

  if (self->fp_purpose != FP_RTK_PURPOSE_ENROLL)
    {
      /* verify or identify purpose process */
      fpi_ssm_next_state (self->task_ssm);
      return;
    }
  else
    {
      /* enroll purpose process */
      if (in_status == FP_RTK_CMD_ERR)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Command error!"));
          return;
        }

      if (self->enroll_stage < self->max_enroll_stage)
        {
          if (in_status == FP_RTK_SUCCESS)
            {
              self->enroll_stage++;
              fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);

              /* SDCP sensors merge on-chip; like the Windows driver, stop at
               * exactly max_enroll_stage good samples and go to the duplicate check */
              if (self->sdcp && self->enroll_stage >= self->max_enroll_stage)
                fpi_ssm_next_state (self->task_ssm);
              else
                fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_CAPTURE);
            }
          else if (in_status == FP_RTK_MATCH_FAIL)
            {
              fpi_ssm_mark_failed (self->task_ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                             "InStatus invalid!"));
            }
          else
            {
              fpi_device_enroll_progress (FP_DEVICE (self),
                                          self->enroll_stage,
                                          NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));

              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_CAPTURE);
            }
          return;
        }

      if (in_status != FP_RTK_SUCCESS)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "Enrollment failed (status 0x%02x)",
                                                         in_status));
          return;
        }

      fpi_ssm_next_state (self->task_ssm);
    }
}

static FpPrint *
fp_print_from_data (FpiDeviceRealtek *self, uint8_t *buffer)
{
  FpPrint *print;
  GVariant *data;
  GVariant *uid;
  guint finger;
  gsize userid_len;
  g_autofree gchar *userid = NULL;

  userid = g_strndup ((gchar *) buffer + 1, DEFAULT_UID_LEN);
  finger = *(buffer);

  print = fp_print_new (FP_DEVICE (self));
  userid_len = MIN (DEFAULT_UID_LEN, strlen (userid));

  uid = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                   userid,
                                   userid_len,
                                   1);

  data = g_variant_new ("(y@ay)",
                        finger,
                        uid);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
  g_object_set (print, "description", userid, NULL);
  fpi_print_fill_from_user_id (print, userid);

  return print;
}

static void
fp_identify_feature_cb (FpiDeviceRealtek *self,
                        uint8_t          *buffer_in,
                        GError           *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpPrint *match = NULL;
  FpPrint *print = NULL;
  FpiDeviceAction current_action;

  g_autoptr(GPtrArray) templates = NULL;
  gboolean found = FALSE;

  current_action = fpi_device_get_current_action (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];
  if (in_status == FP_RTK_CMD_ERR)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Command error!"));
      return;
    }

  if (in_status >= FP_RTK_TOO_HIGH && in_status <= FP_RTK_MERGE_FAILURE)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }

  if (in_status == FP_RTK_SUCCESS)
    {
      match = fp_print_from_data (self, buffer_in + 1);

      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        {
          templates = g_ptr_array_sized_new (1);
          fpi_device_get_verify_data (device, &print);
          g_ptr_array_add (templates, print);
        }
      else
        {
          fpi_device_get_identify_data (device, &templates);
          g_ptr_array_ref (templates);
        }

      for (gint cnt = 0; cnt < templates->len; cnt++)
        {
          print = g_ptr_array_index (templates, cnt);

          if (fp_print_equal (print, match))
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        {
          if (current_action == FPI_DEVICE_ACTION_VERIFY)
            {
              fpi_device_verify_report (device, FPI_MATCH_SUCCESS, match, error);
              fpi_ssm_next_state (self->task_ssm);
            }
          else
            {
              fpi_device_identify_report (device, print, match, error);
              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
            }
          return;
        }
    }

  if (!found)
    {
      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, error);
      else
        fpi_device_identify_report (device, NULL, NULL, error);

      fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
    }
}

/* Reports a match (or none) for verify/identify. Takes ownership of match. */
static void
fp_report_match (FpiDeviceRealtek *self, FpPrint *match)
{
  FpDevice *device = FP_DEVICE (self);
  FpiDeviceAction current_action = fpi_device_get_current_action (device);
  FpPrint *print = NULL;

  g_autoptr(GPtrArray) templates = NULL;

  if (match)
    {
      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        {
          templates = g_ptr_array_sized_new (1);
          fpi_device_get_verify_data (device, &print);
          g_ptr_array_add (templates, print);
        }
      else
        {
          fpi_device_get_identify_data (device, &templates);
          g_ptr_array_ref (templates);
        }

      for (guint i = 0; i < templates->len; i++)
        {
          print = g_ptr_array_index (templates, i);
          if (fp_print_equal (print, match))
            {
              if (current_action == FPI_DEVICE_ACTION_VERIFY)
                fpi_device_verify_report (device, FPI_MATCH_SUCCESS, match, NULL);
              else
                fpi_device_identify_report (device, print, match, NULL);

              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
              return;
            }
        }

      /* matched a print that is not ours (e.g. enrolled by Windows) */
      fp_dbg ("SDCP: matched print is not in the gallery");
      g_object_unref (g_object_ref_sink (match));
    }

  if (current_action == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);
  else
    fpi_device_identify_report (device, NULL, NULL, NULL);

  fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
}

static void
fp_sdcp_identify_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  guint8 expected[SDCP_MAC_LEN];
  const guint8 *id, *mac;
  gint in_status;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  in_status = buffer_in[0];
  fp_dbg ("SDCP identify status 0x%02x", in_status);

  if (in_status == FP_RTK_CMD_ERR)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Command error!"));
      return;
    }

  if (in_status >= FP_RTK_TOO_HIGH && in_status <= FP_RTK_MERGE_FAILURE)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }

  if (in_status != FP_RTK_SUCCESS)
    {
      fp_report_match (self, NULL);
      return;
    }

  /* status | finger | id 32 | mac 32 | quality words */
  id = buffer_in + 2;
  mac = id + SDCP_ID_LEN;
  if (!sdcp_hmac (self->sdcp_mac_key, "identify", self->sdcp_nonce, SDCP_RANDOM_LEN,
                  id, SDCP_ID_LEN, expected) ||
      CRYPTO_memcmp (expected, mac, SDCP_MAC_LEN) != 0)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "SDCP: identify MAC mismatch"));
      return;
    }

  fp_report_match (self, sdcp_print_new (self, NULL, buffer_in[1], id));
}

static void
fp_sdcp_enroll_begin_cb (FpiDeviceRealtek *self,
                         uint8_t          *buffer_in,
                         GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  memcpy (self->sdcp_enroll_nonce, buffer_in, SDCP_RANDOM_LEN);
  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_sdcp_connect_response_cb (FpiDeviceRealtek *self,
                             uint8_t          *buffer_in,
                             GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (!sdcp_process_connect_response (self, buffer_in, &error))
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_get_delete_pos_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  FpPrint *print = NULL;

  g_autoptr(GVariant) data = NULL;
  gsize user_id_len = 0;
  const guint8 *user_id;
  guint8 finger;
  gboolean found = FALSE;
  gchar temp_userid[DEFAULT_UID_LEN + 1] = {0};

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_device_get_delete_data (FP_DEVICE (self), &print);

  if (self->sdcp)
    {
      guint8 sdcp_id[SDCP_ID_LEN];

      if (!sdcp_parse_print (print, &finger, sdcp_id))
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      for (gint i = 0; i < self->template_num; i++)
        {
          const guint8 *slot = buffer_in + i * self->template_len;

          if (slot[0] != 0 && slot[SUBFACTOR_OFFSET] == finger &&
              memcmp (slot + UID_OFFSET, sdcp_id, SDCP_ID_LEN) == 0)
            {
              self->pos_index = i;
              fpi_ssm_next_state (self->task_ssm);
              return;
            }
        }

      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }

  g_object_get (print, "fpi-data", &data, NULL);

  if (!parse_print_data (data, &finger, &user_id, &user_id_len))
    {
      fpi_device_delete_complete (FP_DEVICE (self),
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] != 0)
        {
          memcpy (temp_userid, buffer_in + i * self->template_len + UID_OFFSET, DEFAULT_UID_LEN);
          if (g_strcmp0 (fp_print_get_description (print), (const char *) temp_userid) == 0)
            {
              self->pos_index = i;
              found = TRUE;
              break;
            }
        }
    }

  if (!found)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Get template position failed!"));
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_get_enroll_num_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  self->template_num = buffer_in[1];

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_verify_get_template_cb (FpiDeviceRealtek *self,
                           uint8_t          *buffer_in,
                           GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_enroll_get_template_cb (FpiDeviceRealtek *self,
                           uint8_t          *buffer_in,
                           GError           *error)
{
  g_autofree guchar *seq_list = NULL;
  gboolean found = FALSE;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] == 0)
        {
          self->pos_index = i;
          found = TRUE;
          break;
        }
    }

  if (!found)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "No free template was found!"));
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_check_duplicate_cb (FpiDeviceRealtek *self,
                       uint8_t          *buffer_in,
                       GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];
  if (in_status == FP_RTK_CMD_ERR)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Command error!"));
      return;
    }

  if (in_status == FP_RTK_SUCCESS)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                                     "Current fingerprint is duplicate!"));
    }
  else if (in_status == FP_RTK_MATCH_FAIL)
    {
      fpi_ssm_next_state (self->task_ssm);
    }
  else
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                     "InStatus invalid!"));
    }
}

static void
fp_list_cb (FpiDeviceRealtek *self,
            uint8_t          *buffer_in,
            GError           *error)
{
  gboolean found = FALSE;

  g_autoptr(GPtrArray) list_result = NULL;

  if (error)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL, error);
      return;
    }

  list_result = g_ptr_array_new_with_free_func (g_object_unref);

  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] != 0)
        {
          FpPrint *print = NULL;

          if (self->sdcp)
            {
              const guint8 *slot = buffer_in + i * self->template_len;

              print = sdcp_print_new (self, NULL, slot[SUBFACTOR_OFFSET], slot + UID_OFFSET);
              g_ptr_array_add (list_result, g_object_ref_sink (print));
              found = TRUE;
              continue;
            }

          print = fp_print_from_data (self, buffer_in + i * self->template_len + SUBFACTOR_OFFSET);
          g_ptr_array_add (list_result, g_object_ref_sink (print));
          found = TRUE;
        }
    }

  if (!found)
    {
      fpi_device_list_complete (FP_DEVICE (self),
                                g_steal_pointer (&list_result),
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                          "Database is empty"));
      return;
    }

  fp_info ("Query templates complete!");
  fpi_device_list_complete (FP_DEVICE (self),
                            g_steal_pointer (&list_result),
                            NULL);
}

static void
fp_clear_storage_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  FpDevice *device = FP_DEVICE (self);

  if (error)
    {
      fpi_device_clear_storage_complete (device, error);
      return;
    }

  fp_info ("Successfully cleared storage");
  fpi_device_clear_storage_complete (device, NULL);
}


static gint
parse_status (guint8 *buffer, gint status_type)
{
  switch (status_type)
    {
    case FP_RTK_MSG_NO_STATUS:
      return 0;
      break;

    case FP_RTK_MSG_DEFAULT:
      return buffer[0];
      break;

    default:
      return 1;
      break;
    }
}

static void
fp_cmd_receive_cb (FpiUsbTransfer *transfer,
                   FpDevice       *device,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  CommandData *data = user_data;
  gint ssm_state = 0;
  gint status_flag = 1;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (data == NULL)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  ssm_state = fpi_ssm_get_cur_state (transfer->ssm);

  /* skip zero length package */
  if (transfer->actual_length == 0)
    {
      fpi_ssm_jump_to_state (transfer->ssm, ssm_state);
      return;
    }

  /* get data */
  if (ssm_state == FP_RTK_CMD_TRANS_DATA)
    {
      g_autofree guchar *read_buf = NULL;

      if ((gsize) transfer->actual_length < self->trans_data_len)
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Truncated data received"));
          return;
        }

      read_buf = g_malloc0 (sizeof (guchar) * (self->trans_data_len));
      memcpy (read_buf, transfer->buffer, self->trans_data_len);
      self->read_data = g_steal_pointer (&read_buf);

      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  /* get status */
  status_flag = parse_status (transfer->buffer, self->message_type);
  if (status_flag != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Status check failed"));
      return;
    }

  if (data->callback)
    data->callback (self, self->read_data, NULL);

  if (self->read_data)
    g_clear_pointer (&self->read_data, g_free);

  fpi_ssm_mark_completed (transfer->ssm);
}

static void
fp_cmd_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_CMD_SEND:
      if (self->cmd_transfer)
        {
          self->cmd_transfer->ssm = ssm;
          fpi_usb_transfer_submit (g_steal_pointer (&self->cmd_transfer),
                                   CMD_TIMEOUT,
                                   NULL,
                                   fpi_ssm_usb_transfer_cb,
                                   NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case FP_RTK_CMD_TRANS_DATA:
      if (self->cmd_type == FP_RTK_CMD_BULK_ONLY)
        {
          fpi_ssm_jump_to_state (ssm, FP_RTK_CMD_GET_STATUS);
          break;
        }

      if (self->cmd_type == FP_RTK_CMD_BULK_WRITE)
        {
          if (self->data_transfer)
            {
              self->data_transfer->ssm = ssm;
              fpi_usb_transfer_submit (g_steal_pointer (&self->data_transfer),
                                       DATA_TIMEOUT,
                                       NULL,
                                       fpi_ssm_usb_transfer_cb,
                                       NULL);
            }
          else
            {
              fpi_ssm_next_state (ssm);
            }
        }
      else  /* CMD_READ */
        {
          transfer = fpi_usb_transfer_new (dev);
          transfer->ssm = ssm;
          fpi_usb_transfer_fill_bulk (transfer, EP_IN, EP_IN_MAX_BUF_SIZE);

          fpi_usb_transfer_submit (transfer,
                                   self->cmd_cancellable ? 0 : DATA_TIMEOUT,
                                   self->cmd_cancellable ? fpi_device_get_cancellable (dev) : NULL,
                                   fp_cmd_receive_cb,
                                   fpi_ssm_get_data (ssm));
        }
      break;

    case FP_RTK_CMD_GET_STATUS:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
      fpi_usb_transfer_submit (transfer,
                               STATUS_TIMEOUT,
                               NULL,
                               fp_cmd_receive_cb,
                               fpi_ssm_get_data (ssm));
      break;
    }
}

static void
fp_cmd_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);
  CommandData *data = fpi_ssm_get_data (ssm);

  self->cmd_ssm = NULL;

  if (error)
    {
      if (data->callback)
        data->callback (self, NULL, error);
      else
        g_error_free (error);
    }
}

static FpiUsbTransfer *
prepare_bulk_transfer (FpDevice      *dev,
                       guint8        *data,
                       gsize          data_len,
                       GDestroyNotify free_func)
{
  g_autoptr(FpiUsbTransfer) transfer = NULL;

  g_return_val_if_fail (data || data_len == 0, NULL);

  transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer,
                                   EP_OUT,
                                   data,
                                   data_len,
                                   free_func);

  return g_steal_pointer (&transfer);
}


static void
fp_ctrl_cmd_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  g_autofree CommandData *data = g_steal_pointer (&user_data);

  g_return_if_fail (data != NULL);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->direction == G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE)
    {
      if (data->callback)
        data->callback (self, NULL, NULL);
    }
  else
    {
      if (transfer->actual_length == 0)
        {
          fp_info ("Control transfer receive data failed!");
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      if (data->callback)
        data->callback (self, transfer->buffer, NULL);
    }
}

static void
rtk_send_ctrl_cmd (FpiDeviceRealtek    *self,
                   struct rtk_cmd_ctrl *ctrl_cmd,
                   guint8              *cmd_data,
                   SynCmdMsgCallback    callback)
{
  FpiUsbTransfer *transfer = NULL;
  g_autofree CommandData *data = g_new0 (CommandData, 1);

  data->callback = callback;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_control (transfer,
                                 ctrl_cmd->direction,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 ctrl_cmd->request,
                                 ctrl_cmd->value,
                                 ctrl_cmd->index,
                                 ctrl_cmd->len);

  transfer->ssm = self->task_ssm;
  if (ctrl_cmd->direction == G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST &&
      cmd_data != NULL && ctrl_cmd->len != 0)
    memcpy (transfer->buffer, cmd_data, ctrl_cmd->len);

  fpi_usb_transfer_submit (transfer,
                           CMD_TIMEOUT,
                           NULL,
                           fp_ctrl_cmd_cb,
                           g_steal_pointer (&data));
}

static void
rtk_sensor_bulk_cmd (FpiDeviceRealtek *self,
                     guint8           *cmd,
                     guint8           *trans_data,
                     FpRtkMsgType      message_type,
                     gboolean          bwait_data_delay,
                     SynCmdMsgCallback callback)
{
  g_autoptr(FpiUsbTransfer) cmd_transfer = NULL;
  g_autoptr(FpiUsbTransfer) data_transfer = NULL;
  CommandData *data = g_new0 (CommandData, 1);

  self->cmd_type = GET_BULK_CMD_TYPE (cmd[0]);
  self->message_type = message_type;
  self->trans_data_len = GET_TRANS_DATA_LEN (cmd[11], cmd[10]);
  self->cmd_cancellable = bwait_data_delay;

  cmd_transfer = prepare_bulk_transfer (FP_DEVICE (self), cmd, FP_RTK_CMD_BULK_TOTAL_LEN, NULL);
  self->cmd_transfer = g_steal_pointer (&cmd_transfer);

  if ((self->cmd_type == FP_RTK_CMD_BULK_WRITE) && trans_data)
    {
      data_transfer = prepare_bulk_transfer (FP_DEVICE (self), trans_data, self->trans_data_len, g_free);
      self->data_transfer = g_steal_pointer (&data_transfer);
    }

  self->cmd_ssm = fpi_ssm_new (FP_DEVICE (self),
                               fp_cmd_run_state,
                               FP_RTK_CMD_NUM_STATES);

  data->callback = callback;
  fpi_ssm_set_data (self->cmd_ssm, data, (GDestroyNotify) fp_cmd_ssm_done_data_free);

  fpi_ssm_start (self->cmd_ssm, fp_cmd_ssm_done);
}

static void
fp_verify_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Verify complete!");

  if (error && error->domain == FP_DEVICE_RETRY)
    {
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
      else
        fpi_device_identify_report (dev, NULL, NULL, g_steal_pointer (&error));
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);

  self->task_ssm = NULL;
}

static void
fp_enroll_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);
  FpPrint *print = NULL;

  fp_info ("Enrollment complete!");

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      self->task_ssm = NULL;
      return;
    }

  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
  self->task_ssm = NULL;
}

static void
fp_init_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Init complete!");

  sdcp_clear_host_key (self);
  fpi_device_open_complete (dev, error);
  self->task_ssm = NULL;
}

static void
fp_delete_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Delete print complete!");

  fpi_device_delete_complete (dev, error);
  self->task_ssm = NULL;
}

static void
fp_verify_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_VERIFY_GET_TEMPLATE:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);
      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_verify_get_template_cb);
      break;

    case FP_RTK_VERIFY_CAPTURE:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);

      /* SDCP sensors expect the purpose here, like the Windows driver sends */
      co_start_capture.param[0] = self->sdcp ? self->fp_purpose : 0;
      cmd_buf = (guint8 *) &co_start_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_VERIFY_FINISH_CAPTURE:
      co_finish_capture.param[0] = self->sdcp ? self->fp_purpose : 0;
      cmd_buf = (guint8 *) &co_finish_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_finish_capture_cb);
      break;

    case FP_RTK_VERIFY_ACCEPT_SAMPLE:
      co_accept_sample.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_accept_sample;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_accept_sample_cb);
      break;

    case FP_RTK_VERIFY_SDCP_NONCE:
      if (!self->sdcp)
        {
          fpi_ssm_next_state (ssm);
          break;
        }

      if (!sdcp_random (self->sdcp_nonce, SDCP_RANDOM_LEN))
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "SDCP: random generation failed"));
          break;
        }

      cmd_buf = (guint8 *) &sdcp_identify_nonce;
      rtk_sensor_bulk_cmd (self, cmd_buf, g_memdup2 (self->sdcp_nonce, SDCP_RANDOM_LEN),
                           FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_VERIFY_INDENTIFY_FEATURE:
      if (self->sdcp)
        {
          cmd_buf = (guint8 *) &sdcp_identify;
          rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_sdcp_identify_cb);
          break;
        }

      cmd_buf = (guint8 *) &nor_identify_feature;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_identify_feature_cb);
      break;

    case FP_RTK_VERIFY_UPDATE_TEMPLATE:
      cmd_buf = (guint8 *) &co_update_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_update_template_cb);
      break;

    case FP_RTK_VERIFY_CANCEL_CAPTURE:
      co_cancel_capture.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_cancel_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;
    }
}

static void
fp_enroll_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autofree gchar *user_id = NULL;
  g_autofree guint8 *payload = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  FpPrint *print = NULL;
  guint8 *cmd_buf = NULL;
  GVariant *uid = NULL;
  GVariant *data = NULL;
  gsize user_id_len;
  guint finger;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_ENROLL_GET_TEMPLATE:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_enroll_get_template_cb);
      break;

    case FP_RTK_ENROLL_BEGIN_POS:
      if (self->sdcp)
        {
          /* 45 09 <slot>: the chip refuses (01 f7) a slot that is in use */
          sdcp_enroll_begin.param[0] = self->pos_index;
          cmd_buf = (guint8 *) &sdcp_enroll_begin;
          rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_sdcp_enroll_begin_cb);
          break;
        }

      nor_enroll_begin.param[0] = self->pos_index;
      cmd_buf = (guint8 *) &nor_enroll_begin;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_ENROLL_CAPTURE:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);

      co_start_capture.param[0] = self->sdcp ? self->fp_purpose : 0;
      cmd_buf = (guint8 *) &co_start_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_ENROLL_FINISH_CAPTURE:
      co_finish_capture.param[0] = self->sdcp ? self->fp_purpose : 0;
      cmd_buf = (guint8 *) &co_finish_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_finish_capture_cb);
      break;

    case FP_RTK_ENROLL_ACCEPT_SAMPLE:
      co_accept_sample.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_accept_sample;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_accept_sample_cb);
      break;

    case FP_RTK_ENROLL_CHECK_DUPLICATE:
      co_check_duplicate.param[0] = self->sdcp ? 0x01 : 0x00;
      cmd_buf = (guint8 *) &co_check_duplicate;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_check_duplicate_cb);
      break;

    case FP_RTK_ENROLL_COMMIT:
      gchar *valid_uid = NULL;
      gint payload_len;

      if (self->sdcp)
        {
          guint8 sdcp_id[SDCP_ID_LEN];

          /* enrollment id = HMAC (s, "enroll\0" || nonce from 45 09) */
          if (!sdcp_hmac (self->sdcp_mac_key, "enroll", self->sdcp_enroll_nonce,
                          SDCP_RANDOM_LEN, NULL, 0, sdcp_id))
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                             "SDCP: enrollment id failed"));
              break;
            }

          finger = SDCP_FINGER_BASE + self->pos_index;
          fpi_device_get_enroll_data (device, &print);
          sdcp_print_new (self, print, finger, sdcp_id);
          fp_dbg ("SDCP commit slot %d finger 0x%02x", self->pos_index, finger);

          sdcp_enroll_commit.param[0] = finger;
          cmd_buf = (guint8 *) &sdcp_enroll_commit;
          rtk_sensor_bulk_cmd (self, cmd_buf, g_memdup2 (sdcp_id, SDCP_ID_LEN),
                               FP_RTK_MSG_DEFAULT, 1, fp_enroll_commit_cb);
          break;
        }

      payload_len = UID_PAYLOAD_LEN_DEFAULT;

      fpi_device_get_enroll_data (device, &print);
      user_id = fpi_print_generate_user_id (print);
      user_id_len = MIN (DEFAULT_UID_LEN, strlen (user_id));

      payload = g_malloc0 (payload_len);
      memcpy (payload, user_id, user_id_len);
      valid_uid = (gchar *) payload;

      finger = SUB_FINGER_01;
      uid = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                       valid_uid,
                                       user_id_len,
                                       1);
      data = g_variant_new ("(y@ay)",
                            finger,
                            uid);

      fpi_print_set_type (print, FPI_PRINT_RAW);
      fpi_print_set_device_stored (print, TRUE);
      g_object_set (print, "fpi-data", data, NULL);
      g_object_set (print, "description", valid_uid, NULL);

      g_debug ("user_id: %s, finger: 0x%x", user_id, finger);

      nor_enroll_commit.param[0] = SUB_FINGER_01;
      nor_enroll_commit.data_len[0] = GET_LEN_L (payload_len);
      nor_enroll_commit.data_len[1] = GET_LEN_H (payload_len);

      cmd_buf = (guint8 *) &nor_enroll_commit;
      rtk_sensor_bulk_cmd (self, cmd_buf, g_steal_pointer (&payload),
                           FP_RTK_MSG_DEFAULT, 1, fp_enroll_commit_cb);
      break;

    case FP_RTK_ENROLL_CANCEL_CAPTURE:
      co_cancel_capture.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_cancel_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
    }
}

static void
fp_init_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_INIT_GET_DEVICE_INFO:
      rtk_send_ctrl_cmd (self, &get_device_info, NULL, fp_get_device_info_cb);
      break;

    case FP_RTK_INIT_SELECT_OS:
      /* the Windows driver never sends select_system to SDCP sensors */
      if (self->sdcp)
        {
          fpi_ssm_next_state (ssm);
          break;
        }

      co_select_system.param[0] = 0x01;
      cmd_buf = (guint8 *) &co_select_system;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_INIT_GET_ENROLL_NUM:
      cmd_buf = (guint8 *) &co_get_enroll_num;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_get_enroll_num_cb);
      break;

    case FP_RTK_INIT_SDCP_CONNECT:
      {
        GError *error = NULL;
        guint8 *payload;

        if (!self->sdcp)
          {
            fpi_ssm_jump_to_state (ssm, FP_RTK_INIT_NUM_STATES);
            break;
          }

        payload = sdcp_connect_payload (self, &error);
        if (!payload)
          {
            fpi_ssm_mark_failed (ssm, error);
            break;
          }

        cmd_buf = (guint8 *) &sdcp_connect;
        rtk_sensor_bulk_cmd (self, cmd_buf, payload, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      }
      break;

    case FP_RTK_INIT_SDCP_CONNECT_RESPONSE:
      cmd_buf = (guint8 *) &sdcp_connect_response;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_sdcp_connect_response_cb);
      break;
    }
}

static void
fp_delete_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_DELETE_GET_POS:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_get_delete_pos_cb);
      break;

    case FP_RTK_DELETE_PRINT:
      co_delete_record.param[0] = self->pos_index;
      cmd_buf = (guint8 *) &co_delete_record;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;
    }
}


static void
identify_verify (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  FpiDeviceAction current_action;

  G_DEBUG_HERE ();
  current_action = fpi_device_get_current_action (device);

  g_assert (current_action == FPI_DEVICE_ACTION_VERIFY ||
            current_action == FPI_DEVICE_ACTION_IDENTIFY);

  self->fp_purpose = FP_RTK_PURPOSE_IDENTIFY;

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_verify_sm_run_state,
                                     FP_RTK_VERIFY_NUM_STATES,
                                     FP_RTK_VERIFY_CANCEL_CAPTURE,
                                     "Verify & Identify");

  fpi_ssm_start (self->task_ssm, fp_verify_ssm_done);
}

static void
enroll (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();
  self->enroll_stage = 0;
  self->fp_purpose = FP_RTK_PURPOSE_ENROLL;

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_enroll_sm_run_state,
                                     FP_RTK_ENROLL_NUM_STATES,
                                     FP_RTK_ENROLL_CANCEL_CAPTURE,
                                     "Enroll");

  fpi_ssm_start (self->task_ssm, fp_enroll_ssm_done);
}

static void
dev_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  GError *error = NULL;
  g_autofree gchar *product = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();
  /* Claim usb interface */
  usb_dev = fpi_device_get_usb_device (device);
  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_reset (usb_dev, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  product = g_usb_device_get_string_descriptor (usb_dev,
                                                g_usb_device_get_product_index (usb_dev),
                                                &error);

  if (product)
    fp_dbg ("Device name: %s", product);

  self->sdcp = (fpi_device_get_driver_data (device) & RTK_DEVICE_SDCP) != 0;
  self->max_enroll_stage = self->sdcp ? MAX_ENROLL_SAMPLES_SDCP : MAX_ENROLL_SAMPLES;
  fpi_device_set_nr_enroll_stages (device, self->max_enroll_stage);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (device)), 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);

  fpi_device_probe_complete (device, NULL, product, error);
}

static void
dev_init (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  GError *error = NULL;

  G_DEBUG_HERE ();
  self->sdcp = (fpi_device_get_driver_data (device) & RTK_DEVICE_SDCP) != 0;
  self->sdcp_connected = FALSE;

  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Claim usb interface */
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device), 0, 0, &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_init_sm_run_state,
                                     FP_RTK_INIT_NUM_STATES,
                                     FP_RTK_INIT_NUM_STATES,
                                     "Init");

  fpi_ssm_start (self->task_ssm, fp_init_ssm_done);
}

static void
dev_exit (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  g_autoptr(GError) release_error = NULL;

  G_DEBUG_HERE ();

  sdcp_clear_host_key (self);
  memset (self->sdcp_mac_key, 0, sizeof (self->sdcp_mac_key));
  self->sdcp_connected = FALSE;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (self)), 0, 0, &release_error);

  fpi_device_close_complete (device, release_error);
}

static void
delete_print (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_delete_sm_run_state,
                                     FP_RTK_DELETE_NUM_STATES,
                                     FP_RTK_DELETE_NUM_STATES,
                                     "Delete print");

  fpi_ssm_start (self->task_ssm, fp_delete_ssm_done);
}

static void
clear_storage (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  G_DEBUG_HERE ();
  co_delete_record.param[0] = 0xff;
  cmd_buf = (guint8 *) &co_delete_record;
  rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_clear_storage_cb);
}

static void
list_print (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  G_DEBUG_HERE ();

  if (self->template_num < 0)
    {
      fpi_device_list_complete (device, NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Get template number failed!"));
      return;
    }

  if (self->template_num == 0)
    {
      g_autoptr(GPtrArray) list_result = g_ptr_array_new_with_free_func (g_object_unref);
      fpi_device_list_complete (device, g_steal_pointer (&list_result), NULL);
      return;
    }

  co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
  co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

  cmd_buf = (guint8 *) &co_get_template;
  rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 1, fp_list_cb);
}

static void
fpi_device_realtek_init (FpiDeviceRealtek *self)
{
}

static void
fpi_device_realtek_class_init (FpiDeviceRealtekClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Realtek MOC Fingerprint Sensor";

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = MAX_ENROLL_SAMPLES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_init;
  dev_class->close = dev_exit;
  dev_class->probe = dev_probe;
  dev_class->verify = identify_verify;
  dev_class->identify = identify_verify;
  dev_class->enroll = enroll;
  dev_class->delete = delete_print;
  dev_class->clear_storage = clear_storage;
  dev_class->list = list_print;

  fpi_device_class_auto_initialize_features (dev_class);
}
