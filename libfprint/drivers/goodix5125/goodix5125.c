/*
 * Goodix 27c6:5125 fingerprint driver
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
#include "chicago/goodix-chicago-calibration.h"
#include "chicago/goodix-chicago-runtime.h"
#include "chicago/goodix-chicago-template.h"

#include <errno.h>

#include <glib/gstdio.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#define COMMAND_TIMEOUT_MS 5000
#define NOP_TIMEOUT_MS 100
#define TLS_PLAINTEXT_SIZE 7693
#define TLS_HEADER_SIZE 8
#define TLS_TRAILER_SIZE 5
#define GOODIX5125_USB_INTERFACE 1

#define GOODIX5125_PSK_FILE_ENV "GOODIX5125_PSK_FILE"
#define GOODIX5125_PSK_DEFAULT_PATH "/var/lib/fprint/goodix5125/psk"
#define GOODIX5125_PROVISION_PSK_ENV "GOODIX5125_PROVISION_PSK"

/* Matching is delegated to the vendored ChicagoHS backend (see
 * drivers/goodix5125/chicago/), which implements the same official
 * AlgoChicago algorithm family this sensor's profile-12 selector uses.
 * A rejected/poor-quality capture may be retried a few times on the same
 * verify()/identify() call before giving up. */
#define GOODIX5125_VERIFY_CAPTURE_ATTEMPTS 3

#define GOODIX5125_CHICAGO_CALIBRATION_ENV \
  "GOODIX5125_CHICAGO_CALIBRATION_FILE"
#define GOODIX5125_CHICAGO_CALIBRATION_DEFAULT_PATH \
  "/var/lib/fprint/goodix5125/chicago-calibration.dat"

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

struct _FpDeviceGoodix5125
{
  FpDevice                   parent;

  Goodix5125Tls             *tls;
  Goodix5125OtpInfo          otp_info;
  guint8                     fdt_base[GOODIX5125_FDT_BASE_SIZE];
  guint8                     fdt_up_base[GOODIX5125_FDT_BASE_SIZE];
  guint16                    cached_image_base[GOODIX5125_IMAGE_N_PIXELS];
  guint8                     sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN];
  GBytes                    *chicago_calibration;
  GoodixChicagoPreprocessor *chicago_preprocessor;
  gboolean                   operation_active;
  gboolean                   initialized;

  /* Current enroll/verify/identify action state. */
  Goodix5125Action                    action_kind;

  GoodixChicagoEnrollment            *enroll_accumulator;
  GoodixChicagoEngineEnrollmentPolicy enroll_policy;
  GoodixChicagoRuntimeProbe          *enroll_deferred_probe;

  FpPrint                            *match_template;
  GPtrArray                          *match_gallery;
  guint                               match_attempt;
  gboolean                            match_exhausted_bad_quality;
  gboolean                            match_decided;
  GoodixChicagoRuntimeProbe          *match_probe;
  GoodixChicagoMatchTemplateResult    match_result;
  FpiMatchResult                      match_verify_result;
  FpPrint                            *match_identify_match;

  gboolean                            cycle_should_continue;
  GError                             *cycle_error;
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
  const gchar *path = g_getenv (GOODIX5125_PSK_FILE_ENV);
  const gchar *provision = g_getenv (GOODIX5125_PROVISION_PSK_ENV);
  Goodix5125PskState state;
  guint8 wb[GOODIX5125_WB_SIZE];
  guint8 expected_hash[GOODIX5125_PSK_SIZE];
  guint8 sensor_hash[GOODIX5125_PSK_SIZE];
  gboolean sensor_has_key = FALSE;
  gboolean authorized = provision &&
                        g_ascii_strcasecmp (provision, "random") == 0;
  gboolean needs_provision = FALSE;
  gboolean success = FALSE;

  if (!path || !*path)
    path = GOODIX5125_PSK_DEFAULT_PATH;
  if (provision && *provision && !authorized)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s accepts only the value random",
                   GOODIX5125_PROVISION_PSK_ENV);
      goto out;
    }

  state = goodix5125_pairing_load_psk (path, psk, error);
  if (state == GOODIX5125_PSK_STATE_ERROR ||
      !read_psk_hash (self, &sensor_has_key, sensor_hash, error))
    goto out;

  if (state == GOODIX5125_PSK_STATE_MISSING)
    {
      if (sensor_has_key && !authorized)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                       "The Goodix sensor is paired to another host, but %s "
                       "is missing; import its PSK or set %s=random once to "
                       "replace the existing pairing",
                       path, GOODIX5125_PROVISION_PSK_ENV);
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
      if (!sensor_has_key)
        {
          needs_provision = TRUE;
        }
      else if (CRYPTO_memcmp (expected_hash, sensor_hash,
                              GOODIX5125_PSK_SIZE) != 0)
        {
          if (!authorized)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "The persisted Goodix PSK does not match the "
                           "sensor; set %s=random once to restore it",
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
ensure_parent_directory (const gchar *path,
                         GError     **error)
{
  g_autofree gchar *directory = g_path_get_dirname (path);

  if (g_mkdir_with_parents (directory, 0700) == 0)
    return TRUE;

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
               "Cannot create Goodix state directory %s: %s",
               directory, g_strerror (errno));
  return FALSE;
}

