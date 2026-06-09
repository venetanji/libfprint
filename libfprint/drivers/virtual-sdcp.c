/*
 * Virtual driver for SDCP device debugging
 *
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

/*
 * This is a virtual driver to debug the SDCP-based drivers.
 * This virtual driver does not use a socket listener but instead
 * embeds the simulated logic of the fake "device" directly within
 * the logic of each function. This driver also allows prints to be
 * registered programmatically, making it possible to test libfprint
 * and fprintd.
 *
 * This virtual driver will override FpSdcpDevice's dynamically
 * generated cryptography values and instead replace them with
 * pre-generated values taken from from Microsoft's sample client
 * implementation. See:
 *   https://github.com/Microsoft/SecureDeviceConnectionProtocol
 */

#define FP_COMPONENT "virtual_sdcp"
#include "fpi-log.h"

#include "../fpi-sdcp.h"
#include "virtual-sdcp.h"

struct _FpDeviceVirtualSdcp
{
  FpSdcpDevice parent;

  GPtrArray   *print_ids;
};

G_DECLARE_FINAL_TYPE (FpDeviceVirtualSdcp, fpi_device_virtual_sdcp, FPI, DEVICE_VIRTUAL_SDCP, FpSdcpDevice)
G_DEFINE_TYPE (FpDeviceVirtualSdcp, fpi_device_virtual_sdcp, FP_TYPE_SDCP_DEVICE)

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
dev_identify (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceVirtualSdcp *self = FPI_DEVICE_VIRTUAL_SDCP (sdcp_device);
  g_autoptr(GBytes) enrollment_id = NULL;
  g_autoptr(GBytes) identify_mac = NULL;
  GBytes *identify_nonce = NULL;

  if (self->print_ids->len > 0)
    {
      /*
       * Pretend that the virtual device identified the first print.
       * Since we used a pre-generated enrollment_id for it, we can also use the
       * matching pre-generated test identify data for its identification.
       */
      identify_nonce = g_bytes_from_hex (identify_nonce_hex);
      enrollment_id = g_bytes_from_hex (enrollment_id_hex);
      fpi_sdcp_device_set_identify_data (sdcp_device, identify_nonce);
      identify_mac = g_bytes_from_hex (identify_mac_hex);

      fpi_sdcp_device_identify_complete (sdcp_device, enrollment_id, identify_mac, NULL);
    }
  else
    {
      fpi_sdcp_device_identify_complete (sdcp_device, NULL, NULL,
                                         fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
    }
}

static void
dev_enroll_commit (FpSdcpDevice *sdcp_device, GBytes *id)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceVirtualSdcp *self = FPI_DEVICE_VIRTUAL_SDCP (sdcp_device);
  g_autoptr(GBytes) expected_first_print_id = NULL;
  GBytes *print_id;

  print_id = g_bytes_new (g_bytes_get_data (id, NULL), g_bytes_get_size (id));

  /*
   * If this is the first print, it is probably good if we make sure the
   * internal API assigned it the expected pre-known ID
   */
  if (self->print_ids->len == 0)
    {
      expected_first_print_id = g_bytes_from_hex (enrollment_id_hex);
      if (!g_bytes_equal (print_id, expected_first_print_id))
        {
          fpi_sdcp_device_enroll_commit_complete (sdcp_device,
                                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_UNTRUSTED,
                                                                            "First enrolled print ID does not match expected value"));
          return;
        }
    }

  g_ptr_array_add (self->print_ids, g_steal_pointer (&print_id));

  fpi_sdcp_device_enroll_commit_complete (sdcp_device, NULL);
}

static void
dev_enroll (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceVirtualSdcp *self = FPI_DEVICE_VIRTUAL_SDCP (sdcp_device);
  FpDevice *device = FP_DEVICE (sdcp_device);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) nonce = NULL;
  FpPrint *print;

  fpi_device_get_enroll_data (device, &print);

  if (self->print_ids->len == 0)
    {
      /* Use the hard-coded nonce for the first enrollment */
      nonce = g_bytes_from_hex (enrollment_nonce_hex);
    }
  else
    {
      /* Generate a new nonce for all other enrollments */
      nonce = fpi_sdcp_generate_random (&error);
      if (error)
        fpi_device_enroll_progress (device, 0, print, error);
    }

  fpi_device_enroll_progress (device, 1, print, NULL);
  fpi_sdcp_device_enroll_commit (sdcp_device, nonce, error);
}

static void
dev_list (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceVirtualSdcp *self = FPI_DEVICE_VIRTUAL_SDCP (sdcp_device);
  GPtrArray *print_ids = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

  for (gint i = 0; i < self->print_ids->len; i++)
    {
      GBytes *print_id = g_ptr_array_index (self->print_ids, i);
      fp_dbg ("print %d:", i);
      fp_dbg_hex_dump_gbytes (print_id);
      g_ptr_array_add (print_ids, g_bytes_new (g_bytes_get_data (print_id, NULL),
                                               g_bytes_get_size (print_id)));
    }

  fpi_sdcp_device_list_complete (sdcp_device, g_steal_pointer (&print_ids), NULL);
}

