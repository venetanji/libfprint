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

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "fpi-byte-utils.h"
#include "validity.h"
#include "validity_fwext.h"
#include "validity_hal.h"
#include "vcsfw_protocol.h"

#include "fpi-byte-writer.h"
#include "validity_pack.h"
#include <gio/gio.h>
#include <string.h>

/* ---- Constants ---- */

#define FWEXT_CHUNK_SIZE 0x1000              /* 4 KB per write_flash chunk */
#define FWEXT_SIGNATURE_SIZE 256             /* RSA signature length */
#define FWEXT_HEADER_DELIMITER 0x1A          /* .xpfwext header end marker */

#define FWEXT_HW_REG_WRITE_ADDR 0x8000205C
#define FWEXT_HW_REG_WRITE_VALUE 7
#define FWEXT_HW_REG_READ_ADDR 0x80002080

/* Firmware partition */
#define FWEXT_PARTITION 2

/* Reboot command: 0x05 0x02 0x00 */
#define VCSFW_CMD_REBOOT 0x05
#define VCSFW_REBOOT_SUBCMD 0x02

/* Cleanup command (call_cleanups): 0x1a */
#define VCSFW_CMD_CLEANUP 0x1A

/* ---- Firmware file search paths ---- */

static const gchar *firmware_search_paths[] = {
  "/usr/share/libfprint/validity",
  "/var/lib/python-validity",
  "/var/run/python-validity",
  "/usr/share/python-validity",
  NULL,
};

/* db_write_enable blobs now live in HAL (validity_hal.c) */

/* ================================================================
 * Firmware info parsing
 * ================================================================ */

gboolean
validity_fwext_parse_fw_info (const guint8   *data,
                              gsize           data_len,
                              guint16         status,
                              ValidityFwInfo *info)
{
  memset (info, 0, sizeof (*info));

  /* Status 0xB004 (bytes: 0xb0 0x04) means no firmware loaded.
   * python-validity checks: rsp[0]==0xb0 and rsp[1]==4
   * Our vcsfw_cmd_send reads status as uint16 LE from the first 2 bytes,
   * so wire bytes {0xb0, 0x04} -> status = 0x04B0 in LE. */
  if (status == VCSFW_STATUS_NO_FW)
    {
      info->loaded = FALSE;
      return TRUE;
    }

  if (status != VCSFW_STATUS_OK)
    {
      fp_warn ("GET_FW_INFO unexpected status: 0x%04x", status);
      info->loaded = FALSE;
      return FALSE;
    }

  /* Response data (after 2-byte status stripped by vcsfw_cmd_send):
   * major(2) + minor(2) + modcnt(2) + buildtime(4) = 10 bytes header
   * + 12 bytes per module */
  if (data_len < 10)
    {
      fp_warn ("GET_FW_INFO response too short: %zu", data_len);
      info->loaded = FALSE;
      return FALSE;
    }

  info->loaded = TRUE;

  if (!validity_unpack (data, data_len, "hhhw",
                        &info->major, &info->minor,
                        &info->module_count, &info->buildtime))
    {
      info->loaded = FALSE;
      return FALSE;
    }

  if (info->module_count > 32)
    info->module_count = 32;

  for (guint16 i = 0; i < info->module_count; i++)
    {
      if (!validity_unpack (data + 10 + i * 12, data_len - 10 - i * 12,
                            "hhhhw",
                            &info->modules[i].type,
                            &info->modules[i].subtype,
                            &info->modules[i].major,
                            &info->modules[i].minor,
                            &info->modules[i].size))
        break;
    }

  return TRUE;
}

/* ================================================================
 * Firmware filename mapping
 *
 * The firmware extension differs per OEM, and a chip can reject another
 * OEM's file (06cb:00b7 answers Lenovo's with 044f, signature validation
 * failed), so each device needs the right one. The files are not shipped
 * with libfprint: the driver only names the vendor package that ships each
 * file, for the missing-file error. Each package below was checked to
 * contain its file.
 *
 * A sensor that has run the vendor driver (or python-validity) already has
 * the extension in flash, so the upload only happens on a factory-empty or
 * erased chip. The HP mappings follow python-validity PR #256; upstream
 * python-validity has no HP devices. The uploads marked verified below were
 * done by python-validity on real hardware; none has yet gone through this
 * driver's upload path.
 * ================================================================ */