/* Load the persisted Chicago state bound to this physical sensor, then
* replace its ImageBase plane with the fresh no-finger frame captured during
* this activation. On first use, generate the state entirely from that frame.
* A file belonging to a different OTP identity is never overwritten. */
static gboolean
ensure_chicago_calibration (FpDeviceGoodix5125 *self,
                            GError            **error)
{
  const gchar *path = g_getenv (GOODIX5125_CHICAGO_CALIBRATION_ENV);

  g_autoptr(GError) load_error = NULL;
  g_autoptr(GBytes) rebased = NULL;

  if (!path || !*path)
    path = GOODIX5125_CHICAGO_CALIBRATION_DEFAULT_PATH;

  g_clear_pointer (&self->chicago_calibration, g_bytes_unref);
  self->chicago_calibration = goodix_chicago_calibration_load (
    path, self->sensor_id, &load_error);
  if (self->chicago_calibration)
    {
      rebased = goodix_chicago_calibration_rebase (
        self->chicago_calibration, self->cached_image_base, error);
      if (!rebased)
        return FALSE;
      g_clear_pointer (&self->chicago_calibration, g_bytes_unref);
      self->chicago_calibration = g_steal_pointer (&rebased);
      if (!ensure_parent_directory (path, error) ||
          !goodix_chicago_calibration_save (path, self->sensor_id,
                                            self->chicago_calibration, error))
        return FALSE;
      fp_dbg ("loaded and rebased %" G_GSIZE_FORMAT "-byte sensor-bound "
              "Chicago calibration from %s",
              g_bytes_get_size (self->chicago_calibration), path);
      return TRUE;
    }

  if (!g_error_matches (load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
      !g_error_matches (load_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_prefixed_error (
        error, g_steal_pointer (&load_error),
        "refusing to replace invalid Chicago calibration at %s: ", path);
      return FALSE;
    }

  self->chicago_calibration = goodix_chicago_calibration_generate (
    self->cached_image_base);
  if (!self->chicago_calibration)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "could not generate a first-run Chicago "
                           "calibration");
      return FALSE;
    }

  if (!ensure_parent_directory (path, error) ||
      !goodix_chicago_calibration_save (path, self->sensor_id,
                                        self->chicago_calibration, error))
    return FALSE;

  fp_info ("generated and persisted first-run Chicago calibration at %s",
           path);
  return TRUE;
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
  self->tls = goodix5125_tls_new (psk, 32, error);
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

  if (!ensure_chicago_calibration (self, error))
    goto out;
  g_clear_pointer (&self->chicago_preprocessor,
                   goodix_chicago_preprocessor_free);
  self->chicago_preprocessor = goodix_chicago_preprocessor_new (
    self->chicago_calibration, self->cached_image_base, error);
  if (!self->chicago_preprocessor)
    goto out;

  fp_dbg ("live no-finger ImageBase captured and Chicago state prepared");

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
 * Shared async capture cycle: wait for finger -> capture+decrypt -> run
 * through the Chicago preprocessor/feature extractor -> wait for finger
 * removal. What happens with a probe (accumulate for enrollment, or match
 * against a template/gallery) is decided in capture_ready_cb() based on
 * self->action_kind; the outcome only takes effect once the finger-off
 * wait below completes, so the sensor is always left in a known,
 * no-finger state between actions.
 * --------------------------------------------------------------------- */

static void begin_wait_for_finger (FpDeviceGoodix5125 *self);

static void finish_action (FpDeviceGoodix5125 *self);

static void
reset_action_state (FpDeviceGoodix5125 *self)
{
  self->action_kind = GOODIX5125_ACTION_NONE;
  g_clear_pointer (&self->enroll_accumulator, goodix_chicago_enrollment_free);
  g_clear_pointer (&self->enroll_deferred_probe,
                   goodix_chicago_runtime_probe_free);
  g_clear_object (&self->match_template);
  g_clear_pointer (&self->match_gallery, g_ptr_array_unref);
  self->match_attempt = 0;
  self->match_exhausted_bad_quality = FALSE;
  self->match_decided = FALSE;
  g_clear_pointer (&self->match_probe, goodix_chicago_runtime_probe_free);
  memset (&self->match_result, 0, sizeof (self->match_result));
  self->match_identify_match = NULL;
  self->cycle_should_continue = FALSE;
  g_clear_error (&self->cycle_error);
}

static void
fdt_up_ready_cb (GObject      *source_object,
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
      if (!self->cycle_error)
        self->cycle_error = error;
      else
        g_clear_error (&error);
      finish_action (self);
      return;
    }

  data = g_bytes_get_data (response, &length);
  if (!goodix5125_fdt_down_base_from_reply (
        data, length, self->fdt_base, &error))
    {
      if (!self->cycle_error)
        self->cycle_error = error;
      else
        g_clear_error (&error);
      finish_action (self);
      return;
    }

  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);

  if (self->cycle_should_continue && !self->cycle_error)
    {
      self->cycle_should_continue = FALSE;
      begin_wait_for_finger (self);
      return;
    }

  finish_action (self);
}

