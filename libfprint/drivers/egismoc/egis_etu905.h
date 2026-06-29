/*
 * Driver for Egis Technology (LighTuning) Match-On-Chip sensors
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

G_DECLARE_FINAL_TYPE (FpiDeviceEgisEtu905, fpi_device_egis_etu905, FPI, DEVICE_EGIS_ETU905, FpDevice)

#define EGIS_ETU905_DRIVER_FULLNAME "Egis Technology (LighTuning) Match-on-Chip"

#define EGIS_ETU905_DRIVER_CHECK_PREFIX_TYPE1 (1 << 0)
#define EGIS_ETU905_DRIVER_CHECK_PREFIX_TYPE2 (1 << 1)
#define EGIS_ETU905_DRIVER_MAX_ENROLL_STAGES_20 (1 << 2)

#define EGIS_ETU905_EP_CMD_OUT (0x02 | FPI_USB_ENDPOINT_OUT)
#define EGIS_ETU905_EP_CMD_IN (0x81 | FPI_USB_ENDPOINT_IN)
#define EGIS_ETU905_EP_CMD_INTERRUPT_IN 0x83

#define EGIS_ETU905_USB_CONTROL_TIMEOUT 5000
#define EGIS_ETU905_USB_SEND_TIMEOUT 5000
#define EGIS_ETU905_USB_RECV_TIMEOUT 5000
#define EGIS_ETU905_USB_INTERRUPT_TIMEOUT 0 /* No timeout for waiting for finger down */

#define EGIS_ETU905_USB_IN_RECV_LENGTH 4096
#define EGIS_ETU905_USB_INTERRUPT_IN_RECV_LENGTH 64

#define EGIS_ETU905_MAX_ENROLL_STAGES_DEFAULT 12
#define EGIS_ETU905_MAX_ENROLL_NUM 12
#define EGIS_ETU905_FINGERPRINT_DATA_SIZE 32
#define EGIS_ETU905_LIST_RESPONSE_PREFIX_SIZE 14
#define EGIS_ETU905_LIST_RESPONSE_SUFFIX_SIZE 2

#define EGIS_ETU905_PARA_4_SIZE 68

#define EGIS_ETU905_PARA_1_VALUE 0
#define EGIS_ETU905_PARA_2_VALUE 0
#define EGIS_ETU905_PARA_3_VALUE 32

/* standard prefixes for all read/writes */

static const guchar egis_etu905_write_prefix[] = {'E', 'G', 'I', 'S', 0x00, 0x00, 0x00, 0x01};

static const guchar egis_etu905_read_prefix[] = {'S', 'I', 'G', 'E', 0x00, 0x00, 0x00, 0x01};


/* hard-coded command payloads */

static const guchar cmd_fw_version[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x0c};
static const guchar rsp_fw_version_suffix[] = {0x90, 0x00};

static const guchar cmd_init[] = {0x00, 0x00, 0x00, 0x29, 0x50, 0x81, 0x80, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x12, 0x00, 0x45, 0x67, 0x69, 0x73,
                                  0x46, 0x50, 0x53, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x00, 0x51, 0xc6, 0xbd, 0x71};

static const guchar cmd_list[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x19, 0x04, 0x00, 0x00, 0x01, 0x40};

static const guchar cmd_sensor_reset[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x1a, 0x00, 0x00};

static const guchar cmd_sensor_check[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x17, 0x02, 0x00};

static const guchar cmd_sensor_identify[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x17, 0x01, 0x01};

static const guchar rsp_identify_match_suffix[] = {0x90, 0x00};

static const guchar rsp_identify_notmatch_suffix[] = {0x90, 0x04};

static const guchar cmd_enroll_starting[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x16, 0x01, 0x00, 0x00, 0x00, 0x20};

static const guchar cmd_sensor_start_capture[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x16, 0x02, 0x01};

static const guchar cmd_read_capture[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x16, 0x02, 0x02, 0x00, 0x00, 0x02};

static const guchar cmd_duplicate_check[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x16, 0x05, 0x00};

