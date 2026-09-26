/*
 * VCSFW protocol implementation for Validity/Synaptics fingerprint sensors
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
#include "validity_pack.h"
#include "vcsfw_protocol.h"

/* ---- VcsfwCmdData lifecycle ---- */

VcsfwCmdData *
vcsfw_cmd_data_new (const guint8    *cmd,
                    gsize            cmd_len,
                    VcsfwCmdCallback callback)
{
  VcsfwCmdData *data = g_new0 (VcsfwCmdData, 1);

  data->cmd_data = g_memdup2 (cmd, cmd_len);
  data->cmd_len = cmd_len;
  data->callback = callback;

  return data;
}

void
vcsfw_cmd_data_free (gpointer data)
{
  VcsfwCmdData *cmd_data = data;

  if (cmd_data == NULL)
    return;

  g_free (cmd_data->cmd_data);
  g_free (cmd_data);
}

static void
cmd_receive_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  VcsfwCmdData *cmd_data = fpi_ssm_get_data (transfer->ssm);
  guint16 status;

  if (error)
    {
      if (cmd_data->callback)
        cmd_data->callback (self, NULL, 0, 0, error);
      else
        fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length < 2)
    {
      g_warning ("VCSFW response too short: %" G_GSSIZE_FORMAT " bytes",
                 transfer->actual_length);
      error = fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
      if (cmd_data->callback)
        cmd_data->callback (self, NULL, 0, 0, error);
      else
        fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  validity_unpack (transfer->buffer, transfer->actual_length, "h", &status);

  fp_dbg ("VCSFW response: status=0x%04x, len=%" G_GSSIZE_FORMAT,
          status, transfer->actual_length - 2);

  /* Stash status and data for direct access in RECV states */
  self->cmd_response_status = status;
  g_clear_pointer (&self->cmd_response_data, g_free);
  if (transfer->actual_length > 2)
    {
      self->cmd_response_len = transfer->actual_length - 2;
      self->cmd_response_data = g_memdup2 (transfer->buffer + 2,
                                           self->cmd_response_len);
    }
  else
    {
      self->cmd_response_len = 0;
      self->cmd_response_data = NULL;
    }

  if (cmd_data->callback)
    {
      cmd_data->callback (self,
                          self->cmd_response_data,
                          self->cmd_response_len,
                          status,
                          NULL);
    }

  /* If the callback didn't fail the SSM, advance it */
  fpi_ssm_mark_completed (transfer->ssm);
}

void
vcsfw_cmd_run_state (FpiSsm   *ssm,
                     FpDevice *dev)
{
  VcsfwCmdData *cmd_data = fpi_ssm_get_data (ssm);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VCSFW_CMD_STATE_SEND:
      fp_dbg ("VCSFW send cmd 0x%02x, len=%" G_GSIZE_FORMAT,
              cmd_data->cmd_data[0], cmd_data->cmd_len);

      transfer = fpi_usb_transfer_new (dev);
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT,
                                  cmd_data->cmd_len);
      memcpy (transfer->buffer, cmd_data->cmd_data, cmd_data->cmd_len);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case VCSFW_CMD_STATE_RECV:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                                  VALIDITY_MAX_TRANSFER_LEN);
      fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                               cmd_receive_cb, NULL);
      break;
    }
}

static void
cmd_ssm_done (FpiSsm   *ssm,
              FpDevice *dev,
              GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  self->cmd_ssm = NULL;

  if (error)
    fp_dbg ("VCSFW command SSM failed: %s", error->message);

  /* Error is handled by the callback, nothing else to do here */
  g_clear_error (&error);
}

void
vcsfw_cmd_send (FpiDeviceValidity *self,
                FpiSsm            *parent_ssm,
                const guint8      *cmd,
                gsize              cmd_len,
                VcsfwCmdCallback   callback)
{
  FpiSsm *ssm;
  VcsfwCmdData *cmd_data;

  cmd_data = vcsfw_cmd_data_new (cmd, cmd_len, callback);

  ssm = fpi_ssm_new (FP_DEVICE (self), vcsfw_cmd_run_state,
                     VCSFW_CMD_STATE_NUM_STATES);
  fpi_ssm_set_data (ssm, cmd_data, vcsfw_cmd_data_free);

  self->cmd_ssm = ssm;

  if (parent_ssm)
    fpi_ssm_start_subsm (parent_ssm, ssm);
  else
    fpi_ssm_start (ssm, cmd_ssm_done);
}

/* ================================================================
 * TLS-wrapped command/response exchange
 *
 * Same pattern as vcsfw_cmd_send but wraps the outgoing command in
 * a TLS application data record (with 0x44000000 prefix) and
 * decrypts the incoming response before extracting the 2-byte
 * VCSFW status.
 * ================================================================ */

