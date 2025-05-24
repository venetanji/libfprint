/*
 * ChromeOS Fingerprint driver for libfprint
 *
 * Copyright (C) 2024 Abhinav Baid <abhinavbaid@gmail.com>
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

#include <config.h>
#include <stdint.h>
#include <sys/ioctl.h>

#ifndef HAVE_UDEV
#error "crfpmoc requires udev"
#endif

#include "drivers_api.h"

G_DECLARE_FINAL_TYPE (FpiDeviceCrfpMoc, fpi_device_crfpmoc, FPI, DEVICE_CRFPMOC, FpDevice)

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#define CRFPMOC_DRIVER_FULLNAME "ChromeOS Fingerprint Match-on-Chip"

#define CRFPMOC_NR_ENROLL_STAGES 5

/* crfpmoc_ec_command return value for non-success result from EC */
#define CRFPMOC_EECRESULT 1000

/* Resend last response (not supported on LPC). */
#define CRFPMOC_EC_CMD_RESEND_RESPONSE 0x00DB
/* Configure the Fingerprint MCU behavior */
#define CRFPMOC_EC_CMD_FP_MODE 0x0402
#define CRFPMOC_EC_CMD_FP_INFO 0x0403
#define CRFPMOC_EC_CMD_FP_STATS 0x0407
#define CRFPMOC_EC_CMD_FP_SEED 0x0408

/* Put the sensor in its lowest power mode */
#define CRFPMOC_FP_MODE_DEEPSLEEP BIT (0)
/* Wait to see a finger on the sensor */
#define CRFPMOC_FP_MODE_FINGER_DOWN BIT (1)
/* Poll until the finger has left the sensor */
#define CRFPMOC_FP_MODE_FINGER_UP BIT (2)
/* Capture the current finger image */
#define CRFPMOC_FP_MODE_CAPTURE BIT (3)
/* Finger enrollment session on-going */
#define CRFPMOC_FP_MODE_ENROLL_SESSION BIT (4)
/* Enroll the current finger image */
#define CRFPMOC_FP_MODE_ENROLL_IMAGE BIT (5)
/* Try to match the current finger image */
#define CRFPMOC_FP_MODE_MATCH BIT (6)
/* Reset and re-initialize the sensor. */
#define CRFPMOC_FP_MODE_RESET_SENSOR BIT (7)
/* special value: don't change anything just read back current mode */
#define CRFPMOC_FP_MODE_DONT_CHANGE BIT (31)

#define CRFPMOC_FPSTATS_CAPTURE_INV BIT (0)
#define CRFPMOC_FPSTATS_MATCHING_INV BIT (1)

/* Version of the format of the encrypted templates. */
#define CRFPMOC_FP_TEMPLATE_FORMAT_VERSION 4

/* Constants for encryption parameters */
#define CRFPMOC_FP_CONTEXT_NONCE_BYTES 12
#define CRFPMOC_FP_CONTEXT_USERID_WORDS (32 / sizeof (guint32))
#define CRFPMOC_FP_CONTEXT_TAG_BYTES 16
#define CRFPMOC_FP_CONTEXT_ENCRYPTION_SALT_BYTES 16
#define CRFPMOC_FP_CONTEXT_TPM_BYTES 32

#define CRFPMOC_EC_CMD_FP_FRAME 0x0404

/* Load a template into the MCU */
#define CRFPMOC_EC_CMD_FP_TEMPLATE 0x0405
/* Flag in the 'size' field indicating that the full template has been sent */
#define CRFPMOC_FP_TEMPLATE_COMMIT 0x80000000

/* Should be made dynamic */
#define CRFPMOC_DEFAULT_SEED "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define CRFPMOC_DEFAULT_CONTEXT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* constants defining the 'offset' field which also contains the frame index */
#define CRFPMOC_FP_FRAME_INDEX_SHIFT 28
/* Frame buffer where the captured image is stored */
#define CRFPMOC_FP_FRAME_INDEX_RAW_IMAGE 0
/* First frame buffer holding a template */
#define CRFPMOC_FP_FRAME_INDEX_TEMPLATE 1
#define CRFPMOC_FP_FRAME_GET_BUFFER_INDEX(offset) ((offset) >> FP_FRAME_INDEX_SHIFT)
#define CRFPMOC_FP_FRAME_OFFSET_MASK 0x0FFFFFFF

#define CRFPMOC_EC_CMD_GET_PROTOCOL_INFO 0x000B

#define CRFPMOC_EC_CMD_FP_ENC_STATUS 0x0409

/* FP TPM seed has been set or not */
#define CRFPMOC_FP_ENC_STATUS_SEED_SET BIT (0)