static const guchar rsp_read_success_prefix[] = {0x00, 0x00, 0x00, 0x04};
static const guchar rsp_read_success_suffix[] = {0x90, 0x00};
static const guchar rsp_read_offcenter_prefix[] = {0x00, 0x00, 0x00, 0x04};
static const guchar rsp_read_offcenter_suffix[] = {0x64, 0x91};
static const guchar rsp_read_dirty_prefix[] = {0x00, 0x00, 0x00, 0x02, 0x64};

static const guchar cmd_commit_starting[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x16, 0x05, 0x00, 0x00, 0x00, 0x20};

static const guchar cmd_enroll_discard[] = {0x00, 0x00, 0x00, 0x07, 0x50, 0x16, 0x04, 0x00, 0x00, 0x00, 0x20};

static const guchar cmd_identify_cancel[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x04, 0x01, 0x00};

static const guchar cmd_identify_cancel_result[] = {0x00, 0x00, 0x00, 0x04, 0x50, 0x04, 0x02, 0x00};

/* prefixes/suffixes and other things for dynamically created command payloads */

#define EGIS_ETU905_CHECK_BYTES_LENGTH 2
#define EGIS_ETU905_IDENTIFY_RESPONSE_PRINT_ID_OFFSET 46
#define EGIS_ETU905_CMD_CHECK_SEPARATOR_LENGTH 32

static const guchar cmd_new_print_prefix_type2[] = {0x00, 0x00, 0x00, 0x50, 0x50, 0x16, 0x03, 0x00, 0x00, 0x00, 0x49};

static const guchar cmd_delete_prefix[] = {0x50, 0x18, 0x04, 0x00, 0x00};
static const guchar rsp_delete_success_prefix[] = {0x00, 0x00, 0x00, 0x02, 0x90, 0x00};

static const guchar cmd_check_prefix_type1[] = {0x50, 0x17, 0x03, 0x00, 0x00};
static const guchar cmd_check_prefix_type2[] = {0x50, 0x17, 0x03, 0x80, 0x00};
static const guchar cmd_check_suffix[] = {0x00, 0x40};
static const guchar rsp_check_not_yet_enrolled_suffix[] = {0x90, 0x04};


/* SSM task states and various status enums */

typedef enum {
  CMD_SEND,
  CMD_GET,
  CMD_STATES,
} CommandStates;

typedef enum {
  DEV_GET_FW_VERSION,
  DEV_INIT_CONTROL,
  DEV_INIT_STATES,
} DeviceInitStates;

typedef enum {
  IDENTIFY_GET_ENROLLED_IDS,
  IDENTIFY_CHECK_ENROLLED_NUM,
  IDENTIFY_SENSOR_RESET,
  IDENTIFY_SENSOR_IDENTIFY,
  IDENTIFY_WAIT_FINGER,
  IDENTIFY_SENSOR_CHECK,
  IDENTIFY_CHECK,
  IDENTIFY_SEND_CANCEL_RESULT,
  IDENTIFY_COMPLETE_SENSOR_RESET,
  IDENTIFY_COMPLETE,
  IDENTIFY_STATES,
} IdentifyStates;

typedef enum {
  ENROLL_START,
  ENROLL_CAPTURE_SENSOR_RESET,
  ENROLL_CAPTURE_SENSOR_START_CAPTURE,
  ENROLL_CAPTURE_WAIT_FINGER,
  ENROLL_CAPTURE_READ_RESPONSE,
  ENROLL_DUPLICATE_CHECK,
  ENROLL_COMMIT_START,
  ENROLL_COMMIT,
  ENROLL_COMMIT_SENSOR_RESET,
  ENROLL_COMPLETE,
  ENROLL_STATES,
} EnrollStates;

typedef enum {
  ENROLL_STATUS_DEVICE_FULL,
  ENROLL_STATUS_DUPLICATE,
  ENROLL_STATUS_PARTIAL_OK,
  ENROLL_STATUS_RETRY,
  ENROLL_STATUS_COMPLETE,
} EnrollStatus;

typedef enum {
  LIST_GET_ENROLLED_IDS,
  LIST_RETURN_ENROLLED_PRINTS,
  LIST_STATES,
} ListStates;

typedef enum {
  DELETE_GET_ENROLLED_IDS,
  DELETE_DELETE,
  DELETE_STATES,
} DeleteStates;
