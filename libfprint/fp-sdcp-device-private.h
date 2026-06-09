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

#pragma once

#include "fpi-sdcp-device.h"

typedef struct
{
  GBytes *host_private_key;
  GBytes *host_public_key;
  GBytes *host_random;
  GBytes *reconnect_random;
  GBytes *identify_nonce;
  GBytes *data;
} FpSdcpDevicePrivate;

void fpi_sdcp_device_get_application_secret (FpSdcpDevice *self,
                                             GBytes      **application_secret);
void fpi_sdcp_device_set_application_secret (FpSdcpDevice *self,
                                             GBytes       *application_secret);

void fpi_sdcp_device_open (FpSdcpDevice *self);
void fpi_sdcp_device_connect (FpSdcpDevice *self);
void fpi_sdcp_device_reconnect (FpSdcpDevice *self);

void fpi_sdcp_device_list (FpSdcpDevice *self);
void fpi_sdcp_device_enroll (FpSdcpDevice *self);
void fpi_sdcp_device_identify (FpSdcpDevice *self);