struct crfpmoc_ec_params_fp_frame
{
  /*
   * The offset contains the template index or FP_FRAME_INDEX_RAW_IMAGE
   * in the high nibble, and the real offset within the frame in
   * FP_FRAME_OFFSET_MASK.
   */
  guint32 offset;
  guint32 size;
} __attribute__((packed));

struct crfpmoc_ec_params_fp_template
{
  guint32 offset;
  guint32 size;
  guint8  data[];
} __attribute__((packed));

struct crfpmoc_ec_response_get_protocol_info
{
  /* Fields which exist if at least protocol version 3 supported */
  guint32 protocol_versions;
  guint16 max_request_packet_size;
  guint16 max_response_packet_size;
  guint32 flags;
} __attribute__((packed));

/* crfpmoc_ec_host_response and crfpmoc_ec_host_request are only here for the size of the struct */
struct crfpmoc_ec_host_response
{
  guint8  struct_version;
  guint8  checksum;
  guint16 result;
  guint16 data_len;
  guint16 reserved;
} __attribute__((packed));

struct crfpmoc_ec_host_request
{
  guint8  struct_version;
  guint8  checksum;
  guint16 command;
  guint8  command_version;
  guint8  reserved;
  guint16 data_len;
} __attribute__((packed));

struct crfpmoc_ec_response_fp_encryption_status
{
  /* Used bits in encryption engine status */
  guint32 valid_flags;
  /* Encryption engine status */
  guint32 status;
} __attribute__((packed));

struct crfpmoc_ec_params_fp_mode
{
  guint32 mode; /* as defined by CRFPMOC_FP_MODE_ constants */
} __attribute__((packed));

struct crfpmoc_ec_response_fp_mode
{
  guint32 mode; /* as defined by CRFPMOC_FP_MODE_ constants */
} __attribute__((packed));

struct crfpmoc_ec_response_fp_stats
{
  guint32 capture_time_us;
  guint32 matching_time_us;
  guint32 overall_time_us;
  struct
  {
    guint32 lo;
    guint32 hi;
  } overall_t0;
  guint8 timestamps_invalid;
  gint8  template_matched;
} __attribute__((packed));

struct crfpmoc_ec_params_fp_seed
{
  /*
   * Version of the structure format (N=3).
   */
  guint16 struct_version;
  /* Reserved bytes, set to 0. */
  guint16 reserved;
  /* Seed from the TPM. */
  guint8  seed[CRFPMOC_FP_CONTEXT_TPM_BYTES];
} __attribute__((packed));

/* Clear the current fingerprint user context and set a new one */
#define CRFPMOC_EC_CMD_FP_CONTEXT 0x0406

enum crfpmoc_fp_context_action {
  CRFPMOC_FP_CONTEXT_ASYNC = 0,
  CRFPMOC_FP_CONTEXT_GET_RESULT = 1,
};

/* Version 1 of the command is "asynchronous". */
struct crfpmoc_ec_params_fp_context_v1
{
  guint8  action;      /**< enum fp_context_action */
  guint8  reserved[3]; /**< padding for alignment */
  guint32 userid[CRFPMOC_FP_CONTEXT_USERID_WORDS];
} __attribute__((packed));

struct crfpmoc_ec_response_fp_info
{
  /* Sensor identification */
  guint32 vendor_id;
  guint32 product_id;
  guint32 model_id;
  guint32 version;
  /* Image frame characteristics */
  guint32 frame_size;
  guint32 pixel_format;
  guint16 width;
  guint16 height;
  guint16 bpp;
  guint16 errors;
  /* Template/finger current information */
  guint32 template_size;    /* max template size in bytes */
  guint16 template_max;     /* maximum number of fingers/templates */
  guint16 template_valid;   /* number of valid fingers/templates */
  guint32 template_dirty;   /* bitmap of templates with MCU side changes */
  guint32 template_version; /* version of the template format */
} __attribute__((packed));

/* Note: used in crfpmoc_ec_response_get_next_data_v1 */
struct crfpmoc_ec_response_motion_sense_fifo_info
{
  /* Size of the fifo */
  guint16 size;
  /* Amount of space used in the fifo */
  guint16 count;
  /* Timestamp recorded in us.
   * aka accurate timestamp when host event was triggered.
   */
  guint32 timestamp;
  /* Total amount of vector lost */
  guint16 total_lost;
  /* Lost events since the last fifo_info, per sensors */
  guint16 lost[0];
};

#define CRFPMOC_EC_MKBP_HAS_MORE_EVENTS_SHIFT 7

/*
 * We use the most significant bit of the event type to indicate to the host
 * that the EC has more MKBP events available to provide.
 */
#define CRFPMOC_EC_MKBP_HAS_MORE_EVENTS BIT (CRFPMOC_EC_MKBP_HAS_MORE_EVENTS_SHIFT)

