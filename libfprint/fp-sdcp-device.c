/*
 * FpSdcpDevice - A base class for SDCP enabled devices
 * Copyright (C) 2020 Benjamin Berg <bberg@redhat.com>
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

#define FP_COMPONENT "sdcp_device"
#include "fpi-log.h"

#include "fp-sdcp-device-private.h"

/**
 * SECTION: fp-sdcp-device
 * @title: FpSdcpDevice
 * @short_description: SDCP device subclass
 *
 * This is a base class for devices implementing the SDCP security protocol.
 */

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpSdcpDevice, fp_sdcp_device, FP_TYPE_DEVICE)

enum {
  PROP_0,
  PROP_SDCP_DATA,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

/*******************************************************/

/* Callbacks/vfuncs */
static void
fp_sdcp_device_open (FpDevice *device)
{
  FpSdcpDevice *self = FP_SDCP_DEVICE (device);

  fpi_sdcp_device_open (self);
}

static void
fp_sdcp_device_list (FpDevice *device)
{
  FpSdcpDevice *self = FP_SDCP_DEVICE (device);

  fpi_sdcp_device_list (self);
}

static void
fp_sdcp_device_enroll (FpDevice *device)
{
  FpSdcpDevice *self = FP_SDCP_DEVICE (device);

  fpi_sdcp_device_enroll (self);
}

static void
fp_sdcp_device_identify (FpDevice *device)
{
  FpSdcpDevice *self = FP_SDCP_DEVICE (device);

  fpi_sdcp_device_identify (self);
}

/*********************************************************/

static void
fp_sdcp_device_finalize (GObject *object)
{
  FpSdcpDevice *self = (FpSdcpDevice *) object;
  FpSdcpDevicePrivate *priv = fp_sdcp_device_get_instance_private (self);

  g_clear_pointer (&priv->host_private_key, g_bytes_unref);
  g_clear_pointer (&priv->host_public_key, g_bytes_unref);
  g_clear_pointer (&priv->host_random, g_bytes_unref);
  g_clear_pointer (&priv->reconnect_random, g_bytes_unref);
  g_clear_pointer (&priv->identify_nonce, g_bytes_unref);
  g_clear_pointer (&priv->data, g_bytes_unref);

  G_OBJECT_CLASS (fp_sdcp_device_parent_class)->finalize (object);
}

static void
fp_sdcp_device_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  FpSdcpDevice *self = (FpSdcpDevice *) object;
  FpSdcpDevicePrivate *priv = fp_sdcp_device_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SDCP_DATA:
      g_value_set_boxed (value, priv->data);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
fp_sdcp_device_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  FpSdcpDevice *self = FP_SDCP_DEVICE (object);
  FpSdcpDevicePrivate *priv = fp_sdcp_device_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SDCP_DATA:
      g_clear_pointer (&priv->data, g_bytes_unref);
      priv->data = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
fp_sdcp_device_constructed (GObject *obj)
{
  G_OBJECT_CLASS (fp_sdcp_device_parent_class)->constructed (obj);
}

static void
fp_sdcp_device_class_init (FpSdcpDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *fp_device_class = FP_DEVICE_CLASS (klass);

  object_class->finalize = fp_sdcp_device_finalize;
  object_class->get_property = fp_sdcp_device_get_property;
  object_class->set_property = fp_sdcp_device_set_property;
  object_class->constructed = fp_sdcp_device_constructed;

  fp_device_class->open = fp_sdcp_device_open;
  fp_device_class->list = fp_sdcp_device_list;
  fp_device_class->enroll = fp_sdcp_device_enroll;
  fp_device_class->verify = fp_sdcp_device_identify;
  fp_device_class->identify = fp_sdcp_device_identify;

  properties[PROP_SDCP_DATA] =
    g_param_spec_boxed ("sdcp-data",
                        "SDCP Data",
                        "SDCP-related device data that should be persisted and used with the "
                        "device during the current system boot",
                        G_TYPE_BYTES,
                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  fpi_device_class_auto_initialize_features (fp_device_class);
}

static void
fp_sdcp_device_init (FpSdcpDevice *self)
{
}