static void
dev_reconnect (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) random = NULL;
  g_autoptr(GBytes) reconnect_mac = g_bytes_from_hex (reconnect_mac_hex);

  /*
   * Normally, a driver would fetch the reconnect data and then send it to the
   * device's Reconnect command. In this fake device, we will just fetch and
   * verify the random was generated but do nothing with it
   */

  fpi_sdcp_device_get_reconnect_data (sdcp_device, &random);

  g_assert (random);
  g_assert (g_bytes_get_size (random) == SDCP_RANDOM_SIZE);

  /*
   * In emulation mode (FP_DEVICE_EMULATION=1), a different hard-coded random is
   * set in fpi-sdcp-device, which was the same random used to generate the
   * reconnect_mac value provided here
   */

  fpi_sdcp_device_reconnect_complete (sdcp_device, reconnect_mac, error);
}

static void
dev_connect (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) host_random = NULL;
  g_autoptr(GBytes) host_public_key = NULL;
  g_autoptr(GBytes) device_random = g_bytes_from_hex (device_random_hex);
  g_autoptr(GBytes) connect_mac = g_bytes_from_hex (connect_mac_hex);
  g_autoptr(FpiSdcpClaim) claim = get_fake_sdcp_claim ();

  fpi_sdcp_device_get_connect_data (sdcp_device, &host_random, &host_public_key);

  g_assert (host_random);
  g_assert (g_bytes_get_size (host_random) == SDCP_RANDOM_SIZE);

  g_assert (host_public_key);
  g_assert (g_bytes_get_size (host_public_key) == SDCP_PUBLIC_KEY_SIZE);

  fpi_sdcp_device_connect_complete (sdcp_device, device_random, claim, connect_mac, error);
}

static void
dev_close (FpDevice *device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  fpi_device_close_complete (device, NULL);
}

static void
dev_open (FpSdcpDevice *sdcp_device)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpSdcpDeviceClass *klass = FP_SDCP_DEVICE_GET_CLASS (sdcp_device);

  if (g_strcmp0 (g_getenv ("FP_VIRTUAL_SDCP_NO_RECONNECT"), "1") == 0)
    {
      fp_dbg ("Virtual SDCP device: FP_VIRTUAL_SDCP_NO_RECONNECT=1; disabling SDCP reconnect");
      klass->reconnect = NULL;
    }
  else
    {
      fp_dbg ("Virtual SDCP device: FP_VIRTUAL_SDCP_NO_RECONNECT!=1; enabling SDCP reconnect");
      klass->reconnect = dev_reconnect;
    }

  fpi_sdcp_device_open_complete (sdcp_device, NULL);
}

static void
fpi_device_virtual_sdcp_finalize (GObject *object)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceVirtualSdcp *self = FPI_DEVICE_VIRTUAL_SDCP (object);

  g_clear_pointer (&self->print_ids, g_ptr_array_unref);

  G_OBJECT_CLASS (fpi_device_virtual_sdcp_parent_class)->finalize (object);
}

static void
fpi_device_virtual_sdcp_init (FpDeviceVirtualSdcp *self)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);

  /* Force FP_DEVICE_EMULATION=1 when using FpDeviceVirtualSdcp */
  g_setenv ("FP_DEVICE_EMULATION", "1", TRUE);

  self->print_ids = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
}

static const FpIdEntry driver_ids[] = {
  { .virtual_envvar = "FP_VIRTUAL_SDCP" },
  { .virtual_envvar = NULL }
};

static void
fpi_device_virtual_sdcp_class_init (FpDeviceVirtualSdcpClass *klass)
{
  fp_dbg ("Virtual SDCP device: %s()", G_STRFUNC);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpSdcpDeviceClass *sdcp_dev_class = FP_SDCP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_virtual_sdcp_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Virtual SDCP device for debugging";
  dev_class->type = FP_DEVICE_TYPE_VIRTUAL;
  dev_class->id_table = driver_ids;
  dev_class->nr_enroll_stages = 1;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  sdcp_dev_class->ignore_device_certificate = FALSE;
  sdcp_dev_class->ignore_device_signatures = FALSE;

  sdcp_dev_class->open = dev_open;
  sdcp_dev_class->connect = dev_connect;
  sdcp_dev_class->reconnect = dev_reconnect;

  sdcp_dev_class->list = dev_list;
  sdcp_dev_class->enroll = dev_enroll;
  sdcp_dev_class->enroll_commit = dev_enroll_commit;
  sdcp_dev_class->identify = dev_identify;

  dev_class->close = dev_close;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features |= FP_DEVICE_FEATURE_STORAGE;
}