static void
wait_for_finger_removal (FpDeviceGoodix5125 *self)
{
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { 0x0a, 0x01 };

  memcpy (payload + 2, self->fdt_up_base, GOODIX5125_FDT_BASE_SIZE);
  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_UP,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    0, fpi_device_get_cancellable (FP_DEVICE (self)),
    fdt_up_ready_cb, self);
}

static void
report_enroll_progress (FpDeviceGoodix5125 *self,
                        GError             *retry_error)
{
  fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_policy.used,
                              NULL, retry_error);
}

static const gchar *
enroll_position_message (guint position_reject)
{
  switch (position_reject)
    {
    case 1:
      return "Finger is too high; move it lower.";

    case 2:
      return "Finger is too low; move it higher.";

    case 3:
      return "Finger is too far left; move it right.";

    default:
      return "Finger is too far right; move it left.";
    }
}

/* Accumulate one accepted capture into the in-progress enrollment,
 * following the official enrollment policy verbatim (including its
 * deferred-sample handling for directional-tip images). */
static void
enroll_accumulate (FpDeviceGoodix5125        *self,
                   GoodixChicagoRuntimeProbe *probe)
{
  const GoodixChicagoSubtemplateView *view =
    goodix_chicago_runtime_probe_get_view (probe);
  GoodixChicagoEnrollmentResult result = { 0, };
  GError *error = NULL;
  gboolean ok;
  gboolean policy_accepted;
  guint position_reject = 0;

  if (goodix_chicago_enrollment_get_count (self->enroll_accumulator) == 0)
    {
      ok = goodix_chicago_enrollment_insert_first (
        self->enroll_accumulator, view->records, view->record_count,
        view->active_count, view->quality, view->coverage,
        view->metric_data, &result, &error);
    }
  else
    {
      ok = goodix_chicago_enrollment_insert_next (
        self->enroll_accumulator, view->records, view->record_count,
        view->active_count, view->quality, view->coverage,
        view->metric_data, &result, &error);
    }

  if (!ok)
    {
      goodix_chicago_runtime_probe_free (probe);
      self->cycle_error = error;
      return;
    }

  policy_accepted = goodix_chicago_engine_enrollment_policy_accept (
    &self->enroll_policy, result.position_x, result.position_y,
    &position_reject);

  if (self->enroll_policy.defer_current_sample)
    {
      if (self->enroll_deferred_probe != NULL ||
          !goodix_chicago_enrollment_drop_last (self->enroll_accumulator,
                                                &error))
        {
          if (!error)
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "duplicate deferred Chicago enrollment "
                                 "sample");
          goodix_chicago_runtime_probe_free (probe);
          self->cycle_error = error;
          return;
        }
      self->enroll_deferred_probe = probe;
      probe = NULL;
    }

  if (self->enroll_policy.restore_deferred_sample)
    {
      GoodixChicagoEnrollmentResult deferred_result = { 0, };
      const GoodixChicagoSubtemplateView *deferred_view;

      if (self->enroll_deferred_probe == NULL)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "missing deferred Chicago enrollment sample");
          g_clear_pointer (&probe, goodix_chicago_runtime_probe_free);
          self->cycle_error = error;
          return;
        }
      deferred_view = goodix_chicago_runtime_probe_get_view (
        self->enroll_deferred_probe);
      if (!goodix_chicago_enrollment_insert_next (
            self->enroll_accumulator, deferred_view->records,
            deferred_view->record_count, deferred_view->active_count,
            deferred_view->quality, deferred_view->coverage,
            deferred_view->metric_data, &deferred_result, &error))
        {
          g_clear_pointer (&probe, goodix_chicago_runtime_probe_free);
          self->cycle_error = error;
          return;
        }
      g_clear_pointer (&self->enroll_deferred_probe,
                       goodix_chicago_runtime_probe_free);
    }

  g_clear_pointer (&probe, goodix_chicago_runtime_probe_free);

  if (!policy_accepted)
    {
      report_enroll_progress (
        self, fpi_device_retry_new_msg (
          FP_DEVICE_RETRY_CENTER_FINGER, "%s",
          enroll_position_message (position_reject)));
    }
  else
    {
      report_enroll_progress (self, NULL);
    }

  self->cycle_should_continue =
    !goodix_chicago_engine_enrollment_policy_complete (&self->enroll_policy);
}

