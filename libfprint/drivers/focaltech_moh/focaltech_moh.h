/*
 * FocalTech FT9201 Match-on-Host driver for libfprint
 *
 * Copyright (C) 2025 libfprint contributors
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

#include "fpi-device.h"
#include "fpi-ssm.h"
#include "fpi-image-device.h"

G_DECLARE_FINAL_TYPE (FpiDeviceFocaltechMoh, fpi_device_focaltech_moh, FPI,
                      DEVICE_FOCALTECH_MOH, FpImageDevice)

#define FT9201_VID 0x2808
#define FT9201_PID 0x9338

#define FT9201_EP_IN 0x83   /* Bulk IN  (EP3, 32B max packet) */

/* Image: 64 wide x 80 high, 8-bit grayscale */
#define FT9201_IMG_WIDTH    64
#define FT9201_IMG_HEIGHT   80
#define FT9201_IMG_SIZE     (FT9201_IMG_WIDTH * FT9201_IMG_HEIGHT)  /* 5120 */

#define FT9201_CMD_TIMEOUT   5000
#define FT9201_POLL_INTERVAL 30   /* ms between finger detection polls */

/* USB vendor request codes */
#define FT9201_REQ_PREPARE      0x34
#define FT9201_REQ_INT_STATUS   0x43
#define FT9201_REQ_NEW_SIU_RW   0x6F

/* Prepare command wValue values */
#define FT9201_PREPARE_INIT     0x00FF
#define FT9201_PREPARE_READ     0x0003

/* New SIU compound register addresses (wIndex for req 0x6F) */
#define FT9201_REG_STATUS       0x9180  /* Chip status / OTP info */
#define FT9201_REG_CAPTURE      0x9080  /* Image capture */
#define FT9201_REG_SYNC         0xFF00  /* Sync / reset (size=0, no bulk) */

/*
 * Capture state machine — one state per async USB transfer.
 *
 * The read sequence is: PREPARE_INIT → PREPARE_READ → NEW_SIU_RW → BULK_IN.
 * Each is a separate async transfer, so each gets its own SSM state.
 */
enum capture_states {
  /* Warmup: discard first bulk read after USB reset */
  CAPTURE_WARMUP_PREP1,       /* OUT 0x34(0xFF) */
  CAPTURE_WARMUP_PREP2,       /* OUT 0x34(3) */
  CAPTURE_WARMUP_CMD,         /* OUT 0x6F(32, 0x9180) */
  CAPTURE_WARMUP_READ,        /* BULK IN 32B (discard) */

  /* Sync: poke 0xFF00 */
  CAPTURE_SYNC_PREP1,         /* OUT 0x34(0xFF) */
  CAPTURE_SYNC_PREP2,         /* OUT 0x34(3) */
  CAPTURE_SYNC_CMD,           /* OUT 0x6F(0, 0xFF00) — no bulk */

  /* Status: read 4 bytes from 0x9180 */
  CAPTURE_STATUS_PREP1,       /* OUT 0x34(0xFF) */
  CAPTURE_STATUS_PREP2,       /* OUT 0x34(3) */
  CAPTURE_STATUS_CMD,         /* OUT 0x6F(4, 0x9180) */
  CAPTURE_STATUS_READ,        /* BULK IN 32B (check status) */

  /* Image: read 5120 bytes from 0x9080 */
  CAPTURE_IMG_PREP1,          /* OUT 0x34(0xFF) */
  CAPTURE_IMG_PREP2,          /* OUT 0x34(3) */
  CAPTURE_IMG_CMD,            /* OUT 0x6F(0x1400, 0x9080) */
  CAPTURE_IMG_READ,           /* BULK IN 5120B */

  CAPTURE_NUM_STATES,
};

struct _FpiDeviceFocaltechMoh
{
  FpImageDevice parent;

  gboolean      deactivating;
  gboolean      warmup_done;
  FpiSsm       *capture_ssm;
  guint8       *image_buf;
};
