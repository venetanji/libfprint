/*
 * Goodix 27c6:5125 fingerprint driver
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "drivers_api.h"
#include "goodix5125-calib.h"
#include "goodix5125-fdt.h"
#include "goodix5125-image.h"
#include "goodix5125-pairing.h"
#include "goodix5125-tls.h"
#include "goodix5125-usb-async.h"
#include "goodix5125-usb.h"
#include "goodix5125-algo.h"

#include <errno.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#define COMMAND_TIMEOUT_MS 5000
#define NOP_TIMEOUT_MS 100
#define TLS_PLAINTEXT_SIZE 7693
#define TLS_HEADER_SIZE 8
#define TLS_TRAILER_SIZE 5
#define GOODIX5125_USB_INTERFACE 1

/* Host state (PSK, openchicago calibration state) lives in fprintd's state
 * directory; a user without access to it (examples, tests) falls back to
 * $XDG_STATE_HOME/libfprint/goodix5125.  Each file can be overridden. */
#define GOODIX5125_STATE_DIR_ENV "GOODIX5125_STATE_DIR"
#define GOODIX5125_STATE_DEFAULT_DIR "/var/lib/fprint/goodix5125"
#define GOODIX5125_PSK_FILE_ENV "GOODIX5125_PSK_FILE"
#define GOODIX5125_PSK_FILE_NAME "psk"
#define GOODIX5125_STATE_FILE_ENV "GOODIX5125_STATE_FILE"
#define GOODIX5125_STATE_FILE_NAME "openchicago.state"
/* Learned templates (template study) per print, see goodix5125-algo.h. */
#define GOODIX5125_LEARNED_DIR_ENV "GOODIX5125_LEARNED_DIR"
#define GOODIX5125_LEARNED_DIR_NAME "learned"
/* Optional one-time import of a calibration in the [sensor id][payload]
 * layout (MR !648 chicago-calibration.dat). */
#define GOODIX5125_CHICAGO_CALIBRATION_ENV "GOODIX5125_CHICAGO_CALIBRATION_FILE"
#define GOODIX5125_CHICAGO_CALIBRATION_FILE_NAME "chicago-calibration.dat"

/* The PSK is written to the sensor only with GOODIX5125_PROVISION_PSK=random. */
#define GOODIX5125_PROVISION_PSK_ENV "GOODIX5125_PROVISION_PSK"

/* Finger detection, see begin_wait_for_finger () */
#define FDT_MODE_ARM 0x0d
#define FDT_MODE_DOWN 0x0c
#define FDT_PROBE_TIMEOUT_MS 2000
#define FDT_LIFT_POLL_MS 100

/* A second frame of the same touch during enrolment;
 * GOODIX5125_ENROLL_PAIR=0 disables it. */
#define GOODIX5125_ENROLL_PAIR_ENV "GOODIX5125_ENROLL_PAIR"

/* A rejected capture may be retried a few times within one verify() or
 * identify() before giving up. */
#define GOODIX5125_VERIFY_CAPTURE_ATTEMPTS 3

/* Bootstrap thresholds used only until cmd 0x36 measures this sensor's live
 * no-finger FDT baseline. Subsequent waits use dynamically derived values. */
static const guint8 default_fdt_base[GOODIX5125_FDT_BASE_SIZE] = {
  0x80, 0xad, 0x80, 0xbb, 0x80, 0xa2,
  0x80, 0xae, 0x80, 0xa3, 0x80, 0xae,
};

typedef enum {
  GOODIX5125_ACTION_NONE = 0,
  GOODIX5125_ACTION_ENROLL,
  GOODIX5125_ACTION_VERIFY,
  GOODIX5125_ACTION_IDENTIFY,
} Goodix5125Action;

/* System suspend during verify or identify, see goodix5125_suspend () */
typedef enum {
  GOODIX5125_SUSPEND_NONE = 0,
  GOODIX5125_SUSPEND_PENDING,
  GOODIX5125_SUSPEND_PARKED,
} Goodix5125SuspendState;

struct _FpDeviceGoodix5125
{
  FpDevice          parent;

  Goodix5125Tls    *tls;
  Goodix5125OtpInfo otp_info;
  guint8            fdt_base[GOODIX5125_FDT_BASE_SIZE];
  guint8            lift_base[GOODIX5125_FDT_BASE_SIZE];
  guint16           cached_image_base[GOODIX5125_IMAGE_N_PIXELS];
  guint8            sensor_id[OC_SENSOR_ID_LEN];
  Goodix5125Algo   *algo;
  gchar            *psk_path;
  gchar            *calidata_path;
  gchar            *emulation_dir;
  gboolean          enroll_pair;
  gboolean          operation_active;
  gboolean          initialized;

  /* Current enroll/verify/identify action state. */
  Goodix5125Action action_kind;

  guint16          first_frame[OC_FRAME_PIXELS];
  gboolean         have_first_frame;
  gboolean         enroll_complete;

  FpPrint         *match_template;
  GPtrArray       *match_gallery;
  guint            match_attempt;
  gboolean         match_exhausted_bad_quality;
  gboolean         match_decided;
  FpiMatchResult   match_verify_result;
  FpPrint         *match_identify_match;
  GVariant        *match_updated;

  gboolean         cycle_should_continue;
  GError          *cycle_error;

  /* The finger wait has no timeout: suspend, cancellation and removal end
   * it through wait_cancellable.  Suspend: see cycle_park (). */
  GCancellable          *wait_cancellable;
  Goodix5125SuspendState suspend_state;
  gboolean               resume_finish;
  gboolean               removed;
};

G_DECLARE_FINAL_TYPE (FpDeviceGoodix5125, fpi_device_goodix5125,
                      FPI, DEVICE_GOODIX5125, FpDevice)
G_DEFINE_TYPE (FpDeviceGoodix5125, fpi_device_goodix5125,
               FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x27c6, .pid = 0x5125 },
  { .vid = 0, .pid = 0 },
};

static guint16
read_le16 (const guint8 *data)
{
  guint16 value;

  memcpy (&value, data, sizeof (value));
  return GUINT16_FROM_LE (value);
}

static void
write_le16 (guint8 *data,
            guint16 value)
{
  value = GUINT16_TO_LE (value);
  memcpy (data, &value, sizeof (value));
}

static GBytes *
command_sync (FpDeviceGoodix5125    *self,
              Goodix5125Command      command,
              const guint8          *payload,
              gsize                  payload_length,
              gboolean               expect_reply,
              Goodix5125OuterFlag    reply_flag,
              Goodix5125ChecksumMode reply_checksum,
              guint                  timeout_ms,
              GError               **error)
{
  GBytes *result;

  result = goodix5125_usb_command_sync (FP_DEVICE (self), command,
                                        GOODIX5125_CHECKSUM_NORMAL,
                                        reply_checksum,
                                        payload, payload_length,
                                        expect_reply, reply_flag,
                                        timeout_ms, error);
  if (result == NULL && error != NULL && *error != NULL)
    g_prefix_error (error, "Goodix command 0x%02x: ", command);

  return result;
}

static gboolean
command_no_reply (FpDeviceGoodix5125 *self,
                  Goodix5125Command   command,
                  const guint8       *payload,
                  gsize               payload_length,
                  GError            **error)
{
  g_autoptr(GBytes) result = command_sync (
    self, command, payload, payload_length, FALSE,
    GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);
  return result != NULL;
}