/* The mask to apply to get the raw event type */
#define CRFPMOC_EC_MKBP_EVENT_TYPE_MASK (BIT (CRFPMOC_EC_MKBP_HAS_MORE_EVENTS_SHIFT) - 1)

enum ec_mkbp_event {
  /* Keyboard matrix changed. The event data is the new matrix state. */
  CRFPMOC_EC_MKBP_EVENT_KEY_MATRIX = 0,

  /* New host event. The event data is 4 bytes of host event flags. */
  CRFPMOC_EC_MKBP_EVENT_HOST_EVENT = 1,

  /* New Sensor FIFO data. The event data is fifo_info structure. */
  CRFPMOC_EC_MKBP_EVENT_SENSOR_FIFO = 2,

  /* The state of the non-matrixed buttons have changed. */
  CRFPMOC_EC_MKBP_EVENT_BUTTON = 3,

  /* The state of the switches have changed. */
  CRFPMOC_EC_MKBP_EVENT_SWITCH = 4,

  /* New Fingerprint sensor event, the event data is fp_events bitmap. */
  CRFPMOC_EC_MKBP_EVENT_FINGERPRINT = 5,

  /*
   * Sysrq event: send emulated sysrq. The event data is sysrq,
   * corresponding to the key to be pressed.
   */
  CRFPMOC_EC_MKBP_EVENT_SYSRQ = 6,

  /*
   * New 64-bit host event.
   * The event data is 8 bytes of host event flags.
   */
  CRFPMOC_EC_MKBP_EVENT_HOST_EVENT64 = 7,

  /* Notify the AP that something happened on CEC */
  CRFPMOC_EC_MKBP_EVENT_CEC_EVENT = 8,

  /* Send an incoming CEC message to the AP */
  CRFPMOC_EC_MKBP_EVENT_CEC_MESSAGE = 9,

  /* We have entered DisplayPort Alternate Mode on a Type-C port. */
  CRFPMOC_EC_MKBP_EVENT_DP_ALT_MODE_ENTERED = 10,

  /* New online calibration values are available. */
  CRFPMOC_EC_MKBP_EVENT_ONLINE_CALIBRATION = 11,

  /* Peripheral device charger event */
  CRFPMOC_EC_MKBP_EVENT_PCHG = 12,

  /* Number of MKBP events */
  CRFPMOC_EC_MKBP_EVENT_COUNT,
};

/* Fingerprint events in 'fp_events' for CRFPMOC_EC_MKBP_EVENT_FINGERPRINT */
#define CRFPMOC_EC_MKBP_FP_RAW_EVENT(fp_events) ((fp_events) & 0x00FFFFFF)
#define CRFPMOC_EC_MKBP_FP_ERRCODE(fp_events) ((fp_events) & 0x0000000F)
#define CRFPMOC_EC_MKBP_FP_ENROLL_PROGRESS_OFFSET 4
#define CRFPMOC_EC_MKBP_FP_ENROLL_PROGRESS(fpe) \
  (((fpe) & 0x00000FF0) >> CRFPMOC_EC_MKBP_FP_ENROLL_PROGRESS_OFFSET)
#define CRFPMOC_EC_MKBP_FP_MATCH_IDX_OFFSET 12
#define CRFPMOC_EC_MKBP_FP_MATCH_IDX_MASK 0x0000F000
#define CRFPMOC_EC_MKBP_FP_MATCH_IDX(fpe) \
  (((fpe) & CRFPMOC_EC_MKBP_FP_MATCH_IDX_MASK) >> CRFPMOC_EC_MKBP_FP_MATCH_IDX_OFFSET)
#define CRFPMOC_EC_MKBP_FP_ENROLL BIT (27)
#define CRFPMOC_EC_MKBP_FP_MATCH BIT (28)
#define CRFPMOC_EC_MKBP_FP_FINGER_DOWN BIT (29)
#define CRFPMOC_EC_MKBP_FP_FINGER_UP BIT (30)
#define CRFPMOC_EC_MKBP_FP_IMAGE_READY BIT (31)
/* code given by CRFPMOC_EC_MKBP_FP_ERRCODE() when CRFPMOC_EC_MKBP_FP_ENROLL is set */
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_OK 0
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_LOW_QUALITY 1
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_IMMOBILE 2
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_LOW_COVERAGE 3
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_INTERNAL 5
/* Can be used to detect if image was usable for enrollment or not. */
#define CRFPMOC_EC_MKBP_FP_ERR_ENROLL_PROBLEM_MASK 1
/* code given by CRFPMOC_EC_MKBP_FP_ERRCODE() when CRFPMOC_EC_MKBP_FP_MATCH is set */
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO 0
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_INTERNAL 6
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_TEMPLATES 7
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_AUTH_FAIL 8
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_LOW_QUALITY 2
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_LOW_COVERAGE 4
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES 1
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES_UPDATED 3
#define CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES_UPDATE_FAILED 5

