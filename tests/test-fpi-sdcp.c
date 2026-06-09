/*
 * Secure Device Connection Protocol (SDCP) support unit tests
 * Copyright (C) 2025 Joshua Grisham <josh@joshuagrisham.com>
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

#define FP_COMPONENT "test_fpi_sdcp"
#include "fpi-log.h"

#include "fpi-sdcp.h"
#include "fpi-sdcp-device.h"

/* We can re-use the test payloads from virtual-sdcp */
#include "drivers/virtual-sdcp.h"

/******************************************************************************/

static const guint8 from_hex_map[] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,   // 01234567
  0x08, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // 89:;<=>?
  0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,         // @abcdef
};

static GBytes *
g_bytes_from_hex (const gchar *hex)
{
  g_autoptr(GBytes) res = NULL;
  guint8 b0, b1;
  gsize bytes_len = strlen (hex) / 2;
  guint8 *bytes = g_malloc0 (bytes_len);

  for (int i = 0; i < strlen (hex) - 1; i += 2)
    {
      b0 = ((guint8) hex[i + 0] & 0x1F) ^ 0x10;
      b1 = ((guint8) hex[i + 1] & 0x1F) ^ 0x10;
      bytes[i / 2] = (guint8) (from_hex_map[b0] << 4) | from_hex_map[b1];
    }

  res = g_bytes_new_take (bytes, bytes_len);

  return g_steal_pointer (&res);
}

static FpiSdcpClaim *
get_fake_sdcp_claim (void)
{
  FpiSdcpClaim *claim = g_new0 (FpiSdcpClaim, 1);

  claim->model_certificate = g_bytes_from_hex (model_certificate_hex);
  claim->device_public_key = g_bytes_from_hex (device_public_key_hex);
  claim->firmware_public_key = g_bytes_from_hex (firmware_public_key_hex);
  claim->firmware_hash = g_bytes_from_hex (firmware_hash_hex);
  claim->model_signature = g_bytes_from_hex (model_signature_hex);
  claim->device_signature = g_bytes_from_hex (device_signature_hex);
  return g_steal_pointer (&claim);
}

/******************************************************************************/

static void
test_generate_enrollment_id (void)
{
  g_autoptr(GBytes) id = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) application_secret = g_bytes_from_hex (application_secret_hex);
  g_autoptr(GBytes) nonce = g_bytes_from_hex (enrollment_nonce_hex);
  g_autoptr(GBytes) expected_id = g_bytes_from_hex (enrollment_id_hex);

  id = fpi_sdcp_generate_enrollment_id (application_secret, nonce, &error);

  fp_dbg ("id:");
  fp_dbg_hex_dump_gbytes (id);

  fp_dbg ("expected:");
  fp_dbg_hex_dump_gbytes (expected_id);

  g_assert (g_bytes_equal (expected_id, id));
  g_assert_null (error);
}

static void
test_verify_identify (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) application_secret = g_bytes_from_hex (application_secret_hex);
  g_autoptr(GBytes) nonce = g_bytes_from_hex (identify_nonce_hex);
  g_autoptr(GBytes) id = g_bytes_from_hex (enrollment_id_hex);
  g_autoptr(GBytes) mac = g_bytes_from_hex (identify_mac_hex);

  g_assert_true (fpi_sdcp_verify_identify (application_secret, nonce, id, mac, &error));
  g_assert_null (error);
}

static void
test_verify_reconnect (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) application_secret = g_bytes_from_hex (application_secret_hex);
  g_autoptr(GBytes) random = g_bytes_from_hex (reconnect_random_hex);
  g_autoptr(GBytes) mac = g_bytes_from_hex (reconnect_mac_hex);

  g_assert_true (fpi_sdcp_verify_reconnect (application_secret, random, mac, &error));
  g_assert_null (error);
}

static void
test_verify_connect (void)
{
  g_autoptr(GBytes) application_secret = NULL;
  g_autoptr(GError) error = NULL;

  g_autoptr(GBytes) host_private_key = g_bytes_from_hex (host_private_key_hex);
  g_autoptr(GBytes) host_random = g_bytes_from_hex (host_random_hex);
  g_autoptr(GBytes) device_random = g_bytes_from_hex (device_random_hex);
  g_autoptr(GBytes) connect_mac = g_bytes_from_hex (connect_mac_hex);
  FpiSdcpClaim *claim = get_fake_sdcp_claim ();

  g_autoptr(GBytes) expected_application_secret = g_bytes_from_hex (application_secret_hex);

  g_assert_true (fpi_sdcp_verify_connect (host_private_key,
                                          host_random,
                                          device_random,
                                          claim,
                                          connect_mac,
                                          TRUE,
                                          TRUE,
                                          &application_secret,
                                          &error));

  g_assert_null (error);
  g_assert (g_bytes_get_size (application_secret) == SDCP_APPLICATION_SECRET_SIZE);

  fp_dbg ("application_secret:");
  fp_dbg_hex_dump_gbytes (application_secret);

  fp_dbg ("expected:");
  fp_dbg_hex_dump_gbytes (expected_application_secret);

  g_assert_true (g_bytes_equal (expected_application_secret, application_secret));

  fpi_sdcp_claim_free (claim);
}

static void
test_generate_random (void)
{
  g_autoptr(GBytes) random = NULL;
  g_autoptr(GError) error = NULL;

  random = fpi_sdcp_generate_random (&error);

  g_assert_null (error);
  g_assert (g_bytes_get_size (random) == SDCP_RANDOM_SIZE);

  fp_dbg ("random:");
  fp_dbg_hex_dump_gbytes (random);
}

static void
test_generate_host_key (void)
{
  g_autoptr(GBytes) private_key = NULL;
  g_autoptr(GBytes) public_key = NULL;
  g_autoptr(GError) error = NULL;
  gsize len = 0;

  fpi_sdcp_generate_host_key (&private_key, &public_key, &error);

  g_assert_null (error);

  g_bytes_get_data (private_key, &len);
  g_assert (len == 32);

  fp_dbg ("private_key:");
  fp_dbg_hex_dump_gbytes (private_key);

  g_bytes_get_data (public_key, &len);
  g_assert (len == SDCP_PUBLIC_KEY_SIZE);

  fp_dbg ("public_key:");
  fp_dbg_hex_dump_gbytes (public_key);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sdcp/generate_host_key", test_generate_host_key);
  g_test_add_func ("/sdcp/generate_random", test_generate_random);
  g_test_add_func ("/sdcp/verify_connect", test_verify_connect);
  g_test_add_func ("/sdcp/verify_reconnect", test_verify_reconnect);
  g_test_add_func ("/sdcp/verify_identify", test_verify_identify);
  g_test_add_func ("/sdcp/generate_enrollment_id", test_generate_enrollment_id);

  return g_test_run ();
}
