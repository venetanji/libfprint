/*
 * Broadcom ControlVault2 (BCM5880) fingerprint driver
 *
 * Copyright (C) 2026 Matt Trent <matt@thetrents.org>
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
 * Match-on-chip driver for the fingerprint function of the Dell ControlVault2
 * (Broadcom BCM5880, USB 0a5c:5834). Only interface 0 is used; the
 * smart-card interfaces are left alone.
 *
 * The protocol was worked out from USB captures of the reader only.
 *
 * Security: templates are stored and matched inside the chip, and no
 * fingerprint images or templates cross USB. The plaintext interface used
 * here does not authenticate replies, so the host trusts the reader's match
 * result, as with other match-on-chip drivers in libfprint.
 */

#define FP_COMPONENT "cv2"

#include "drivers_api.h"

#include "cv2.h"

#define CV2_INTERFACE 0

struct _FpiDeviceCv2
{
  FpDevice parent;
};

G_DEFINE_TYPE (FpiDeviceCv2, fpi_device_cv2, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x0a5c, .pid = 0x5834, },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

/* ---- Device open and close ---- */

static void
cv2_open (FpDevice *dev)
{
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     CV2_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void
cv2_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  CV2_INTERFACE, 0, &error);
  fpi_device_close_complete (dev, error);
}

/* ---- GObject ---- */

static void
fpi_device_cv2_init (FpiDeviceCv2 *self)
{
}

static void
fpi_device_cv2_class_init (FpiDeviceCv2Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Broadcom ControlVault2 (BCM5880) Fingerprint Reader";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 4;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = cv2_open;
  dev_class->close = cv2_close;

  fpi_device_class_auto_initialize_features (dev_class);
}