typedef struct
{
  const gchar *filename;
  const gchar *source;   /* vendor package that ships the file */
} FwextFileEntry;

static const FwextFileEntry fwext_lenovo = {
  "6_07f_Lenovo.xpfwext",
  "Lenovo's fingerprint driver installer n1cgn08w.exe",
};

static const FwextFileEntry fwext_lenovo_mis_qm = {
  "6_07f_lenovo_mis_qm.xpfwext",
  "Lenovo's fingerprint driver installer nz3gf09w.exe",
};

static const FwextFileEntry fwext_hp_cmit_mis_qm = {
  "6_07f_hp_cmit_mis_qm.xpfwext",
  "HP SoftPaq sp135736",
};

static const FwextFileEntry fwext_hp_mis_qm = {
  "6_07f_hp_mis_qm.xpfwext",
  "HP SoftPaq sp138431",
};

static const struct
{
  guint16               vid;
  guint16               pid;
  const FwextFileEntry *file;
} fwext_device_map[] = {
  { 0x138a, 0x0090, &fwext_lenovo },
  { 0x138a, 0x0097, &fwext_lenovo_mis_qm },
  { 0x138a, 0x009d, &fwext_lenovo_mis_qm },
  { 0x06cb, 0x009a, &fwext_lenovo_mis_qm },

  /* 138a:00ab spans two silicon variants. Verified on 0xd51 (EliteBook 840
   * G5) in PR #256, from zero partitions through upload and enroll. NOT
   * verified on 0x969 (ZBook Studio x360 G5): HP's driver for 00ab ships only
   * this file, but no upload has been seen on that silicon. */
  { 0x138a, 0x00ab, &fwext_hp_cmit_mis_qm },
  /* Verified on 0xd51 (EliteBook 840 G6) by @gfiguero: the chip rejects the
   * Lenovo file with 044f and accepts this one. */
  { 0x06cb, 0x00b7, &fwext_hp_cmit_mis_qm },
  /* Verified on 0x969 (Pavilion x360 14-dh), uploaded after a factory
   * reset. */
  { 0x06cb, 0x00cb, &fwext_hp_mis_qm },
  /* NOT verified: same 0x969 silicon as 00cb, but sp138431 does not list
   * 00d2 and no upload has been seen on it. */
  { 0x06cb, 0x00d2, &fwext_hp_mis_qm },
};

const gchar *
validity_fwext_get_firmware_name (guint16       vid,
                                  guint16       pid,
                                  const gchar **source)
{
  for (gsize i = 0; i < G_N_ELEMENTS (fwext_device_map); i++)
    {
      if (fwext_device_map[i].vid == vid && fwext_device_map[i].pid == pid)
        {
          if (source)
            *source = fwext_device_map[i].file->source;
          return fwext_device_map[i].file->filename;
        }
    }

  if (source)
    *source = NULL;
  return NULL;
}

gchar *
validity_fwext_find_firmware (guint16  vid,
                              guint16  pid,
                              GError **error)
{
  const gchar *source = NULL;
  const gchar *filename = validity_fwext_get_firmware_name (vid, pid, &source);

  if (filename == NULL)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED,
                   "No firmware filename known for %04x:%04x", vid, pid);
      return NULL;
    }

  for (const gchar **path = firmware_search_paths; *path != NULL; path++)
    {
      g_autofree gchar *full_path = g_build_filename (*path, filename, NULL);

      if (g_file_test (full_path, G_FILE_TEST_IS_REGULAR))
        {
          fp_info ("Found firmware file: %s", full_path);
          return g_steal_pointer (&full_path);
        }
    }

  g_autofree gchar *searched = g_strjoinv (", ",
                                           (gchar **) firmware_search_paths);

  g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_NOT_FOUND,
               "Firmware file '%s' not found (looked in %s). "
               "Extract it from %s.",
               filename, searched, source);
  return NULL;
}

/* ================================================================
 * .xpfwext file parser
 * ================================================================ */

