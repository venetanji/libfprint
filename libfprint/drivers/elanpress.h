/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 *
 * Copyright (C) 2026 Filip Spanne (original driver, 04f3:0c6e)
 * Copyright (C) 2026 Mohammad Nauman (04f3:0c3d, full-frame capture,
 *                                    multi-frame touches, rotation matcher)
 *
 * These sensors stream raw frames like the swipe sensors handled by the
 * elan driver, but the finger rests on the sensor instead of being swiped
 * across it. The imaged area is far too small for reliable minutiae
 * matching, so enrollment stores the images themselves and matching is
 * done by normalized cross-correlation, like the Windows driver does.
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
 */

#pragma once

#include "fpi-device.h"
#include "fpi-usb-transfer.h"

#define ELANPRESS_VEND_ID 0x04f3

#define ELANPRESS_EP_CMD_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define ELANPRESS_EP_CMD_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define ELANPRESS_EP_IMG_IN (0x2 | FPI_USB_ENDPOINT_IN)

#define ELANPRESS_CMD_LEN 2
#define ELANPRESS_CMD_TIMEOUT 5000
#define ELANPRESS_FRAME_TIMEOUT 2000

/* the pre-scan read blocks in the device until a finger arrives; after this
 * long the wait is restarted (stop, LED, fresh background) */
#define ELANPRESS_WAIT_FINGER_TIMEOUT 10000
/* once frames are flowing, a pre-scan that does not answer within this
 * time means the finger lifted */
#define ELANPRESS_FINGER_GONE_TIMEOUT 300

/* interval between finger presence polls when the device answers at once */
#define ELANPRESS_POLL_INTERVAL_MS 30

/* pre_scan response when a finger is on the sensor */
#define ELANPRESS_FINGER_PRESENT 0x55

/* frames captured per touch */
#define ELANPRESS_MIN_FRAMES 3
#define ELANPRESS_MAX_FRAMES 14

/* distinct images kept per touch (enroll) / used as probes (verify) */
#define ELANPRESS_MAX_IMAGES_PER_TOUCH 3
#define ELANPRESS_MAX_PROBES 3
/* zero-shift similarity above which two frames of a touch are duplicates */
#define ELANPRESS_DUP_SIMILARITY 0.85

/* number of touches stored during enrollment */
#define ELANPRESS_ENROLL_STAGES 20

/* a background frame is rejected (finger probably still on the pad) when
 * its mean is this far from the device's own calibration mean; this is the
 * elan driver's ELAN_CALIBRATION_MAX_DELTA */
#define ELANPRESS_BG_MAX_DELTA 500
#define ELANPRESS_BG_ATTEMPTS 8
/* first firmware that answers the calibration-mean command */
#define ELANPRESS_MIN_CALIBRATION_FW 0x0138

/* quality gates on a frame; raw sensor units. A finger raises the mean by
 * roughly 1500 on the 0c3d pad. Both are env-tunable, see elanpress.c */
#define ELANPRESS_MIN_MEAN 150.0
#define ELANPRESS_MIN_CONTRAST 40.0
#define ELANPRESS_MIN_COVERAGE 0.35

G_DECLARE_FINAL_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FPI,
                      DEVICE_ELANPRESS, FpDevice);

static const FpIdEntry elanpress_id_table[] = {
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0c3d, },
  {.vid = 0, .pid = 0, },
};
