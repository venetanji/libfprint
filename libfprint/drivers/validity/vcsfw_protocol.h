/*
 * VCSFW protocol definitions for Validity/Synaptics fingerprint sensors
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

#include "validity.h"

/* ---- VCSFW Command IDs (pre-TLS) ---- */
#define VCSFW_CMD_GET_VERSION 0x01
#define VCSFW_CMD_READ_HW_REG32 0x07
#define VCSFW_CMD_WRITE_HW_REG32 0x08
#define VCSFW_CMD_UNKNOWN_INIT 0x19
#define VCSFW_CMD_GET_FLASH_INFO 0x3E
#define VCSFW_CMD_READ_FLASH 0x40
#define VCSFW_CMD_WRITE_FLASH 0x41
#define VCSFW_CMD_WRITE_FW_SIG 0x42
#define VCSFW_CMD_GET_FW_INFO 0x43
#define VCSFW_CMD_PARTITION_FLASH 0x4F

/* ---- VCSFW Command IDs (post-TLS, via tls.app()) ---- */
#define VCSFW_CMD_CAPTURE 0x02
#define VCSFW_CMD_CAPTURE_STOP 0x04
#define VCSFW_CMD_GLOW_START 0x39
#define VCSFW_CMD_ERASE_FLASH 0x3F
#define VCSFW_CMD_DB_INFO 0x45
#define VCSFW_CMD_GET_RECORD_CHILDREN 0x46
#define VCSFW_CMD_NEW_RECORD 0x47
#define VCSFW_CMD_DEL_RECORD 0x48
#define VCSFW_CMD_GET_RECORD_VALUE 0x49
#define VCSFW_CMD_GET_USER 0x4A
#define VCSFW_CMD_GET_USER_STORAGE 0x4B
#define VCSFW_CMD_GET_PRG_STATUS 0x51
#define VCSFW_CMD_MATCH_FINGER 0x5E
#define VCSFW_CMD_GET_MATCH_RESULT 0x60
#define VCSFW_CMD_MATCH_CLEANUP 0x62
#define VCSFW_CMD_ENROLLMENT_UPDATE_START 0x68
#define VCSFW_CMD_CREATE_ENROLLMENT 0x69
#define VCSFW_CMD_ENROLLMENT_UPDATE 0x6B
#define VCSFW_CMD_GET_FACTORY_BITS 0x6F
#define VCSFW_CMD_IDENTIFY_SENSOR 0x75

/* ---- VCSFW Response Status Codes ---- */
#define VCSFW_STATUS_OK 0x0000
#define VCSFW_STATUS_NO_FW 0xB004

/* ---- Callback types ---- */

/**
 * VcsfwCmdCallback:
 * @self: the validity device
 * @data: response data (after 2-byte status, NULL on error)
 * @data_len: length of response data (excluding 2-byte status)
 * @status: the 2-byte VCSFW status code
 * @error: a GError if the transfer failed, or NULL
 *
 * Callback invoked after a VCSFW command/response exchange completes.
 */
typedef void (*VcsfwCmdCallback) (FpiDeviceValidity *self,
                                  const guint8      *data,
                                  gsize              data_len,
                                  guint16            status,
                                  GError            *error);

/* ---- Command/response SSM states ---- */
typedef enum {
  VCSFW_CMD_STATE_SEND = 0,
  VCSFW_CMD_STATE_RECV,
  VCSFW_CMD_STATE_NUM_STATES,
} VcsfwCmdSsmState;

/* ---- TLS-wrapped command/response SSM states ---- */
typedef enum {
  VCSFW_TLS_CMD_STATE_SEND = 0,
  VCSFW_TLS_CMD_STATE_RECV,
  VCSFW_TLS_CMD_STATE_NUM_STATES,
} VcsfwTlsCmdSsmState;

/* ---- Context for a single command/response exchange ---- */
typedef struct
{
  guint8          *cmd_data;
  gsize            cmd_len;
  VcsfwCmdCallback callback;
} VcsfwCmdData;

/* ---- Functions ---- */

void vcsfw_cmd_run_state (FpiSsm   *ssm,
                          FpDevice *dev);

VcsfwCmdData *vcsfw_cmd_data_new (const guint8    *cmd,
                                  gsize            cmd_len,
                                  VcsfwCmdCallback callback);

void vcsfw_cmd_data_free (gpointer data);

void vcsfw_cmd_send (FpiDeviceValidity *self,
                     FpiSsm            *parent_ssm,
                     const guint8      *cmd,
                     gsize              cmd_len,
                     VcsfwCmdCallback   callback);

void vcsfw_tls_cmd_run_state (FpiSsm   *ssm,
                              FpDevice *dev);

void vcsfw_tls_cmd_send (FpiDeviceValidity *self,
                         FpiSsm            *parent_ssm,
                         const guint8      *cmd,
                         gsize              cmd_len,
                         VcsfwCmdCallback   callback);

gboolean vcsfw_parse_version (const guint8        *data,
                              gsize                data_len,
                              ValidityVersionInfo *info);