gboolean
validity_fwext_load_file (const gchar       *path,
                          ValidityFwextFile *fwext,
                          GError           **error)
{
  g_autofree guint8 *contents = NULL;
  gsize length = 0;

  memset (fwext, 0, sizeof (*fwext));

  if (!g_file_get_contents (path, (gchar **) &contents, &length, error))
    return FALSE;

  /* Find 0x1A delimiter that ends the header */
  const guint8 *delim = memchr (contents, FWEXT_HEADER_DELIMITER, length);

  if (delim == NULL)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Firmware file has no 0x1A header delimiter");
      return FALSE;
    }

  gsize header_len = (delim - contents) + 1;  /* includes 0x1A itself */
  gsize data_len = length - header_len;

  if (data_len < FWEXT_SIGNATURE_SIZE + 1)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Firmware file too short after header (%zu bytes)", data_len);
      return FALSE;
    }

  /* Split: payload = data[:-256], signature = data[-256:] */
  fwext->payload_len = data_len - FWEXT_SIGNATURE_SIZE;
  fwext->payload = g_memdup2 (contents + header_len, fwext->payload_len);
  memcpy (fwext->signature,
          contents + header_len + fwext->payload_len,
          FWEXT_SIGNATURE_SIZE);

  fp_info ("Firmware file loaded: %zu bytes payload, %d bytes signature",
           fwext->payload_len, FWEXT_SIGNATURE_SIZE);

  return TRUE;
}

void
validity_fwext_file_clear (ValidityFwextFile *fwext)
{
  g_clear_pointer (&fwext->payload, g_free);
  fwext->payload_len = 0;
}

/* ================================================================
 * Command builders
 * ================================================================ */

void
validity_fwext_build_write_hw_reg32 (guint32 addr,
                                     guint32 value,
                                     guint8 *cmd,
                                     gsize  *cmd_len)
{
  *cmd_len = validity_pack (cmd, 10, "bwwb",
                            VCSFW_CMD_WRITE_HW_REG32, addr, value, 4);
}

void
validity_fwext_build_read_hw_reg32 (guint32 addr,
                                    guint8 *cmd,
                                    gsize  *cmd_len)
{
  *cmd_len = validity_pack (cmd, 6, "bwb",
                            VCSFW_CMD_READ_HW_REG32, addr, 4);
}

gboolean
validity_fwext_parse_read_hw_reg32 (const guint8 *data,
                                    gsize         data_len,
                                    guint32      *value)
{
  return validity_unpack (data, data_len, "w", value);
}

void
validity_fwext_build_write_flash (guint8        partition,
                                  guint32       offset,
                                  const guint8 *data,
                                  gsize         data_len,
                                  guint8       *cmd,
                                  gsize        *cmd_len)
{
  *cmd_len = validity_pack (cmd, 13 + data_len, "bbbhwwd",
                            VCSFW_CMD_WRITE_FLASH, partition, 1,
                            (guint16) 0, offset, (guint32) data_len,
                            data, data_len);
}

void
validity_fwext_build_write_fw_sig (guint8        partition,
                                   const guint8 *signature,
                                   gsize         sig_len,
                                   guint8       *cmd,
                                   gsize        *cmd_len)
{
  *cmd_len = validity_pack (cmd, 5 + sig_len, "bbxhd",
                            VCSFW_CMD_WRITE_FW_SIG, partition,
                            (guint16) sig_len, signature, sig_len);
}

void
validity_fwext_build_reboot (guint8 *cmd,
                             gsize  *cmd_len)
{
  *cmd_len = validity_pack (cmd, 3, "bbb",
                            VCSFW_CMD_REBOOT, VCSFW_REBOOT_SUBCMD, 0x00);
}

/* ================================================================
 * db_write_enable blob lookup
 * ================================================================ */

const guint8 *
validity_fwext_get_db_write_enable (FpiDeviceValidity *self,
                                    gsize             *len)
{
  return validity_data_get_bytes (&self->device_data,
                                  VALIDITY_DATA_DB_WRITE_ENABLE,
                                  len);
}

/* ================================================================
 * Upload SSM -- firmware extension upload state machine
 *
 * This SSM is started as a standalone child from the open sequence
 * when fwext_loaded == FALSE. It uses the subsm pattern:
 * SEND states call vcsfw_tls_cmd_send(self, ssm, ..., NULL) with a
 * NULL callback. The child SSM auto-advances the parent to the
 * RECV state on completion. RECV states read
 * self->cmd_response_status and self->cmd_response_data.
 *
 * All commands are sent via TLS because the sensor requires
 * encrypted writes to flash partition 2.
 * ================================================================ */