static gboolean
query_state (FpDeviceGoodix5125 *self,
             GError            **error)
{
  static const guint8 payload[] = { 0x55 };

  g_autoptr(GBytes) response = command_sync (
    self, GOODIX5125_COMMAND_QUERY_STATE, payload, sizeof (payload), TRUE,
    GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);
  gsize length = 0;

  if (!response)
    return FALSE;
  g_bytes_get_data (response, &length);
  if (length < 14)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix MCU-state reply is shorter than 14 bytes");
      return FALSE;
    }
  return TRUE;
}

static gboolean
read_psk_hash (FpDeviceGoodix5125 *self,
               gboolean           *present,
               guint8              hash[GOODIX5125_PSK_SIZE],
               GError            **error)
{
  static const guint8 payload[] = {
    0x03, 0x00, 0x02, 0xbb,
    0x00, 0x00, 0x00, 0x00,
  };

  g_autoptr(GBytes) response = NULL;
  const guint8 *data;
  gsize length = 0;

  response = command_sync (
    self, GOODIX5125_COMMAND_READ_PSK_HASH,
    payload, sizeof (payload), TRUE,
    GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);
  if (!response)
    return FALSE;

  data = g_bytes_get_data (response, &length);
  return goodix5125_pairing_parse_hash_reply (data, length, present, hash,
                                              error);
}

static gboolean
provision_psk (FpDeviceGoodix5125 *self,
               const guint8        psk[GOODIX5125_PSK_SIZE],
               GError            **error)
{
  g_autoptr(GBytes) payload = NULL;
  g_autoptr(GBytes) response = NULL;
  guint8 wb[GOODIX5125_WB_SIZE];
  guint8 expected_hash[GOODIX5125_PSK_SIZE];
  guint8 actual_hash[GOODIX5125_PSK_SIZE];
  gboolean hash_present = FALSE;
  const guint8 *data;
  gsize length = 0;

  if (!goodix5125_pairing_self_test (error) ||
      !(payload = goodix5125_pairing_build_write_payload (psk, error)))
    return FALSE;

  data = g_bytes_get_data (payload, &length);
  response = command_sync (
    self, GOODIX5125_COMMAND_WRITE_PSK,
    data, length, TRUE,
    GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);
  if (!response)
    return FALSE;
  data = g_bytes_get_data (response, &length);
  if (length < 1 || data[0] != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix MCU rejected PSK provisioning");
      return FALSE;
    }

  if (!goodix5125_pairing_wrap (psk, wb, error) ||
      !goodix5125_pairing_hash (wb, expected_hash, error) ||
      !read_psk_hash (self, &hash_present, actual_hash, error))
    goto fail;
  if (!hash_present ||
      CRYPTO_memcmp (expected_hash, actual_hash,
                     GOODIX5125_PSK_SIZE) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix MCU did not retain the provisioned PSK");
      goto fail;
    }

  OPENSSL_cleanse (wb, sizeof (wb));
  OPENSSL_cleanse (expected_hash, sizeof (expected_hash));
  OPENSSL_cleanse (actual_hash, sizeof (actual_hash));
  fp_info ("PSK provisioning verified against the sensor hash");
  return TRUE;

fail:
  OPENSSL_cleanse (wb, sizeof (wb));
  OPENSSL_cleanse (expected_hash, sizeof (expected_hash));
  OPENSSL_cleanse (actual_hash, sizeof (actual_hash));
  return FALSE;
}

static gboolean
ensure_pairing (FpDeviceGoodix5125 *self,
                guint8              psk[GOODIX5125_PSK_SIZE],
                GError            **error)
{
  const gchar *path = self->psk_path;
  gboolean emulation = fpi_device_emulation_mode_enabled (FP_DEVICE (self));
  const gchar *provision = emulation ? NULL :
                           g_getenv (GOODIX5125_PROVISION_PSK_ENV);
  Goodix5125PskState state;
  guint8 wb[GOODIX5125_WB_SIZE];
  guint8 expected_hash[GOODIX5125_PSK_SIZE];
  guint8 sensor_hash[GOODIX5125_PSK_SIZE];
  gboolean sensor_has_key = FALSE;
  gboolean authorized = provision &&
                        g_ascii_strcasecmp (provision, "random") == 0;
  gboolean needs_provision = FALSE;
  gboolean success = FALSE;

  if (provision && *provision && !authorized)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s accepts only the value random",
                   GOODIX5125_PROVISION_PSK_ENV);
      goto out;
    }

  if (emulation)
    {
      /* Device emulation (umockdev tests): the recorded sensor is paired
       * with the all-zero key, which is never written. */
      memset (psk, 0, GOODIX5125_PSK_SIZE);
      state = GOODIX5125_PSK_STATE_FOUND;
    }
  else
    {
      state = goodix5125_pairing_load_psk (path, psk, error);
    }
  if (state == GOODIX5125_PSK_STATE_ERROR ||
      !read_psk_hash (self, &sensor_has_key, sensor_hash, error))
    goto out;

  if (state == GOODIX5125_PSK_STATE_MISSING)
    {
      /* Never generate or write a key implicitly: the PSK slot of the MCU
       * is write-only and a wrong write unpairs every other host. */
      if (!authorized)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                       "Goodix PSK file %s is missing; put the host key "
                       "there (the sensor %s a key), or set %s=random once "
                       "to pair the sensor with a new key",
                       path, sensor_has_key ? "already holds" : "holds no",
                       GOODIX5125_PROVISION_PSK_ENV);
          goto out;
        }
      if (RAND_bytes (psk, GOODIX5125_PSK_SIZE) != 1)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Could not generate a Goodix pairing key");
          goto out;
        }
      /* Persist the only recoverable copy before touching the write-only MCU
       * register. A failed state write must leave the current pairing intact. */
      if (!goodix5125_pairing_save_psk (path, psk, error))
        goto out;
      needs_provision = TRUE;
    }
  else
    {
      if (!goodix5125_pairing_wrap (psk, wb, error) ||
          !goodix5125_pairing_hash (wb, expected_hash, error))
        goto out;
      if (!sensor_has_key ||
          CRYPTO_memcmp (expected_hash, sensor_hash,
                         GOODIX5125_PSK_SIZE) != 0)
        {
          if (!authorized)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "The Goodix PSK in %s does not match the sensor "
                           "(%s); set %s=random once to write this key",
                           path, sensor_has_key ? "hash differs" : "no key",
                           GOODIX5125_PROVISION_PSK_ENV);
              goto out;
            }
          needs_provision = TRUE;
        }
    }

  if (needs_provision)
    {
      fp_warn ("provisioning the host pairing key in the Goodix sensor");
      if (!provision_psk (self, psk, error))
        goto out;
    }
  else
    {
      fp_dbg ("persisted PSK matches the sensor pairing hash");
    }

  success = TRUE;

out:
  OPENSSL_cleanse (wb, sizeof (wb));
  OPENSSL_cleanse (expected_hash, sizeof (expected_hash));
  OPENSSL_cleanse (sensor_hash, sizeof (sensor_hash));
  return success;
}