static void
match_against_print_data (FpDeviceGoodix5125               *self,
                          const GoodixChicagoRuntimeProbe  *probe,
                          FpPrint                          *print,
                          GoodixChicagoMatchTemplateResult *out_result,
                          GError                          **error)
{
  g_autoptr(GVariant) data = NULL;

  g_object_get (G_OBJECT (print), "fpi-data", &data, NULL);
  goodix_chicago_runtime_match_print_data (
    probe, data, self->sensor_id, self->chicago_calibration, out_result,
    error);
}

static void
match_decide (FpDeviceGoodix5125        *self,
              GoodixChicagoRuntimeProbe *probe)
{
  GError *error = NULL;

  if (self->action_kind == GOODIX5125_ACTION_VERIFY)
    {
      GoodixChicagoMatchTemplateResult result = { 0, };

      match_against_print_data (self, probe, self->match_template, &result,
                                &error);
      if (error)
        {
          goodix_chicago_runtime_probe_free (probe);
          self->cycle_error = error;
          return;
        }
      self->match_verify_result =
        result.score > 0 ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
      if (self->match_verify_result == FPI_MATCH_SUCCESS)
        {
          self->match_result = result;
          self->match_probe = probe;
        }
      else
        {
          goodix_chicago_runtime_probe_free (probe);
        }
    }
  else
    {
      FpPrint *match = NULL;
      gint32 best_score = G_MININT32;
      GoodixChicagoMatchTemplateResult best_result = { 0, };

      for (guint i = 0; self->match_gallery && i < self->match_gallery->len;
           i++)
        {
          FpPrint *candidate = g_ptr_array_index (self->match_gallery, i);
          GoodixChicagoMatchTemplateResult result = { 0, };

          match_against_print_data (self, probe, candidate, &result, &error);
          if (error)
            {
              goodix_chicago_runtime_probe_free (probe);
              self->cycle_error = error;
              return;
            }
          if (result.score > 0 && result.score > best_score)
            {
              best_score = result.score;
              match = candidate;
              best_result = result;
            }
        }

      self->match_identify_match = match;
      if (match)
        {
          self->match_result = best_result;
          self->match_probe = probe;
        }
      else
        {
          goodix_chicago_runtime_probe_free (probe);
        }
    }

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
  GoodixChicagoRuntimeProbe *probe = NULL;
  GoodixChicagoRuntimeReject reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;
  GError *error = NULL;

  encrypted = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                             result, &error);
  self->operation_active = FALSE;
  if (!encrypted)
    {
      self->cycle_error = error;
      wait_for_finger_removal (self);
      return;
    }

  packed = decrypt_packed (self, encrypted, &error);
  if (!packed)
    {
      self->cycle_error = error;
      wait_for_finger_removal (self);
      return;
    }

  {
    gsize length = 0;
    const guint8 *data = g_bytes_get_data (packed, &length);

    if (!goodix5125_image_decode_transposed (data, length, raw,
                                             GOODIX5125_IMAGE_N_PIXELS,
                                             &error))
      {
        self->cycle_error = error;
        wait_for_finger_removal (self);
        return;
      }
  }

  probe = goodix_chicago_runtime_prepare_probe (
    self->chicago_preprocessor, raw, &reject, &error);

  if (!probe && error)
    {
      self->cycle_error = error;
      wait_for_finger_removal (self);
      return;
    }

  if (!probe)
    {
      fp_dbg ("goodix5125: Chicago capture rejected reason=%s",
              reject == GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES ?
              "no-features" :
              reject == GOODIX_CHICAGO_RUNTIME_REJECT_BAD_INPUT ?
              "bad-input" : "poor-capture");

      if (self->action_kind == GOODIX5125_ACTION_ENROLL)
        {
          report_enroll_progress (
            self, fpi_device_retry_new_msg (
              FP_DEVICE_RETRY_GENERAL,
              reject == GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES ?
              "No fingerprint features found; cover the sensor fully." :
              "Fingerprint image quality is too low; try again."));
          self->cycle_should_continue = TRUE;
        }
      else
        {
          self->match_attempt++;
          if (self->match_attempt < GOODIX5125_VERIFY_CAPTURE_ATTEMPTS)
            self->cycle_should_continue = TRUE;
          else
            self->match_exhausted_bad_quality = TRUE;
        }

      wait_for_finger_removal (self);
      return;
    }

  if (self->action_kind == GOODIX5125_ACTION_ENROLL)
    enroll_accumulate (self, probe);
  else
    match_decide (self, probe);

  wait_for_finger_removal (self);
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
  gsize length = 0;
  const guint8 *data;

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (!response)
    {
      self->cycle_error = error;
      finish_action (self);
      return;
    }

  data = g_bytes_get_data (response, &length);
  if (!goodix5125_fdt_up_base_from_reply (
        data, length, self->otp_info.fdt_delta,
        self->fdt_up_base, &error))
    {
      self->cycle_error = error;
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
  guint8 payload[2 + GOODIX5125_FDT_BASE_SIZE] = { 0x0c, 0x01 };

  response = goodix5125_usb_command_finish (FP_DEVICE (source_object),
                                            result, &error);
  self->operation_active = FALSE;
  if (!response)
    {
      self->cycle_error = error;
      finish_action (self);
      return;
    }

  g_bytes_get_data (response, &length);
  if (length < 14)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Goodix MCU-state reply is shorter than 14 bytes");
      self->cycle_error = error;
      finish_action (self);
      return;
    }

  memcpy (payload + 2, self->fdt_base, GOODIX5125_FDT_BASE_SIZE);
  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_FDT_DOWN,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    0, fpi_device_get_cancellable (FP_DEVICE (self)),
    fdt_down_ready_cb, self);
}