/* SSM data for the upload state machine */
typedef struct
{
  ValidityFwextFile fwext;
  gsize             write_offset;    /* current offset into payload */
  guint16           vid;
  guint16           pid;
} FwextUploadData;

static void
fwext_upload_data_free (gpointer data)
{
  FwextUploadData *ud = data;

  validity_fwext_file_clear (&ud->fwext);
  g_free (ud);
}


static void
fwext_recv_read_hw_reg (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "READ_HW_REG failed: status=0x%04x",
                                                     self->cmd_response_status));
      return;
    }

  guint32 value;

  if (!validity_fwext_parse_read_hw_reg32 (self->cmd_response_data,
                                           self->cmd_response_len,
                                           &value))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "READ_HW_REG response too short"));
      return;
    }

  if (value != 2 && value != 3)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected HW register value: %u "
                                                     "(expected 2 or 3)", value));
      return;
    }

  fp_dbg ("FWEXT: HW register 0x%08x = %u (OK)", FWEXT_HW_REG_READ_ADDR, value);
  fpi_ssm_next_state (ssm);
}

static void
fwext_load_file (FpiSsm            *ssm,
                 FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FwextUploadData *ud = fpi_ssm_get_data (ssm);

  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  guint16 vid = g_usb_device_get_vid (usb_dev);
  guint16 pid = g_usb_device_get_pid (usb_dev);

  ud->vid = vid;
  ud->pid = pid;

  g_autofree gchar *fw_path = validity_fwext_find_firmware (vid, pid, &error);

  if (fw_path == NULL)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!validity_fwext_load_file (fw_path, &ud->fwext, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  ud->write_offset = 0;
  fp_info ("FWEXT: Loaded firmware file, %zu bytes to write",
           ud->fwext.payload_len);
  fpi_ssm_next_state (ssm);
}

static void
fwext_send_db_write_enable (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  gsize dbe_len;
  const guint8 *dbe = validity_fwext_get_db_write_enable (self, &dbe_len);

  if (dbe == NULL || dbe_len == 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                     "No db_write_enable data for "
                                                     "this device"));
      return;
    }

  vcsfw_tls_cmd_send (self, ssm, dbe, dbe_len, NULL);
}

static void
fwext_send_write_chunk (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  FwextUploadData *ud = fpi_ssm_get_data (ssm);

  gsize remaining = ud->fwext.payload_len - ud->write_offset;
  gsize chunk_size = MIN (remaining, FWEXT_CHUNK_SIZE);

  /* cmd buffer: 13-byte header + payload */
  g_autofree guint8 *cmd = g_malloc (13 + chunk_size);
  gsize cmd_len;

  validity_fwext_build_write_flash (FWEXT_PARTITION,
                                    (guint32) ud->write_offset,
                                    ud->fwext.payload + ud->write_offset,
                                    chunk_size,
                                    cmd, &cmd_len);

  fp_dbg ("FWEXT: Writing chunk at offset 0x%zx (%zu/%zu bytes)",
          ud->write_offset, ud->write_offset + chunk_size,
          ud->fwext.payload_len);

  ud->write_offset += chunk_size;

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
}

static void
fwext_recv_cleanup (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FwextUploadData *ud = fpi_ssm_get_data (ssm);

  /* Status 0x0491 means "nothing to commit" -- not an error */
  if (self->cmd_response_status != VCSFW_STATUS_OK &&
      self->cmd_response_status != 0x0491)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Cleanup cmd failed: "
                                                     "status=0x%04x",
                                                     self->cmd_response_status));
      return;
    }

  if (ud->write_offset < ud->fwext.payload_len)
    /* More chunks to write -- loop back to db_write_enable */
    fpi_ssm_jump_to_state (ssm, FWEXT_SEND_DB_WRITE_ENABLE);
  else
    /* All chunks written -- proceed to signature */
    fpi_ssm_next_state (ssm);
}