static gboolean
reset_sensor (FpDeviceGoodix5125 *self,
              GError            **error)
{
  static const guint8 payload[] = { 0x05, 20 };

  g_autoptr(GBytes) response = command_sync (
    self, GOODIX5125_COMMAND_RESET, payload, sizeof (payload), TRUE,
    GOODIX5125_OUTER_FLAG_MESSAGE, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);
  gsize length = 0;
  const guint8 *data;

  if (!response)
    return FALSE;
  data = g_bytes_get_data (response, &length);
  if (length < 3 || data[0] != 1 || read_le16 (data + 1) != 2048)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix reset reply is invalid");
      return FALSE;
    }
  return TRUE;
}

static GBytes *
read_register (FpDeviceGoodix5125 *self,
               guint16             address,
               guint8              length,
               GError            **error)
{
  guint8 payload[4] = { 0 };

  write_le16 (payload + 1, address);
  payload[3] = length;
  return command_sync (self, GOODIX5125_COMMAND_READ_SENSOR_REGISTER,
                       payload, sizeof (payload), TRUE,
                       GOODIX5125_OUTER_FLAG_MESSAGE,
                       GOODIX5125_CHECKSUM_NORMAL,
                       COMMAND_TIMEOUT_MS, error);
}

static gboolean
write_register (FpDeviceGoodix5125 *self,
                guint16             address,
                guint16             value,
                GError            **error)
{
  guint8 payload[5] = { 0 };

  write_le16 (payload + 1, address);
  write_le16 (payload + 3, value);
  return command_no_reply (self, GOODIX5125_COMMAND_WRITE_SENSOR_REGISTER,
                           payload, sizeof (payload), error);
}

static GBytes *
fdt_command (FpDeviceGoodix5125 *self,
             Goodix5125Command   command,
             guint8              mode,
             const guint8        base[GOODIX5125_FDT_BASE_SIZE],
             guint               timeout_ms,
             GError            **error)
{
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { mode, 0x01 };

  memcpy (payload + 2, base, GOODIX5125_FDT_BASE_SIZE);
  return command_sync (self, command, payload, sizeof (payload), TRUE,
                       GOODIX5125_OUTER_FLAG_MESSAGE,
                       GOODIX5125_CHECKSUM_NORMAL, timeout_ms, error);
}

static gboolean
establish_tls (FpDeviceGoodix5125 *self,
               const guint8        psk[32],
               GError            **error)
{
  static const guint8 request_payload[] = { 0x00, 0x00 };

  g_autoptr(GBytes) input = command_sync (
    self, GOODIX5125_COMMAND_REQUEST_TLS,
    request_payload, sizeof (request_payload), TRUE,
    GOODIX5125_OUTER_FLAG_TLS, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);

  if (!input)
    return FALSE;
  g_clear_pointer (&self->tls, goodix5125_tls_free);
  self->tls = goodix5125_tls_new (
    psk, 32, fpi_device_emulation_mode_enabled (FP_DEVICE (self)), error);
  if (!self->tls)
    return FALSE;

  for (guint iteration = 0; iteration < 32; iteration++)
    {
      gsize input_length = 0;
      const guint8 *input_data = g_bytes_get_data (input, &input_length);
      Goodix5125TlsResult result;
      g_autoptr(GBytes) output = NULL;
      gsize output_length = 0;
      const guint8 *output_data;

      if (!goodix5125_tls_feed_ciphertext (self->tls, input_data,
                                           input_length, error))
        return FALSE;
      result = goodix5125_tls_handshake (self->tls, error);
      if (result == GOODIX5125_TLS_ERROR)
        return FALSE;

      output = goodix5125_tls_take_ciphertext (self->tls);
      output_data = g_bytes_get_data (output, &output_length);
      if (output_length &&
          !goodix5125_usb_outer_write_sync (FP_DEVICE (self),
                                            GOODIX5125_OUTER_FLAG_TLS,
                                            output_data, output_length,
                                            COMMAND_TIMEOUT_MS, error))
        return FALSE;
      if (result == GOODIX5125_TLS_COMPLETE)
        {
          /* Required by the known-good OEM/Python bridge before D4. */
          g_usleep (10 * 1000);
          return command_no_reply (self,
                                   GOODIX5125_COMMAND_TLS_ESTABLISHED,
                                   request_payload,
                                   sizeof (request_payload), error);
        }

      g_clear_pointer (&input, g_bytes_unref);
      input = goodix5125_usb_outer_read_sync (FP_DEVICE (self),
                                              GOODIX5125_OUTER_FLAG_TLS,
                                              COMMAND_TIMEOUT_MS, error);
      if (!input)
        return FALSE;
    }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                       "Goodix TLS handshake exceeded 32 USB frames");
  return FALSE;
}

static GBytes *
decrypt_packed (FpDeviceGoodix5125 *self,
                GBytes             *encrypted,
                GError            **error)
{
  g_autoptr(GByteArray) plaintext = g_byte_array_new ();
  gsize encrypted_length = 0;
  const guint8 *encrypted_data;

  encrypted_data = g_bytes_get_data (encrypted, &encrypted_length);
  if (!goodix5125_tls_feed_ciphertext (self->tls, encrypted_data,
                                       encrypted_length, error) ||
      !goodix5125_tls_read_plaintext (self->tls, plaintext, error))
    return NULL;
  if (plaintext->len != TLS_PLAINTEXT_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Goodix TLS image is %u bytes, expected %u",
                   plaintext->len, TLS_PLAINTEXT_SIZE);
      return NULL;
    }
  return g_bytes_new (plaintext->data + TLS_HEADER_SIZE,
                      plaintext->len - TLS_HEADER_SIZE - TLS_TRAILER_SIZE);
}

static GBytes *
capture_packed (FpDeviceGoodix5125 *self,
                GError            **error)
{
  static const guint8 payload[] = { 0x01, 0x00 };

  g_autoptr(GBytes) encrypted = command_sync (
    self, GOODIX5125_COMMAND_GET_IMAGE, payload, sizeof (payload), TRUE,
    GOODIX5125_OUTER_FLAG_TLS, GOODIX5125_CHECKSUM_NORMAL,
    COMMAND_TIMEOUT_MS, error);

  if (!encrypted)
    return NULL;
  return decrypt_packed (self, encrypted, error);
}

