/*
 * ChromeOS Fingerprint driver for libfprint
 *
 * Copyright (C) 2026 Marco Trevisan (Treviño) <mail@3v1n0.net>
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

#define FP_COMPONENT "crfpmoc"

#include <errno.h>
#include <sys/ioctl.h>

#include "crfpmoc.h"
#include "crfpmoc-ec-transfer.h"
#include "fpi-log.h"

static void
crfpmoc_ec_transfer_log_cmd (CrfpMocEcTransfer *transfer)
{
  if (!G_LIKELY (fpi_log_is_debug_transfer_enabled ()))
    return;

  g_debug ("EC cmd transfer %p submitted, command 0x%04x, "
           "outsize %d, insize %d",
           transfer, transfer->command, transfer->outsize, transfer->insize);

  if (transfer->outdata)
    fp_dbg_hex_dump_data (transfer->outdata, transfer->outsize);
}

static void
crfpmoc_ec_transfer_log_eventmask (CrfpMocEcTransfer *transfer)
{
  if (!G_LIKELY (fpi_log_is_debug_transfer_enabled ()))
    return;

  g_debug ("EC eventmask transfer %p submitted, mask 0x%lx",
           transfer, transfer->event_mask);
}

static struct crfpmoc_cros_ec_command_v2 *
build_raw_cmd (CrfpMocEcTransfer *transfer)
{
  g_autofree struct crfpmoc_cros_ec_command_v2 *s_cmd = NULL;
  gsize total;

  total = sizeof (*s_cmd) + MAX (transfer->outsize, transfer->insize);
  s_cmd = g_malloc0 (total);
  s_cmd->command = transfer->command;
  s_cmd->version = transfer->version;
  s_cmd->result = 0xff;
  s_cmd->outsize = transfer->outsize;
  s_cmd->insize = transfer->insize;

  if (transfer->outsize > 0 && transfer->outdata)
    memcpy (s_cmd->data, transfer->outdata, transfer->outsize);

  return g_steal_pointer (&s_cmd);
}

static FpDeviceError
ec_result_to_fp_error (int ec_result)
{
  if (ec_result == EC_RES_BUSY)
    return FP_DEVICE_ERROR_BUSY;
  if (ec_result == EC_RES_ACCESS_DENIED)
    return FP_DEVICE_ERROR_NOT_SUPPORTED;
  return FP_DEVICE_ERROR_PROTO;
}

static int
do_ioctl_xcmd (CrfpMocEcTransfer                 *transfer,
               struct crfpmoc_cros_ec_command_v2 *s_cmd,
               GError                           **error)
{
  int fd;
  int r;

  fd = fpi_device_get_udev_fd (transfer->device, FPI_DEVICE_UDEV_SUBTYPE_MISC);
  r = ioctl (fd, CRFPMOC_CROS_EC_DEV_IOCXCMD_V2, s_cmd);

  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errno),
                   "ioctl failed: %s",
                   g_strerror (errno));
      return -1;
    }

  if (s_cmd->result != EC_RES_SUCCESS)
    {
      g_set_error (error,
                   FP_DEVICE_ERROR,
                   ec_result_to_fp_error (s_cmd->result),
                   "command failed: %s",
                   crfpmoc_strresult (s_cmd->result));
      return -1;
    }

  if (transfer->insize > 0 && transfer->indata)
    memcpy (transfer->indata, s_cmd->data, MIN (r, transfer->insize));

  return r;
}

static void
command_thread_func (GTask        *task,
                     gpointer      source_object,
                     gpointer      task_data,
                     GCancellable *cancellable)
{
  CrfpMocEcTransfer *transfer = (CrfpMocEcTransfer *) task_data;

  g_autofree struct crfpmoc_cros_ec_command_v2 *s_cmd = NULL;
  g_autoptr(GError) error = NULL;
  int r;

  if (g_task_return_error_if_cancelled (task))
    return;

  s_cmd = build_raw_cmd (transfer);

  r = do_ioctl_xcmd (transfer, s_cmd, &error);

  if (crfpmoc_umockdev_recording_enabled (FPI_DEVICE_CRFPMOC (transfer->device)))
    {
      transfer->raw_cmd = g_steal_pointer (&s_cmd);
      transfer->raw_cmd_size = sizeof (struct crfpmoc_cros_ec_command_v2)
                               + MAX (transfer->outsize, transfer->insize);
    }

  if (g_task_return_error_if_cancelled (task))
    return;

  if (r < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
eventmask_thread_func (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  CrfpMocEcTransfer *transfer = (CrfpMocEcTransfer *) task_data;
  FpDevice *device;

  g_autoptr(GError) error = NULL;
  int fd;
  int r;

  if (g_task_return_error_if_cancelled (task))
    return;

  device = transfer->device;
  fd = fpi_device_get_udev_fd (device, FPI_DEVICE_UDEV_SUBTYPE_MISC);

  r = ioctl (fd, CRFPMOC_CROS_EC_DEV_IOCEVENTMASK_V2, transfer->event_mask);

  if (r < 0)
    {
      g_set_error (&error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errno),
                   "ioctl failed: %s",
                   g_strerror (errno));

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

CrfpMocEcTransfer *
crfpmoc_ec_transfer_new_cmd (FpDevice     *device,
                             int           command,
                             int           version,
                             gconstpointer outdata,
                             int           outsize,
                             int           insize)
{
  CrfpMocEcTransfer *transfer;

  g_assert (device != NULL);
  g_assert (outsize == 0 || outdata != NULL);

  transfer = g_malloc0 (sizeof (*transfer));
  transfer->device = device;
  transfer->command = command;
  transfer->version = version;
  transfer->outsize = outsize;
  transfer->insize = insize;

  if (outsize > 0)
    transfer->outdata = g_memdup2 (outdata, outsize);

  if (insize > 0)
    transfer->indata = g_malloc0 (insize);

  return transfer;
}

CrfpMocEcTransfer *
crfpmoc_ec_transfer_new_eventmask (FpDevice     *device,
                                   unsigned long event_mask)
{
  CrfpMocEcTransfer *transfer;

  g_assert (device != NULL);

  transfer = g_malloc0 (sizeof (*transfer));
  transfer->device = device;
  transfer->event_mask = event_mask;

  return transfer;
}

void
crfpmoc_ec_transfer_free (CrfpMocEcTransfer *transfer)
{
  if (!transfer)
    return;

  g_clear_pointer (&transfer->outdata, g_free);
  g_clear_pointer (&transfer->indata, g_free);
  g_clear_pointer (&transfer->raw_cmd, g_free);
  g_free (transfer);
}

typedef struct
{
  CrfpMocEcTransferCallback callback;
  gpointer                  user_data;
} CallbackData;

static void
submit_cb_wrapper (GObject *src, GAsyncResult *result, gpointer user_data)
{
  g_autofree CallbackData *cd = g_steal_pointer (&user_data);
  CrfpMocEcTransfer *transfer;

  transfer = g_task_get_task_data (G_TASK (result));
  cd->callback (transfer, result, cd->user_data);
}

static void
submit_to_task (CrfpMocEcTransfer        *transfer,
                GCancellable             *cancellable,
                CrfpMocEcTransferCallback callback,
                gpointer                  user_data,
                GTaskThreadFunc           thread_func)
{
  g_autoptr(GTask) task = NULL;
  CallbackData *cd;

  cd = g_new (CallbackData, 1);
  cd->callback = callback;
  cd->user_data = user_data;

  task = g_task_new (G_OBJECT (transfer->device), cancellable, submit_cb_wrapper, cd);
  g_task_set_task_data (task, transfer, (GDestroyNotify) crfpmoc_ec_transfer_free);
  g_task_run_in_thread (task, thread_func);
}

void
crfpmoc_ec_transfer_submit_cmd (CrfpMocEcTransfer        *transfer,
                                GCancellable             *cancellable,
                                CrfpMocEcTransferCallback callback,
                                gpointer                  user_data)
{
  g_assert (transfer != NULL);
  g_assert (callback != NULL);

  crfpmoc_ec_transfer_log_cmd (transfer);
  submit_to_task (transfer, cancellable, callback, user_data, command_thread_func);
}

void
crfpmoc_ec_transfer_submit_eventmask (CrfpMocEcTransfer        *transfer,
                                      GCancellable             *cancellable,
                                      CrfpMocEcTransferCallback callback,
                                      gpointer                  user_data)
{
  g_assert (transfer != NULL);
  g_assert (callback != NULL);

  crfpmoc_ec_transfer_log_eventmask (transfer);
  submit_to_task (transfer, cancellable, callback, user_data, eventmask_thread_func);
}

gboolean
crfpmoc_ec_transfer_submit_finish (GAsyncResult *result,
                                   GError      **error)
{
  GTask *task = G_TASK (result);
  CrfpMocEcTransfer *transfer;

  g_autoptr(GError) local_error = NULL;
  gboolean ret;

  transfer = g_task_get_task_data (task);

  g_assert (g_task_is_valid (result, G_OBJECT (transfer->device)));

  ret = g_task_propagate_boolean (task, &local_error);

  if (local_error)
    g_propagate_error (error, g_steal_pointer (&local_error));

  if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return ret;

  if (transfer->raw_cmd &&
      crfpmoc_umockdev_recording_enabled (FPI_DEVICE_CRFPMOC (transfer->device)))
    {
      crfpmoc_umockdev_record (FPI_DEVICE_CRFPMOC (transfer->device), 0,
                               CRFPMOC_CROS_EC_DEV_IOCXCMD_V2,
                               transfer->raw_cmd);
    }

  return ret;
}

gboolean
crfpmoc_ec_transfer_submit_sync (CrfpMocEcTransfer *transfer,
                                 GError           **error)
{
  g_autofree struct crfpmoc_cros_ec_command_v2 *s_cmd = NULL;
  int r;

  g_assert (transfer != NULL);

  crfpmoc_ec_transfer_log_cmd (transfer);

  s_cmd = build_raw_cmd (transfer);
  r = do_ioctl_xcmd (transfer, s_cmd, error);

  if (crfpmoc_umockdev_recording_enabled (FPI_DEVICE_CRFPMOC (transfer->device)))
    crfpmoc_umockdev_record (FPI_DEVICE_CRFPMOC (transfer->device), r,
                             CRFPMOC_CROS_EC_DEV_IOCXCMD_V2, s_cmd);

  return r >= 0;
}

void
crfpmoc_ssm_ec_transfer_cb (CrfpMocEcTransfer *transfer,
                            GAsyncResult      *result,
                            gpointer           user_data)
{
  FpiSsm *ssm = user_data;

  g_autoptr(GError) error = NULL;

  if (!crfpmoc_ec_transfer_submit_finish (result, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fpi_ssm_next_state (ssm);
}