static void
nop_before_fdt_cb (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  static const guint8 payload[] = { 0x55 };
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (user_data);
  GError *error = NULL;

  if (!goodix5125_usb_nop_finish (FP_DEVICE (source_object), result, &error))
    {
      self->operation_active = FALSE;
      self->cycle_error = error;
      finish_action (self);
      return;
    }

  self->operation_active = TRUE;
  goodix5125_usb_command_async (
    FP_DEVICE (self), GOODIX5125_COMMAND_QUERY_STATE,
    GOODIX5125_CHECKSUM_NORMAL, GOODIX5125_CHECKSUM_NORMAL,
    payload, sizeof (payload), TRUE, GOODIX5125_OUTER_FLAG_MESSAGE,
    COMMAND_TIMEOUT_MS, fpi_device_get_cancellable (FP_DEVICE (self)),
    query_before_fdt_cb, self);
}

static void
begin_wait_for_finger (FpDeviceGoodix5125 *self)
{
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED);
  self->operation_active = TRUE;
  goodix5125_usb_nop_async (FP_DEVICE (self), NOP_TIMEOUT_MS,
                            fpi_device_get_cancellable (FP_DEVICE (self)),
                            nop_before_fdt_cb, self);
}

/* ---------------------------------------------------------------------
 * Action completion
 * --------------------------------------------------------------------- */