static gboolean
initialize_sensor (FpDeviceGoodix5125 *self,
                   GError            **error)
{
  guint8 psk[32] = { 0 };
  guint8 otp[GOODIX5125_OTP_SIZE];
  guint8 config[GOODIX5125_CONFIG_SIZE];
  static const guint8 enable_payload[] = { 0x01, 0x00 };
  static const guint8 otp_payload[] = { 0x00, 0x00 };
  static const guint8 idle_payload[] = { 20, 0x00 };
  guint8 frequency_payload[2];

  g_autoptr(GBytes) response = NULL;
  g_autoptr(GBytes) fdt_reply = NULL;
  g_autoptr(GBytes) clear = NULL;
  gsize length = 0;
  const guint8 *data;
  guint8 next_base[GOODIX5125_FDT_BASE_SIZE];
  gboolean finger_off = FALSE;
  gboolean success = FALSE;

  fp_dbg ("starting volatile ChicagoHS initialization");
  if (!goodix5125_usb_drain_sync (FP_DEVICE (self), NOP_TIMEOUT_MS, error) ||
      !goodix5125_usb_nop_sync (FP_DEVICE (self), NOP_TIMEOUT_MS, error) ||
      !command_no_reply (self, GOODIX5125_COMMAND_ENABLE,
                         enable_payload, sizeof (enable_payload), error) ||
      !goodix5125_usb_nop_sync (FP_DEVICE (self), NOP_TIMEOUT_MS, error) ||
      !reset_sensor (self, error))
    goto out;

  response = read_register (self, 0x0000, 4, error);
  if (!response)
    goto out;
  data = g_bytes_get_data (response, &length);
  if (length < 4 || memcmp (data, "\xa2\x04\x25\x00", 4) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix chip ID is not ChicagoHS 0x2504");
      goto out;
    }

  g_clear_pointer (&response, g_bytes_unref);
  response = command_sync (self, GOODIX5125_COMMAND_READ_OTP,
                           otp_payload, sizeof (otp_payload), TRUE,
                           GOODIX5125_OUTER_FLAG_MESSAGE,
                           GOODIX5125_CHECKSUM_NORMAL,
                           COMMAND_TIMEOUT_MS, error);
  if (!response)
    goto out;
  data = g_bytes_get_data (response, &length);
  if (length != sizeof (otp))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix OTP reply is not 64 bytes");
      goto out;
    }
  memcpy (otp, data, sizeof (otp));

  if (!goodix5125_otp_parse (otp, sizeof (otp), &self->otp_info, error))
    goto out;

  /* OTP is factory-programmed and stable, so its first 16 bytes bind all
   * persisted host state to this exact sensor. */
  memcpy (self->sensor_id, otp, sizeof (self->sensor_id));
  memcpy (self->fdt_base, default_fdt_base, sizeof (self->fdt_base));

  if (!ensure_pairing (self, psk, error) ||
      !goodix5125_config_build (&self->otp_info, config, error) ||
      !reset_sensor (self, error) ||
      !command_no_reply (self, GOODIX5125_COMMAND_MCU_IDLE,
                         idle_payload, sizeof (idle_payload), error) ||
      !write_register (self, 0x0220,
                       (self->otp_info.dac[0] << 4) | 8, error) ||
      !write_register (self, 0x0236, self->otp_info.dac[1], error) ||
      !write_register (self, 0x0238, self->otp_info.dac[2], error) ||
      !write_register (self, 0x023a, self->otp_info.dac[3], error))
    goto out;

  fp_dbg ("OTP validated; uploading volatile sensor configuration");

  g_clear_pointer (&response, g_bytes_unref);
  response = command_sync (self, GOODIX5125_COMMAND_UPLOAD_CONFIG,
                           config, sizeof (config), TRUE,
                           GOODIX5125_OUTER_FLAG_MESSAGE,
                           GOODIX5125_CHECKSUM_NORMAL,
                           COMMAND_TIMEOUT_MS, error);
  if (!response)
    goto out;
  data = g_bytes_get_data (response, &length);
  if (length < 1 || data[0] != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Goodix rejected the volatile ChicagoHS config");
      goto out;
    }

  write_le16 (frequency_payload, 100);
  g_clear_pointer (&response, g_bytes_unref);
  response = command_sync (self,
                           GOODIX5125_COMMAND_SET_POWERDOWN_FREQUENCY,
                           frequency_payload, sizeof (frequency_payload), TRUE,
                           GOODIX5125_OUTER_FLAG_MESSAGE,
                           GOODIX5125_CHECKSUM_NORMAL,
                           COMMAND_TIMEOUT_MS, error);
  if (!response)
    goto out;
  data = g_bytes_get_data (response, &length);
  if (length < 1 || data[0] != 1 || !establish_tls (self, psk, error) ||
      !goodix5125_usb_nop_sync (FP_DEVICE (self), NOP_TIMEOUT_MS, error) ||
      !query_state (self, error))
    goto out;

  fp_dbg ("TLS-PSK session established");

  fdt_reply = fdt_command (self, GOODIX5125_COMMAND_FDT_MODE, 0x09,
                           self->fdt_base, COMMAND_TIMEOUT_MS, error);
  if (!fdt_reply)
    goto out;

  {
    static const guint8 nav_payload[] = { 0x01, 0x00 };
    g_clear_pointer (&response, g_bytes_unref);
    response = command_sync (self, GOODIX5125_COMMAND_NAVIGATION,
                             nav_payload, sizeof (nav_payload), TRUE,
                             GOODIX5125_OUTER_FLAG_MESSAGE,
                             GOODIX5125_CHECKSUM_TRAILER_88,
                             COMMAND_TIMEOUT_MS, error);
    if (!response)
      goto out;
  }

  data = g_bytes_get_data (fdt_reply, &length);
  if (!goodix5125_fdt_reply_is_finger_off (data, length, &finger_off,
                                           error) ||
      !goodix5125_fdt_down_base_from_reply (data, length, next_base, error))
    goto out;
  g_clear_pointer (&fdt_reply, g_bytes_unref);
  fdt_reply = fdt_command (self, GOODIX5125_COMMAND_FDT_MODE, 0x09,
                           next_base, COMMAND_TIMEOUT_MS, error);
  if (!fdt_reply)
    goto out;

  data = g_bytes_get_data (fdt_reply, &length);
  if (!goodix5125_fdt_reply_is_finger_off (data, length, &finger_off,
                                           error))
    goto out;
  if (!finger_off)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Remove the finger before Goodix calibration and "
                           "try again");
      goto out;
    }

  g_clear_pointer (&response, g_bytes_unref);
  response = read_register (self, 0x0082, 2, error);
  if (!response)
    goto out;
  data = g_bytes_get_data (response, &length);
  if (length < 2 || data[0] != 0x80 ||
      data[1] != self->otp_info.fdt_delta)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix FDT register 0x0082 is unstable");
      goto out;
    }

  clear = capture_packed (self, error);
  if (!clear)
    goto out;
  data = g_bytes_get_data (clear, &length);
  if (!goodix5125_image_decode_transposed (
        data, length, self->cached_image_base,
        GOODIX5125_IMAGE_N_PIXELS, error))
    goto out;

  data = g_bytes_get_data (fdt_reply, &length);
  if (!goodix5125_fdt_down_base_from_reply (data, length,
                                            self->fdt_base, error))
    goto out;
  g_clear_pointer (&fdt_reply, g_bytes_unref);
  fdt_reply = fdt_command (self, GOODIX5125_COMMAND_FDT_MODE, 0x09,
                           self->fdt_base, COMMAND_TIMEOUT_MS, error);
  if (!fdt_reply)
    goto out;
  data = g_bytes_get_data (fdt_reply, &length);
  if (!goodix5125_fdt_reply_is_finger_off (data, length, &finger_off,
                                           error))
    goto out;
  if (!finger_off)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Finger contact changed during Goodix calibration; "
                           "remove the finger and try again");
      goto out;
    }
  if (!goodix5125_fdt_down_base_from_reply (data, length,
                                            self->fdt_base, error))
    goto out;

  if (!goodix5125_algo_activate (self->algo, self->sensor_id,
                                 self->cached_image_base,
                                 self->calidata_path, error))
    goto out;
  memcpy (self->lift_base, self->fdt_base, sizeof (self->lift_base));

  fp_dbg ("live no-finger ImageBase captured, openchicago state %s",
          goodix5125_algo_get_state_path (self->algo));

  self->initialized = TRUE;
  success = TRUE;
  fp_dbg ("ChicagoHS initialization completed");

out:
  OPENSSL_cleanse (psk, sizeof (psk));
  return success;
}

