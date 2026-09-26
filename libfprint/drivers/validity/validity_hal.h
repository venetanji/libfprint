/*
 * Hardware Abstraction Layer for Validity/Synaptics VCSFW fingerprint sensors
 *
 * Per-device blob data, flash partition layouts, and pairing constants.
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

/* Forward declaration from validity.h */
typedef enum {
  VALIDITY_HAL_DEV_90 = 0,
  VALIDITY_HAL_DEV_97,
  VALIDITY_HAL_DEV_9A,
  VALIDITY_HAL_DEV_9D,
  VALIDITY_HAL_DEV_AB,
  VALIDITY_HAL_DEV_CB,
  VALIDITY_HAL_DEV_B7,
  VALIDITY_HAL_DEV_D2,
} ValidityHalDeviceType;

/* Flash partition entry — matches python-validity PartitionInfo:
 *   id, type, access_lvl, offset, size */
typedef struct
{
  guint8  id;
  guint8  type;
  guint16 access_lvl;
  guint32 offset;
  guint32 size;
} ValidityPartition;

/* Per-device flash layout with partition table.
 * The RSA partition signature is now loaded at runtime from external
 * data files (validity_data.c) instead of being compiled in. */
typedef struct
{
  const ValidityPartition *partitions;
  gsize                    num_partitions;
} ValidityFlashLayout;

/* Per-device flash layout descriptor.
 * Blob data has been moved to external .bin files loaded at runtime
 * by validity_data.c. This struct retains only hardware identity
 * and flash partition layout. */
typedef struct
{
  guint16 vid;
  guint16 pid;

  /* Flash partition layout for this device variant */
  const ValidityFlashLayout *flash_layout;
} ValidityDeviceDesc;

/* Number of flash partition entries in the standard layout */
#define VALIDITY_FLASH_NUM_PARTITIONS 5

/* Partition signature size (RSA-2048) */
#define VALIDITY_PARTITION_SIG_SIZE 256

/* Look up device descriptor by ValidityDeviceType enum.
 * Returns NULL if dev_type is out of range. */
const ValidityDeviceDesc *validity_hal_device_lookup (guint dev_type);

/* Look up device descriptor by USB VID/PID.
 * Returns NULL if no matching entry. */
const ValidityDeviceDesc *validity_hal_device_lookup_by_pid (guint16 vid,
                                                             guint16 pid);
