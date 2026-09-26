/*
 * Validity/Synaptics VCSFW firmware extension upload
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

/* ---- Firmware info from GET_FW_INFO response ---- */

typedef struct
{
  guint16 type;
  guint16 subtype;
  guint16 major;
  guint16 minor;
  guint32 size;
} ValidityFwModule;

typedef struct
{
  gboolean         loaded;
  guint16          major;
  guint16          minor;
  guint32          buildtime;
  guint16          module_count;
  ValidityFwModule modules[32];
} ValidityFwInfo;

/* ---- Parsed firmware extension file ---- */

typedef struct
{
  guint8 *payload;        /* firmware data (without header, without signature) */
  gsize   payload_len;
  guint8  signature[256]; /* 256-byte RSA signature */
} ValidityFwextFile;

/* ---- Firmware upload SSM states ----
 *
 * Each command has a SEND and RECV pair:
 * - SEND: calls vcsfw_cmd_send(self, ssm, cmd, len, NULL)
 *   which starts a child SSM (subsm). The parent is paused.
 * - RECV: entered automatically when child completes. Checks
 *   self->cmd_response_status and self->cmd_response_data.
 *   Advances or loops as needed.
 */
typedef enum {
  FWEXT_SEND_WRITE_HW_REG = 0,
  FWEXT_RECV_WRITE_HW_REG,
  FWEXT_SEND_READ_HW_REG,
  FWEXT_RECV_READ_HW_REG,
  FWEXT_LOAD_FILE,
  FWEXT_SEND_DB_WRITE_ENABLE,
  FWEXT_RECV_DB_WRITE_ENABLE,
  FWEXT_SEND_WRITE_CHUNK,
  FWEXT_RECV_WRITE_CHUNK,
  FWEXT_SEND_CLEANUP,
  FWEXT_RECV_CLEANUP,
  FWEXT_SEND_WRITE_SIGNATURE,
  FWEXT_RECV_WRITE_SIGNATURE,
  FWEXT_SEND_VERIFY,
  FWEXT_RECV_VERIFY,
  FWEXT_SEND_REBOOT,
  FWEXT_RECV_REBOOT,
  FWEXT_NUM_STATES,
} ValidityFwextSsmState;

/* ---- API ---- */

gboolean validity_fwext_parse_fw_info (const guint8   *data,
                                       gsize           data_len,
                                       guint16         status,
                                       ValidityFwInfo *info);

gboolean validity_fwext_load_file (const gchar       *filename,
                                   ValidityFwextFile *fwext,
                                   GError           **error);

void     validity_fwext_file_clear (ValidityFwextFile *fwext);

/* Firmware extension filename for @vid:@pid, or NULL if unknown. If @source
 * is not NULL it is set to the vendor package that ships the file (NULL when
 * the device is unknown), for error messages. */
const gchar *validity_fwext_get_firmware_name (guint16       vid,
                                               guint16       pid,
                                               const gchar **source);

gchar   *validity_fwext_find_firmware (guint16  vid,
                                       guint16  pid,
                                       GError **error);

void     validity_fwext_build_write_hw_reg32 (guint32 addr,
                                              guint32 value,
                                              guint8 *cmd,
                                              gsize  *cmd_len);

void     validity_fwext_build_read_hw_reg32 (guint32 addr,
                                             guint8 *cmd,
                                             gsize  *cmd_len);

gboolean validity_fwext_parse_read_hw_reg32 (const guint8 *data,
                                             gsize         data_len,
                                             guint32      *value);

void     validity_fwext_build_write_flash (guint8        partition,
                                           guint32       offset,
                                           const guint8 *data,
                                           gsize         data_len,
                                           guint8       *cmd,
                                           gsize        *cmd_len);

void     validity_fwext_build_write_fw_sig (guint8        partition,
                                            const guint8 *signature,
                                            gsize         sig_len,
                                            guint8       *cmd,
                                            gsize        *cmd_len);

void     validity_fwext_build_reboot (guint8 *cmd,
                                      gsize  *cmd_len);

typedef struct _FpiDeviceValidity FpiDeviceValidity;

const guint8 *validity_fwext_get_db_write_enable (FpiDeviceValidity *self,
                                                  gsize             *len);

/* SSM entry point for upload state machine */
void     validity_fwext_upload_run_state (FpiSsm   *ssm,
                                          FpDevice *dev);

/* Create the fwext upload SSM with data attached.
 * Caller starts it via fpi_ssm_start(). */
FpiSsm  *validity_fwext_upload_ssm_new (FpDevice *dev);