static gboolean
ensure_activated (FpDeviceGoodix5125 *self,
                  GError            **error)
{
  if (self->initialized)
    return TRUE;
  return initialize_sensor (self, error);
}

/* ---------------------------------------------------------------------
 * Shared async capture cycle: wait for finger -> capture+decrypt -> hand the
 * frame to openchicago (goodix5125-algo.c) -> wait for finger removal.  What
 * happens with a frame (enrolment step, or match against a template or
 * gallery) is decided in capture_ready_cb() based on self->action_kind; the
 * outcome only takes effect once the finger-off wait below completes, so the
 * sensor is always left in a known, no-finger state between actions.
 *
 * Finger wait: FDT mode 0x0d (arm) -> FDT-down 0x0c probe, 2 s, no reply
 * expected -> MCU state -> FDT-down 0x0c; lift = poll FDT mode 0x0d until
 * the touch mask (reply byte 2) is 0.  FDT-up is not used: it was not
 * reliable on 27c6:5125.
 *
 * A system suspend stops the cycle at its next command and keeps the
 * verify or identify active, see cycle_park ().
 * --------------------------------------------------------------------- */

static void begin_wait_for_finger (FpDeviceGoodix5125 *self);
static void begin_capture (FpDeviceGoodix5125 *self);
static void finish_action (FpDeviceGoodix5125 *self);

static void
save_algo_state (FpDeviceGoodix5125 *self)
{
  g_autoptr(GError) error = NULL;

  if (goodix5125_algo_is_active (self->algo) &&
      !goodix5125_algo_save_state (self->algo, &error))
    fp_warn ("could not save the openchicago state: %s", error->message);
}

static void
reset_action_state (FpDeviceGoodix5125 *self)
{
  if (self->action_kind == GOODIX5125_ACTION_ENROLL && self->algo)
    goodix5125_algo_enroll_cancel (self->algo);
  self->action_kind = GOODIX5125_ACTION_NONE;
  self->have_first_frame = FALSE;
  self->enroll_complete = FALSE;
  g_clear_object (&self->match_template);
  g_clear_pointer (&self->match_gallery, g_ptr_array_unref);
  self->match_attempt = 0;
  self->match_exhausted_bad_quality = FALSE;
  self->match_decided = FALSE;
  self->match_verify_result = FPI_MATCH_FAIL;
  self->match_identify_match = NULL;
  g_clear_pointer (&self->match_updated, g_variant_unref);
  self->cycle_should_continue = FALSE;
  g_clear_error (&self->cycle_error);
}

static void
set_cycle_error (FpDeviceGoodix5125 *self,
                 GError             *error)
{
  /* libusb reports an unplug before libfprint learns about the removal */
  if (g_error_matches (error, G_USB_DEVICE_ERROR,
                       G_USB_DEVICE_ERROR_NO_DEVICE))
    {
      g_error_free (error);
      error = fpi_device_error_new (FP_DEVICE_ERROR_REMOVED);
    }

  if (!self->cycle_error)
    self->cycle_error = error;
  else
    g_error_free (error);
}

/* A suspend waits for the capture cycle to stop, and neither a cancellation
 * nor the removal of the device ends the action anyway. */
static gboolean
cycle_may_park (FpDeviceGoodix5125 *self)
{
  return self->suspend_state == GOODIX5125_SUSPEND_PENDING &&
         !self->removed &&
         !g_cancellable_is_cancelled (
    fpi_device_get_cancellable (FP_DEVICE (self)));
}

/* With a suspend pending, the cycle stops where it would send its next
 * command: nothing is in flight while the system sleeps and the action
 * stays active.  A match decided meanwhile is dropped; on resume the cycle
 * waits for the finger to lift (the sensor is the power button) and for a
 * new touch, or with @finish ends the action with the cycle error. */
static gboolean
cycle_park (FpDeviceGoodix5125 *self,
            gboolean            finish)
{
  if (!cycle_may_park (self))
    return FALSE;

  if (!finish && !self->cycle_error)
    {
      self->match_attempt = 0;
      self->match_exhausted_bad_quality = FALSE;
      self->match_decided = FALSE;
      self->match_verify_result = FPI_MATCH_FAIL;
      self->match_identify_match = NULL;
      g_clear_pointer (&self->match_updated, g_variant_unref);
      self->cycle_should_continue = TRUE;
    }

  fp_dbg ("capture cycle stopped for system suspend");
  self->operation_active = FALSE;
  self->suspend_state = GOODIX5125_SUSPEND_PARKED;
  self->resume_finish = finish;
  fpi_device_suspend_complete (FP_DEVICE (self), NULL);
  return TRUE;
}

static void
finger_removed (FpDeviceGoodix5125 *self)
{
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);

  /* a decision taken while a suspend is pending is dropped */
  if (!self->cycle_error &&
      (self->cycle_should_continue || cycle_may_park (self)))
    {
      self->cycle_should_continue = FALSE;
      begin_wait_for_finger (self);
      return;
    }

  finish_action (self);
}

static void lift_poll (FpDeviceGoodix5125 *self);

static void
lift_poll_timeout_cb (FpDevice *device,
                      gpointer  user_data)
{
  lift_poll (FPI_DEVICE_GOODIX5125 (device));
}

static void
lift_poll_cb (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) response = NULL;
  GError *error = NULL;
  gboolean finger_off = FALSE;
  const guint8 *data;
  gsize length = 0;

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (response)
    {
      data = g_bytes_get_data (response, &length);
      goodix5125_fdt_reply_is_finger_off (data, length, &finger_off, &error);
    }
  if (error)
    {
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }

  if (finger_off)
    {
      finger_removed (self);
      return;
    }
  self->operation_active = TRUE;
  fpi_device_add_timeout (FP_DEVICE (self), FDT_LIFT_POLL_MS,
                          lift_poll_timeout_cb, NULL, NULL);
}

static void
lift_poll (FpDeviceGoodix5125 *self)
{
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { FDT_MODE_ARM, 0x01 };

  if (cycle_park (self, FALSE))
    return;

  memcpy (payload + 2, self->lift_base, GOODIX5125_FDT_BASE_SIZE);
  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_MODE,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    COMMAND_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    lift_poll_cb, self);
}

static void
enroll_frame (FpDeviceGoodix5125 *self,
              const guint16      *frame,
              const guint16      *extra)
{
  Goodix5125EnrollStep step;
  GError *error = NULL;

  if (!goodix5125_algo_enroll_add (self->algo, frame, extra, &step, &error))
    {
      set_cycle_error (self, error);
      return;
    }

  if (step.result.accepted || step.retry)
    fpi_device_enroll_progress (FP_DEVICE (self), step.stage, NULL,
                                g_steal_pointer (&step.retry));
  goodix5125_enroll_step_clear (&step);
  self->enroll_complete = step.complete;
  self->cycle_should_continue = !step.complete;
}

static void
variant_unref0 (gpointer data)
{
  if (data)
    g_variant_unref (data);
}