static void
fwext_send_write_signature (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  FwextUploadData *ud = fpi_ssm_get_data (ssm);

  guint8 cmd[5 + FWEXT_SIGNATURE_SIZE];
  gsize cmd_len;

  validity_fwext_build_write_fw_sig (FWEXT_PARTITION,
                                     ud->fwext.signature,
                                     FWEXT_SIGNATURE_SIZE,
                                     cmd, &cmd_len);

  fp_info ("FWEXT: Writing firmware signature (%d bytes)",
           FWEXT_SIGNATURE_SIZE);

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
}

static void
fwext_recv_verify (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  ValidityFwInfo info;

  if (!validity_fwext_parse_fw_info (self->cmd_response_data,
                                     self->cmd_response_len,
                                     self->cmd_response_status,
                                     &info) ||
      !info.loaded)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Firmware not detected after upload"));
      return;
    }

  fp_info ("FWEXT: Upload verified -- firmware v%d.%d, %d modules",
           info.major, info.minor, info.module_count);
  fpi_ssm_next_state (ssm);
}

void
validity_fwext_upload_run_state (FpiSsm   *ssm,
                                 FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FWEXT_SEND_WRITE_HW_REG:
      {
        guint8 cmd[10];
        gsize cmd_len;
        validity_fwext_build_write_hw_reg32 (FWEXT_HW_REG_WRITE_ADDR,
                                             FWEXT_HW_REG_WRITE_VALUE,
                                             cmd, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case FWEXT_RECV_WRITE_HW_REG:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "WRITE_HW_REG failed: status=0x%04x",
                                                         self->cmd_response_status));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case FWEXT_SEND_READ_HW_REG:
      {
        guint8 cmd[6];
        gsize cmd_len;
        validity_fwext_build_read_hw_reg32 (FWEXT_HW_REG_READ_ADDR,
                                            cmd, &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case FWEXT_RECV_READ_HW_REG:
      fwext_recv_read_hw_reg (ssm, self);
      break;

    case FWEXT_LOAD_FILE:
      fwext_load_file (ssm, self);
      break;

    case FWEXT_SEND_DB_WRITE_ENABLE:
      fwext_send_db_write_enable (ssm, self);
      break;

    case FWEXT_RECV_DB_WRITE_ENABLE:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "db_write_enable failed: "
                                                         "status=0x%04x",
                                                         self->cmd_response_status));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case FWEXT_SEND_WRITE_CHUNK:
      fwext_send_write_chunk (ssm, self);
      break;

    case FWEXT_RECV_WRITE_CHUNK:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "WRITE_FLASH chunk failed: "
                                                         "status=0x%04x",
                                                         self->cmd_response_status));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case FWEXT_SEND_CLEANUP:
      {
        guint8 cmd[] = { VCSFW_CMD_CLEANUP };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case FWEXT_RECV_CLEANUP:
      fwext_recv_cleanup (ssm, self);
      break;

    case FWEXT_SEND_WRITE_SIGNATURE:
      fwext_send_write_signature (ssm, self);
      break;

    case FWEXT_RECV_WRITE_SIGNATURE:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "WRITE_FW_SIG failed: "
                                                         "status=0x%04x",
                                                         self->cmd_response_status));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case FWEXT_SEND_VERIFY:
      {
        guint8 cmd[] = { VCSFW_CMD_GET_FW_INFO, FWEXT_PARTITION };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case FWEXT_RECV_VERIFY:
      fwext_recv_verify (ssm, self);
      break;

    case FWEXT_SEND_REBOOT:
      {
        guint8 cmd[3];
        gsize cmd_len;
        validity_fwext_build_reboot (cmd, &cmd_len);
        fp_info ("FWEXT: Rebooting sensor to activate new firmware");
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case FWEXT_RECV_REBOOT:
      fp_info ("FWEXT: Reboot sent. Device will re-enumerate.");
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

/* ---- SSM factory ---- */

FpiSsm *
validity_fwext_upload_ssm_new (FpDevice *dev)
{
  FpiSsm *ssm;
  FwextUploadData *ud;

  ssm = fpi_ssm_new (dev, validity_fwext_upload_run_state,
                     FWEXT_NUM_STATES);
  ud = g_new0 (FwextUploadData, 1);
  fpi_ssm_set_data (ssm, ud, fwext_upload_data_free);

  return ssm;
}