static void
finish_enroll (FpDeviceGoodix5125 *self)
{
  FpDevice *dev = FP_DEVICE (self);

  if (self->cycle_error)
    {
      GError *error = g_steal_pointer (&self->cycle_error);

      reset_action_state (self);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  {
    g_autoptr(GBytes) packed = goodix_chicago_enrollment_pack (
      self->enroll_accumulator, NULL);
    GError *error = NULL;
    FpPrint *print = NULL;
    GVariant *data;

    if (!packed)
      {
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "failed to pack the completed Chicago "
                             "enrollment");
        reset_action_state (self);
        fpi_device_enroll_complete (dev, NULL, error);
        return;
      }

    data = g_variant_ref_sink (goodix_chicago_print_data_build (
                                 self->sensor_id, self->chicago_calibration, packed));
    fpi_device_get_enroll_data (dev, &print);
    print = g_object_ref (print);
    fpi_print_set_type (print, FPI_PRINT_RAW);
    g_object_set (G_OBJECT (print), "fpi-data", data, NULL);
    g_variant_unref (data);

    fp_dbg ("goodix5125: native Chicago enrollment complete with %u "
            "subtemplates, %u accepted",
            goodix_chicago_enrollment_get_count (self->enroll_accumulator),
            self->enroll_policy.used);

    reset_action_state (self);
    fpi_device_enroll_complete (dev, print, NULL);
  }
}