static void
match_frame (FpDeviceGoodix5125 *self,
             const guint16      *frame)
{
  g_autoptr(GPtrArray) prints = g_ptr_array_new_with_free_func (variant_unref0);
  GPtrArray *candidates = self->match_gallery;
  FpPrint *verify_print[1] = { self->match_template };
  Goodix5125MatchStep step;
  GError *error = NULL;
  guint n;

  n = self->action_kind == GOODIX5125_ACTION_VERIFY ? 1 : candidates->len;
  for (guint i = 0; i < n; i++)
    {
      FpPrint *print = self->action_kind == GOODIX5125_ACTION_VERIFY ?
                       verify_print[0] : g_ptr_array_index (candidates, i);
      GVariant *data = NULL;

      g_object_get (G_OBJECT (print), "fpi-data", &data, NULL);
      g_ptr_array_add (prints, data);   /* may be NULL: skipped */
    }

  if (!goodix5125_algo_match (self->algo, frame,
                              (GVariant *const *) prints->pdata, prints->len,
                              &step, &error))
    {
      set_cycle_error (self, error);
      return;
    }

  if (step.retry)
    {
      self->match_attempt++;
      if (self->match_attempt < GOODIX5125_VERIFY_CAPTURE_ATTEMPTS)
        self->cycle_should_continue = TRUE;
      else
        self->match_exhausted_bad_quality = TRUE;
      goodix5125_match_step_clear (&step);
      return;
    }

  if (self->action_kind == GOODIX5125_ACTION_VERIFY)
    self->match_verify_result = step.matched >= 0 ? FPI_MATCH_SUCCESS :
                                FPI_MATCH_FAIL;
  else if (step.matched >= 0)
    self->match_identify_match = g_ptr_array_index (candidates, step.matched);
  /* stored: the next match of the same print picks it up from the store,
   * the print itself keeps its data (and so its store key) */
  if (!step.stored)
    self->match_updated = g_steal_pointer (&step.updated);
  goodix5125_match_step_clear (&step);
  self->match_decided = TRUE;
  self->cycle_should_continue = FALSE;
}

static void
capture_ready_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) encrypted = NULL;
  g_autoptr(GBytes) packed = NULL;
  guint16 raw[GOODIX5125_IMAGE_N_PIXELS];
  GError *error = NULL;
  gsize length = 0;
  const guint8 *data = NULL;

  encrypted = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                             result, &error);
  self->operation_active = FALSE;
  if (encrypted)
    packed = decrypt_packed (self, encrypted, &error);
  if (packed)
    data = g_bytes_get_data (packed, &length);
  /* 80 x 64 transport frame -> 64 rows x 80 columns (ChicagoHS input) */
  if (!packed ||
      !goodix5125_image_decode_transposed (data, length, raw,
                                           GOODIX5125_IMAGE_N_PIXELS, &error))
    {
      set_cycle_error (self, error);
      lift_poll (self);
      return;
    }

  if (self->action_kind == GOODIX5125_ACTION_ENROLL)
    {
      if (self->enroll_pair && !self->have_first_frame)
        {
          /* Second frame of the same touch */
          memcpy (self->first_frame, raw, sizeof (self->first_frame));
          self->have_first_frame = TRUE;
          begin_capture (self);
          return;
        }
      if (self->have_first_frame)
        enroll_frame (self, self->first_frame, raw);
      else
        enroll_frame (self, raw, NULL);
      self->have_first_frame = FALSE;
    }
  else
    {
      match_frame (self, raw);
    }

  lift_poll (self);
}

static void
begin_capture (FpDeviceGoodix5125 *self)
{
  static const guint8 payload[] = { 0x01, 0x00 };

  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_GET_IMAGE,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_TLS,
    COMMAND_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    capture_ready_cb, self);
}

static void
fdt_down_ready_cb (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) response = NULL;
  GError *error = NULL;

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  g_clear_object (&self->wait_cancellable);
  if (!response)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          cycle_park (self, FALSE))
        {
          g_error_free (error);
          return;
        }
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }

  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
  begin_capture (self);
}

static void
query_before_fdt_cb (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) response = NULL;
  GError *error = NULL;
  gsize length = 0;
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { FDT_MODE_DOWN, 0x01 };

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (!response)
    {
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }

  g_bytes_get_data (response, &length);
  if (length < 14)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix MCU-state reply is shorter than 14 bytes");
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }

  if (cycle_park (self, FALSE))
    return;
  /* goodix5125_cancel () ends the wait only once it is running */
  if (g_cancellable_set_error_if_cancelled (
        fpi_device_get_cancellable (FP_DEVICE (self)), &error))
    {
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }

  memcpy (payload + 2, self->fdt_base, GOODIX5125_FDT_BASE_SIZE);
  self->operation_active = TRUE;
  self->wait_cancellable = g_cancellable_new ();
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_DOWN,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    0, self->wait_cancellable, fdt_down_ready_cb, self);
}

static void
query_state_async (FpDeviceGoodix5125 *self)
{
  static const guint8 payload[] = { 0x55 };

  if (cycle_park (self, FALSE))
    return;

  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_QUERY_STATE,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    COMMAND_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    query_before_fdt_cb, self);
}

static void
probe_fdt_down_cb (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) response = NULL;
  GError *error = NULL;
  gsize length = 0;
  const guint8 *data;

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (!response)
    {
      /* No reply is the normal case: the probe only arms detection. */
      if (!g_error_matches (error, G_USB_DEVICE_ERROR,
                            G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          set_cycle_error (self, error);
          finish_action (self);
          return;
        }
      g_clear_error (&error);
    }
  else
    {
      data = g_bytes_get_data (response, &length);
      if (!goodix5125_fdt_down_base_from_reply (data, length, self->fdt_base,
                                                &error))
        {
          fp_dbg ("ignoring the FDT-down probe reply: %s", error->message);
          g_clear_error (&error);
        }
    }
  query_state_async (self);
}

static void
arm_fdt_cb (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);

  g_autoptr(GBytes) response = NULL;
  GError *error = NULL;
  gsize length = 0;
  const guint8 *data;
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { FDT_MODE_DOWN, 0x01 };

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (response)
    {
      data = g_bytes_get_data (response, &length);
      goodix5125_fdt_down_base_from_reply (data, length, self->fdt_base,
                                           &error);
    }
  if (error)
    {
      set_cycle_error (self, error);
      finish_action (self);
      return;
    }
  /* the no-finger thresholds of this arm step detect the lift later */
  memcpy (self->lift_base, self->fdt_base, sizeof (self->lift_base));

  if (cycle_park (self, FALSE))
    return;

  memcpy (payload + 2, self->fdt_base, GOODIX5125_FDT_BASE_SIZE);
  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_DOWN,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    FDT_PROBE_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    probe_fdt_down_cb, self);
}

static void
begin_wait_for_finger (FpDeviceGoodix5125 *self)
{
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { FDT_MODE_ARM, 0x01 };

  if (cycle_park (self, FALSE))
    return;

  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED);
  self->operation_active = TRUE;
  memcpy (payload + 2, self->fdt_base, GOODIX5125_FDT_BASE_SIZE);
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_MODE,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    COMMAND_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    arm_fdt_cb, self);
}

/* ---------------------------------------------------------------------
 * Action completion
 * --------------------------------------------------------------------- */

