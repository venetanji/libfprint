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

#define FP_COMPONENT "validity"

#include "validity_hal.h"
#include "fpi-log.h"

/* ================================================================
 * Flash partition layouts
 *
 * Standard layout: used by PID 0097, 009a, 009d
 *   Partition 4 (template DB) = 0x80000 bytes
 *
 * PID 0090 layout: smaller DB partition
 *   Partition 4 (template DB) = 0x30000 bytes
 * ================================================================ */

/* Standard partition table (0097, 009a, 009d) */
static const ValidityPartition flash_partitions_standard[] = {
  { .id = 1, .type = 4, .access_lvl = 7, .offset = 0x00001000, .size = 0x00001000 },  /* cert store */
  { .id = 2, .type = 1, .access_lvl = 2, .offset = 0x00002000, .size = 0x0003e000 },  /* xpfwext */
  { .id = 5, .type = 5, .access_lvl = 3, .offset = 0x00040000, .size = 0x00008000 },  /* reserved */
  { .id = 6, .type = 6, .access_lvl = 3, .offset = 0x00048000, .size = 0x00008000 },  /* calibration */
  { .id = 4, .type = 3, .access_lvl = 5, .offset = 0x00050000, .size = 0x00080000 },  /* template DB */
};

/* PID 0090 partition table (smaller DB) */
static const ValidityPartition flash_partitions_0090[] = {
  { .id = 1, .type = 4, .access_lvl = 7, .offset = 0x00001000, .size = 0x00001000 },  /* cert store */
  { .id = 2, .type = 1, .access_lvl = 2, .offset = 0x00002000, .size = 0x0003e000 },  /* xpfwext */
  { .id = 5, .type = 5, .access_lvl = 3, .offset = 0x00040000, .size = 0x00008000 },  /* reserved */
  { .id = 6, .type = 6, .access_lvl = 3, .offset = 0x00048000, .size = 0x00008000 },  /* calibration */
  { .id = 4, .type = 3, .access_lvl = 5, .offset = 0x00050000, .size = 0x00030000 },  /* template DB */
};

/* Layout descriptors */
static const ValidityFlashLayout flash_layout_standard = {
  .partitions     = flash_partitions_standard,
  .num_partitions = G_N_ELEMENTS (flash_partitions_standard),
};

static const ValidityFlashLayout flash_layout_0090 = {
  .partitions     = flash_partitions_0090,
  .num_partitions = G_N_ELEMENTS (flash_partitions_0090),
};

/* ================================================================
 * Per-device descriptors
 *
 * Blob data (init, reset, db_write_enable) has been moved to external
 * .bin files loaded at runtime by validity_data.c. Only VID/PID and
 * flash layout remain here.
 * ================================================================ */

static const ValidityDeviceDesc device_table[] = {
  [VALIDITY_HAL_DEV_90] = {
    .vid = 0x138a,
    .pid = 0x0090,
    .flash_layout = &flash_layout_0090,
  },

  [VALIDITY_HAL_DEV_97] = {
    .vid = 0x138a,
    .pid = 0x0097,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_9A] = {
    .vid = 0x06cb,
    .pid = 0x009a,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_9D] = {
    .vid = 0x138a,
    .pid = 0x009d,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_AB] = {
    .vid = 0x138a,
    .pid = 0x00ab,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_CB] = {
    .vid = 0x06cb,
    .pid = 0x00cb,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_B7] = {
    .vid = 0x06cb,
    .pid = 0x00b7,
    .flash_layout = &flash_layout_standard,
  },

  [VALIDITY_HAL_DEV_D2] = {
    .vid = 0x06cb,
    .pid = 0x00d2,
    .flash_layout = &flash_layout_standard,
  },
};

/* ================================================================
 * Lookup functions
 * ================================================================ */

const ValidityDeviceDesc *
validity_hal_device_lookup (guint dev_type)
{
  if (dev_type >= G_N_ELEMENTS (device_table))
    return NULL;

  return &device_table[dev_type];
}

const ValidityDeviceDesc *
validity_hal_device_lookup_by_pid (guint16 vid, guint16 pid)
{
  for (gsize i = 0; i < G_N_ELEMENTS (device_table); i++)
    if (device_table[i].vid == vid && device_table[i].pid == pid)
      return &device_table[i];

  return NULL;
}