static void
finish_match (FpDeviceGoodix5125 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  gboolean is_verify = self->action_kind == GOODIX5125_ACTION_VERIFY;

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

  {
    FpPrint *matched_print = is_verify ?
                             (self->match_verify_result == FPI_MATCH_SUCCESS ?
                              self->match_template : NULL) :
                             self->match_identify_match;

    if (matched_print && self->match_probe)
      {
        g_autoptr(GVariant) print_data = NULL;
        g_autoptr(GVariant) updated_data = NULL;
        GError *study_error = NULL;

        g_object_get (G_OBJECT (matched_print), "fpi-data", &print_data,
                      NULL);
        if (!goodix_chicago_runtime_study_print_data (
              self->match_probe, print_data, self->sensor_id,
              self->chicago_calibration, &self->match_result,
              &updated_data, &study_error))
          {
            fp_warn ("goodix5125: templateStudy update failed after "
                     "match: %s",
                     study_error ? study_error->message : "unknown error");
            g_clear_error (&study_error);
          }
        else if (updated_data)
          {
            g_object_set (G_OBJECT (matched_print), "fpi-data",
                          updated_data, NULL);
            fp_dbg ("goodix5125: templateStudy updated matched template");
          }
      }

    if (is_verify)
      {
        fpi_device_verify_report (dev, self->match_verify_result, NULL,
                                  NULL);
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
}

static void
finish_action (FpDeviceGoodix5125 *self)
{
  switch (self->action_kind)
    {
    case GOODIX5125_ACTION_ENROLL:
      finish_enroll (self);
      return;

    case GOODIX5125_ACTION_VERIFY:
    case GOODIX5125_ACTION_IDENTIFY:
      finish_match (self);
      return;

    case GOODIX5125_ACTION_NONE:
    default:
      g_assert_not_reached ();
    }
}

/* ---------------------------------------------------------------------
 * FpDevice vtable
 * --------------------------------------------------------------------- */

static void
goodix5125_probe (FpDevice *device)
{
  fpi_device_probe_complete (device, NULL, NULL, NULL);
}

static void
goodix5125_open (FpDevice *device)
{
  GError *error = NULL;

  if (!g_usb_device_claim_interface (
        fpi_device_get_usb_device (device),
        GOODIX5125_USB_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  fpi_device_set_nr_enroll_stages (device,
                                   GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES);
  fpi_device_open_complete (device, NULL);
}

static void
goodix5125_close (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  GError *error = NULL;

  g_return_if_fail (!self->operation_active);
  self->initialized = FALSE;
  g_clear_pointer (&self->tls, goodix5125_tls_free);
  g_clear_pointer (&self->chicago_calibration, g_bytes_unref);
  g_clear_pointer (&self->chicago_preprocessor,
                   goodix_chicago_preprocessor_free);
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

  if (!ensure_activated (self, &error))
    {
      fpi_device_enroll_complete (device, NULL, error);
      return;
    }

  reset_action_state (self);
  self->action_kind = GOODIX5125_ACTION_ENROLL;
  self->enroll_accumulator = goodix_chicago_enrollment_new ();
  goodix_chicago_engine_enrollment_policy_init (&self->enroll_policy);
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

  reset_action_state (self);
  self->action_kind = GOODIX5125_ACTION_IDENTIFY;
  self->match_gallery = g_ptr_array_ref (gallery);
  begin_wait_for_finger (self);
}

static void
fpi_device_goodix5125_init (FpDeviceGoodix5125 *self)
{
  (void) self;
}

static void
fpi_device_goodix5125_finalize (GObject *object)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (object);

  g_clear_pointer (&self->tls, goodix5125_tls_free);
  g_clear_pointer (&self->chicago_calibration, g_bytes_unref);
  g_clear_pointer (&self->chicago_preprocessor,
                   goodix_chicago_preprocessor_free);
  reset_action_state (self);

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
  device_class->nr_enroll_stages = GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES;

  device_class->probe = goodix5125_probe;
  device_class->open = goodix5125_open;
  device_class->close = goodix5125_close;
  device_class->enroll = goodix5125_enroll;
  device_class->verify = goodix5125_verify;
  device_class->identify = goodix5125_identify;

  fpi_device_class_auto_initialize_features (device_class);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodix5125_finalize;
}