static void
finish_enroll (FpDeviceGoodix5125 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  GError *error = g_steal_pointer (&self->cycle_error);
  FpPrint *print = NULL;
  GVariant *data = NULL;

  if (!error && !self->enroll_complete)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                      "enrolment stopped before completion");
  if (!error)
    data = goodix5125_algo_enroll_finish (self->algo, &error);
  if (error)
    {
      reset_action_state (self);
      save_algo_state (self);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  fpi_device_get_enroll_data (dev, &print);
  print = g_object_ref (print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (G_OBJECT (print), "fpi-data", data, NULL);
  g_variant_unref (data);
  fp_dbg ("openchicago enrolment complete");

  reset_action_state (self);
  save_algo_state (self);
  fpi_device_enroll_complete (dev, print, NULL);
}

static void
finish_match (FpDeviceGoodix5125 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  gboolean is_verify = self->action_kind == GOODIX5125_ACTION_VERIFY;
  FpPrint *matched_print;

  save_algo_state (self);

  if (self->cycle_error)
    {
      GError *error = g_steal_pointer (&self->cycle_error);

      reset_action_state (self);
      if (is_verify)
        fpi_device_verify_complete (dev, error);
      else
        fpi_device_identify_complete (dev, error);
      return;
    }

  if (self->match_exhausted_bad_quality && !self->match_decided)
    {
      GError *retry = fpi_device_retry_new_msg (
        FP_DEVICE_RETRY_GENERAL,
        "Fingerprint image quality is too low; try again.");

      if (is_verify)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, retry);
      else
        fpi_device_identify_report (dev, NULL, NULL, retry);
      reset_action_state (self);
      if (is_verify)
        fpi_device_verify_complete (dev, NULL);
      else
        fpi_device_identify_complete (dev, NULL);
      return;
    }

  matched_print = is_verify ?
                  (self->match_verify_result == FPI_MATCH_SUCCESS ?
                   self->match_template : NULL) :
                  self->match_identify_match;

  /* Template study without the learned-template store (it could not be
  * written): the learned template replaces the print data in memory only.
  * libfprint has no "print updated" signal; a caller keeping the print (or
  * re-saving it after the match) gets the update, fprintd does not. */
  if (matched_print && self->match_updated)
    {
      g_object_set (G_OBJECT (matched_print), "fpi-data", self->match_updated,
                    NULL);
      fp_dbg ("template study updated the matched print");
    }

  if (is_verify)
    {
      fpi_device_verify_report (dev, self->match_verify_result, NULL, NULL);
      reset_action_state (self);
      fpi_device_verify_complete (dev, NULL);
    }
  else
    {
      fpi_device_identify_report (dev, matched_print, NULL, NULL);
      reset_action_state (self);
      fpi_device_identify_complete (dev, NULL);
    }
}

static void
finish_action (FpDeviceGoodix5125 *self)
{
  gboolean complete_suspend;

  /* the action ends after resume, see cycle_park () */
  if (cycle_park (self, TRUE))
    return;

  /* A failed or cancelled cycle can leave an image record of the sensor
   * unread (it is skipped later and the TLS sequence numbers no longer
   * match) or the session failed: initialize again for the next action. */
  if (self->cycle_error)
    {
      self->initialized = FALSE;
      g_clear_pointer (&self->tls, goodix5125_tls_free);
    }

  /* cancelled or removed: a pending suspend completes with the action */
  complete_suspend = self->suspend_state == GOODIX5125_SUSPEND_PENDING;
  self->suspend_state = GOODIX5125_SUSPEND_NONE;

  switch (self->action_kind)
    {
    case GOODIX5125_ACTION_ENROLL:
      finish_enroll (self);
      break;

    case GOODIX5125_ACTION_VERIFY:
    case GOODIX5125_ACTION_IDENTIFY:
      finish_match (self);
      break;

    case GOODIX5125_ACTION_NONE:
    default:
      g_assert_not_reached ();
    }

  if (complete_suspend)
    fpi_device_suspend_complete (FP_DEVICE (self), NULL);
}

/* ---------------------------------------------------------------------
 * FpDevice vtable
 * --------------------------------------------------------------------- */

/* fprintd's state directory when usable, else the user's state directory. */
static gchar *
state_directory (void)
{
  const gchar *env = g_getenv (GOODIX5125_STATE_DIR_ENV);

  if (env && *env)
    return g_strdup (env);
  if (g_mkdir_with_parents (GOODIX5125_STATE_DEFAULT_DIR, 0700) == 0 &&
      g_access (GOODIX5125_STATE_DEFAULT_DIR, R_OK | W_OK | X_OK) == 0)
    return g_strdup (GOODIX5125_STATE_DEFAULT_DIR);
  /* $XDG_STATE_HOME (g_get_user_state_dir () needs GLib 2.72) */
  env = g_getenv ("XDG_STATE_HOME");
  if (env && g_path_is_absolute (env))
    return g_build_filename (env, "libfprint", "goodix5125", NULL);
  return g_build_filename (g_get_home_dir (), ".local", "state", "libfprint",
                           "goodix5125", NULL);
}

static gchar *
state_file (const gchar *directory,
            const gchar *env,
            const gchar *name)
{
  const gchar *value = env ? g_getenv (env) : NULL;

  if (value && *value)
    return g_strdup (value);
  return g_build_filename (directory, name, NULL);
}

static void
remove_directory (const gchar *path)
{
  g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
  const gchar *name;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree gchar *child = g_build_filename (path, name, NULL);

      if (g_file_test (child, G_FILE_TEST_IS_DIR) &&
          !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        remove_directory (child);
      else
        g_unlink (child);
    }
  g_rmdir (path);
}

static void
clear_emulation_dir (FpDeviceGoodix5125 *self)
{
  if (!self->emulation_dir)
    return;
  remove_directory (self->emulation_dir);
  g_clear_pointer (&self->emulation_dir, g_free);
}

static void
goodix5125_probe (FpDevice *device)
{
  fpi_device_probe_complete (device, NULL, NULL, NULL);
}