static void
tls_cmd_receive_cb (FpiUsbTransfer *transfer,
                    FpDevice       *device,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  VcsfwCmdData *cmd_data = fpi_ssm_get_data (transfer->ssm);
  guint16 status;

  if (error)
    {
      if (cmd_data->callback)
        cmd_data->callback (self, NULL, 0, 0, error);
      else
        fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Decrypt TLS app data response */
  {
    GString *hex = g_string_new ("VCSFW TLS raw response: ");
    for (gsize i = 0; i < MIN ((gsize) transfer->actual_length, (gsize) 40); i++)
      g_string_append_printf (hex, "%02x ", transfer->buffer[i]);
    if (transfer->actual_length > 40)
      g_string_append_printf (hex, "... (%" G_GSSIZE_FORMAT " total)",
                              transfer->actual_length);
    fp_dbg ("%s", hex->str);
    g_string_free (hex, TRUE);
  }
  gsize decrypted_len;
  guint8 *decrypted = validity_tls_unwrap_response (
    &self->tls,
    transfer->buffer, transfer->actual_length,
    &decrypted_len, &error);

  if (!decrypted)
    {
      if (cmd_data->callback)
        cmd_data->callback (self, NULL, 0, 0, error);
      else
        fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Decrypted data is VCSFW response: status(2) + payload */
  if (decrypted_len < 2)
    {
      g_free (decrypted);
      error = fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
      if (cmd_data->callback)
        cmd_data->callback (self, NULL, 0, 0, error);
      else
        fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  validity_unpack (decrypted, decrypted_len, "h", &status);

  fp_dbg ("VCSFW TLS response: status=0x%04x, len=%" G_GSIZE_FORMAT,
          status, decrypted_len - 2);

  /* Stash for parent SSM consumption */
  self->cmd_response_status = status;
  g_clear_pointer (&self->cmd_response_data, g_free);
  if (decrypted_len > 2)
    {
      self->cmd_response_len = decrypted_len - 2;
      self->cmd_response_data = g_memdup2 (decrypted + 2,
                                           self->cmd_response_len);
    }
  else
    {
      self->cmd_response_len = 0;
      self->cmd_response_data = NULL;
    }

  g_free (decrypted);

  if (cmd_data->callback)
    {
      cmd_data->callback (self,
                          self->cmd_response_data,
                          self->cmd_response_len,
                          status,
                          NULL);
    }

  fpi_ssm_mark_completed (transfer->ssm);
}

void
vcsfw_tls_cmd_run_state (FpiSsm   *ssm,
                         FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);
  VcsfwCmdData *cmd_data = fpi_ssm_get_data (ssm);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VCSFW_TLS_CMD_STATE_SEND:
      {
        /* Wrap VCSFW command as TLS application data */
        gsize wrapped_len;
        guint8 *wrapped = validity_tls_wrap_app_data (&self->tls,
                                                      cmd_data->cmd_data,
                                                      cmd_data->cmd_len,
                                                      &wrapped_len);

        /* Build USB payload: TLS record directly (no 0x44000000 prefix
         * for post-handshake app data, per python-validity) */
        gsize usb_len = wrapped_len;

        fp_dbg ("VCSFW TLS send cmd 0x%02x, plaintext=%" G_GSIZE_FORMAT
                ", wire=%" G_GSIZE_FORMAT,
                cmd_data->cmd_data[0], cmd_data->cmd_len, usb_len);

        transfer = fpi_usb_transfer_new (dev);
        transfer->short_is_error = TRUE;
        fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_OUT, usb_len);
        memcpy (transfer->buffer, wrapped, wrapped_len);
        g_free (wrapped);

        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                                 fpi_ssm_usb_transfer_cb, NULL);
      }
      break;

    case VCSFW_TLS_CMD_STATE_RECV:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, VALIDITY_EP_CMD_IN,
                                  VALIDITY_MAX_TRANSFER_LEN);
      fpi_usb_transfer_submit (transfer, VALIDITY_USB_TIMEOUT, NULL,
                               tls_cmd_receive_cb, NULL);
      break;
    }
}

void
vcsfw_tls_cmd_send (FpiDeviceValidity *self,
                    FpiSsm            *parent_ssm,
                    const guint8      *cmd,
                    gsize              cmd_len,
                    VcsfwCmdCallback   callback)
{
  FpiSsm *ssm;
  VcsfwCmdData *cmd_data;

  cmd_data = vcsfw_cmd_data_new (cmd, cmd_len, callback);

  ssm = fpi_ssm_new (FP_DEVICE (self), vcsfw_tls_cmd_run_state,
                     VCSFW_TLS_CMD_STATE_NUM_STATES);
  fpi_ssm_set_data (ssm, cmd_data, vcsfw_cmd_data_free);

  self->cmd_ssm = ssm;

  if (parent_ssm)
    fpi_ssm_start_subsm (parent_ssm, ssm);
  else
    fpi_ssm_start (ssm, cmd_ssm_done);
}

/* ---- GET_VERSION (cmd 0x01) response parser ---- */

gboolean
vcsfw_parse_version (const guint8        *data,
                     gsize                data_len,
                     ValidityVersionInfo *info)
{
  const guint8 *serial;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  if (!validity_unpack (data, data_len, "wwbbbbbbbbdhbb",
                        &info->build_time, &info->build_num,
                        &info->version_major, &info->version_minor,
                        &info->target, &info->product,
                        &info->silicon_rev, &info->formal_release,
                        &info->platform, &info->patch,
                        &serial, (gsize) sizeof (info->serial_number),
                        &info->security,
                        &info->iface, &info->device_type))
    return FALSE;

  memcpy (info->serial_number, serial, sizeof (info->serial_number));
  return TRUE;
}