union __attribute__((packed)) crfpmoc_ec_response_get_next_data_v1
{
  guint8 key_matrix[16];

  /* Unaligned */
  guint32 host_event;
  guint64 host_event64;

  struct
  {
    /* For aligning the fifo_info */
    guint8                                            reserved[3];
    struct crfpmoc_ec_response_motion_sense_fifo_info info;
  } sensor_fifo;

  guint32 buttons;
  guint32 switches;
  guint32 fp_events;
  guint32 sysrq;
  guint32 cec_events;
  guint8  cec_message[16];
};

struct crfpmoc_ec_response_get_next_event_v1
{
  guint8                                     event_type;
  /* Followed by event data if any */
  union crfpmoc_ec_response_get_next_data_v1 data;
} __attribute__((packed));

/*
 * @version: Command version number (often 0)
 * @command: Command to send (CRFPMOC_EC_CMD_...)
 * @outsize: Outgoing length in bytes
 * @insize: Max number of bytes to accept from EC
 * @result: EC's response to the command (separate from communication failure)
 * @data: Where to put the incoming data from EC and outgoing data to EC
 */
struct crfpmoc_cros_ec_command_v2
{
  guint32 version;
  guint32 command;
  guint32 outsize;
  guint32 insize;
  guint32 result;
  guint8  data[0];
} __attribute__((packed));

#define CRFPMOC_CROS_EC_DEV_IOC_V2 0xEC
#define CRFPMOC_CROS_EC_DEV_IOCXCMD_V2 \
  _IOWR (CRFPMOC_CROS_EC_DEV_IOC_V2, 0, struct crfpmoc_cros_ec_command_v2)
#define CRFPMOC_CROS_EC_DEV_IOCEVENTMASK_V2 _IO (CRFPMOC_CROS_EC_DEV_IOC_V2, 2)

/* Parameter length was limited by the LPC interface */
#define CRFPMOC_EC_PROTO2_MAX_PARAM_SIZE 0xfc

/*
 * Host command response codes (16-bit).
 */
enum crfpmoc_ec_status {
  EC_RES_SUCCESS = 0,
  EC_RES_INVALID_COMMAND = 1,
  EC_RES_ERROR = 2,
  EC_RES_INVALID_PARAM = 3,
  EC_RES_ACCESS_DENIED = 4,
  EC_RES_INVALID_RESPONSE = 5,
  EC_RES_INVALID_VERSION = 6,
  EC_RES_INVALID_CHECKSUM = 7,
  EC_RES_IN_PROGRESS = 8,             /* Accepted, command in progress */
  EC_RES_UNAVAILABLE = 9,             /* No response available */
  EC_RES_TIMEOUT = 10,                /* We got a timeout */
  EC_RES_OVERFLOW = 11,               /* Table / data overflow */
  EC_RES_INVALID_HEADER = 12,         /* Header contains invalid data */
  EC_RES_REQUEST_TRUNCATED = 13,      /* Didn't get the entire request */
  EC_RES_RESPONSE_TOO_BIG = 14,       /* Response was too big to handle */
  EC_RES_BUS_ERROR = 15,              /* Communications bus error */
  EC_RES_BUSY = 16,                   /* Up but too busy.  Should retry */
  EC_RES_INVALID_HEADER_VERSION = 17, /* Header version invalid */
  EC_RES_INVALID_HEADER_CRC = 18,     /* Header CRC invalid */
  EC_RES_INVALID_DATA_CRC = 19,       /* Data CRC invalid */
  EC_RES_DUP_UNAVAILABLE = 20,        /* Can't resend response */

  EC_RES_COUNT,

  EC_RES_MAX = UINT16_MAX, /**< Force enum to be 16 bits */
} __attribute__((packed));

/* SSM task states and various status enums */

typedef enum {
  ENROLL_ENSURE_KEYS,
  ENROLL_SENSOR_ENROLL,
  ENROLL_WAIT_ENROLL_COMPLETE,
  ENROLL_SENSOR_CHECK,
  ENROLL_COMMIT,
  ENROLL_STATES,
} EnrollStates;

typedef enum {
  VERIFY_ENSURE_KEYS,
  VERIFY_UPLOAD_TEMPLATE,
  VERIFY_SENSOR_MATCH,
  VERIFY_WAIT_MATCH_COMPLETE,
  VERIFY_SENSOR_CHECK,
  VERIFY_CHECK,
  VERIFY_STATES,
} VerifyStates;

typedef enum {
  CLEAR_STORAGE_SENSOR_RESET,
  CLEAR_STORAGE_SENSOR_WAIT,
  CLEAR_STORAGE_SENSOR_DONE,
  CLEAR_STORAGE_STATES,
} ClearStorageStates;