static void
goodix5125_open (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  gboolean emulation = fpi_device_emulation_mode_enabled (device);
  g_autofree gchar *directory = NULL;
  g_autofree gchar *state_path = NULL;
  g_autofree gchar *learned_dir = NULL;
  const gchar *pair = NULL;
  GError *error = NULL;

  /* Device emulation (umockdev tests) starts from an empty host state in a
   * temporary directory, removed on close, and ignores the environment. */
  clear_emulation_dir (self);
  if (emulation)
    {
      self->emulation_dir = g_dir_make_tmp ("libfprint-goodix5125-XXXXXX",
                                            &error);
      if (!self->emulation_dir)
        {
          fpi_device_open_complete (device, error);
          return;
        }
      directory = g_strdup (self->emulation_dir);
    }
  else
    {
      directory = state_directory ();
      pair = g_getenv (GOODIX5125_ENROLL_PAIR_ENV);
    }

  g_free (self->psk_path);
  self->psk_path = state_file (directory,
                               emulation ? NULL : GOODIX5125_PSK_FILE_ENV,
                               GOODIX5125_PSK_FILE_NAME);
  g_free (self->calidata_path);
  self->calidata_path = state_file (directory,
                                    emulation ? NULL :
                                    GOODIX5125_CHICAGO_CALIBRATION_ENV,
                                    GOODIX5125_CHICAGO_CALIBRATION_FILE_NAME);
  state_path = state_file (directory,
                           emulation ? NULL : GOODIX5125_STATE_FILE_ENV,
                           GOODIX5125_STATE_FILE_NAME);
  learned_dir = state_file (directory,
                            emulation ? NULL : GOODIX5125_LEARNED_DIR_ENV,
                            GOODIX5125_LEARNED_DIR_NAME);
  self->enroll_pair = !(pair && g_strcmp0 (pair, "0") == 0);
  fp_dbg ("state %s, learned templates %s, PSK %s, "
          "enrolment pairs %s", state_path, learned_dir, self->psk_path,
          self->enroll_pair ? "on" : "off");

  g_clear_pointer (&self->algo, goodix5125_algo_free);
  self->algo = goodix5125_algo_new (state_path);
  goodix5125_algo_set_learned_store (self->algo, learned_dir, 0, 0);
  if (!goodix5125_algo_load_state (self->algo, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  if (!g_usb_device_claim_interface (
        fpi_device_get_usb_device (device),
        GOODIX5125_USB_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  fpi_device_set_nr_enroll_stages (device, OC_ENGINE_STAGES);
  fpi_device_open_complete (device, NULL);
}

static void
goodix5125_close (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  GError *error = NULL;

  /* libfprint closes only between actions, and the device may be gone:
   * closing always completes. */
  if (self->operation_active)
    fp_warn ("closing with a Goodix command in flight");
  self->operation_active = FALSE;
  self->suspend_state = GOODIX5125_SUSPEND_NONE;
  save_algo_state (self);
  self->initialized = FALSE;
  g_clear_pointer (&self->tls, goodix5125_tls_free);
  g_clear_pointer (&self->algo, goodix5125_algo_free);
  clear_emulation_dir (self);
  g_usb_device_release_interface (
    fpi_device_get_usb_device (device),
    GOODIX5125_USB_INTERFACE, 0, &error);
  fpi_device_close_complete (device, error);
}

static void
goodix5125_enroll (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  GError *error = NULL;

  if (!ensure_activated (self, &error) ||
      !goodix5125_algo_enroll_begin (self->algo, OC_ENROLL_ENGINE,
                                     OC_ENGINE_STAGES, &error))
    {
      fpi_device_enroll_complete (device, NULL, error);
      return;
    }

  reset_action_state (self);
  self->action_kind = GOODIX5125_ACTION_ENROLL;
  begin_wait_for_finger (self);
}

static void
goodix5125_verify (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  GError *error = NULL;
  FpPrint *print = NULL;

  if (!ensure_activated (self, &error))
    {
      fpi_device_verify_complete (device, error);
      return;
    }

  fpi_device_get_verify_data (device, &print);

  reset_action_state (self);
  self->action_kind = GOODIX5125_ACTION_VERIFY;
  self->match_template = g_object_ref (print);
  begin_wait_for_finger (self);
}

static void
goodix5125_identify (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  GError *error = NULL;
  GPtrArray *gallery = NULL;

  if (!ensure_activated (self, &error))
    {
      fpi_device_identify_complete (device, error);
      return;
    }

  fpi_device_get_identify_data (device, &gallery);

  /* An empty gallery cannot match: no need to wait for a finger */
  if (!gallery || gallery->len == 0)
    {
      fpi_device_identify_report (device, NULL, NULL, NULL);
      fpi_device_identify_complete (device, NULL);
      return;
    }

  reset_action_state (self);
  self->action_kind = GOODIX5125_ACTION_IDENTIFY;
  self->match_gallery = g_ptr_array_ref (gallery);
  begin_wait_for_finger (self);
}

/* fprintd releases a device whose action fails, and libfprint refuses to
 * close a suspended device: verify and identify are kept over a suspend
 * (the finger wait is ended here, any other step stops when its command
 * completes, see cycle_park ()).  An enrolment cannot drop a touch it has
 * already added and is cancelled by libfprint on this error. */
static void
goodix5125_suspend (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  FpiDeviceAction action = fpi_device_get_current_action (device);

  if (action != FPI_DEVICE_ACTION_VERIFY &&
      action != FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_suspend_complete (
        device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  /* completed, the result is not returned yet */
  if (self->action_kind == GOODIX5125_ACTION_NONE)
    {
      fpi_device_suspend_complete (device, NULL);
      return;
    }

  self->suspend_state = GOODIX5125_SUSPEND_PENDING;
  if (self->wait_cancellable)
    g_cancellable_cancel (self->wait_cancellable);
}

static void
goodix5125_resume (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  gboolean parked = self->suspend_state == GOODIX5125_SUSPEND_PARKED;

  self->suspend_state = GOODIX5125_SUSPEND_NONE;
  fpi_device_resume_complete (device, NULL);
  if (!parked)
    return;

  if (self->resume_finish)
    finish_action (self);
  else
    lift_poll (self);
}

/* Other commands of the cycle use the action cancellable directly. */
static void
goodix5125_cancel (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);

  if (self->wait_cancellable)
    g_cancellable_cancel (self->wait_cancellable);

  /* nothing is in flight that could fail */
  if (self->suspend_state == GOODIX5125_SUSPEND_PARKED)
    {
      self->suspend_state = GOODIX5125_SUSPEND_NONE;
      set_cycle_error (self,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                            "Operation was cancelled"));
      finish_action (self);
    }
}

/* Transfers to a removed device fail on their own, but the finger wait may
 * outlive it and nothing is in flight while suspended: end both, the action
 * fails with FP_DEVICE_ERROR_REMOVED. */
static void
removed_notify_cb (FpDeviceGoodix5125 *self,
                   GParamSpec         *pspec,
                   gpointer            user_data)
{
  self->removed = TRUE;
  if (self->wait_cancellable)
    g_cancellable_cancel (self->wait_cancellable);

  if (self->suspend_state == GOODIX5125_SUSPEND_PARKED)
    {
      self->suspend_state = GOODIX5125_SUSPEND_NONE;
      set_cycle_error (self, fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
      finish_action (self);
    }
}

static void
fpi_device_goodix5125_init (FpDeviceGoodix5125 *self)
{
  g_signal_connect (self, "notify::removed",
                    G_CALLBACK (removed_notify_cb), NULL);
}

static void
fpi_device_goodix5125_finalize (GObject *object)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (object);

  reset_action_state (self);
  g_clear_object (&self->wait_cancellable);
  g_clear_pointer (&self->tls, goodix5125_tls_free);
  g_clear_pointer (&self->algo, goodix5125_algo_free);
  g_clear_pointer (&self->psk_path, g_free);
  g_clear_pointer (&self->calidata_path, g_free);
  clear_emulation_dir (self);

  G_OBJECT_CLASS (fpi_device_goodix5125_parent_class)->finalize (object);
}

static void
fpi_device_goodix5125_class_init (FpDeviceGoodix5125Class *klass)
{
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);

  device_class->id = FP_COMPONENT;
  device_class->full_name = "Goodix 5125 ChicagoHS";
  device_class->type = FP_DEVICE_TYPE_USB;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;
  device_class->id_table = id_table;
  device_class->nr_enroll_stages = OC_ENGINE_STAGES;
  /* The MCU detects the finger (FDT) and the host captures nothing while
   * waiting, so the sensor can stay armed indefinitely. */
  device_class->temp_hot_seconds = -1;

  device_class->probe = goodix5125_probe;
  device_class->open = goodix5125_open;
  device_class->close = goodix5125_close;
  device_class->enroll = goodix5125_enroll;
  device_class->verify = goodix5125_verify;
  device_class->identify = goodix5125_identify;
  device_class->cancel = goodix5125_cancel;
  device_class->suspend = goodix5125_suspend;
  device_class->resume = goodix5125_resume;

  fpi_device_class_auto_initialize_features (device_class);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodix5125_finalize;
}
