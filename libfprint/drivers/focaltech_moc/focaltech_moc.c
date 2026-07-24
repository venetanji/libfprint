/*
 * Copyright (C) 2022-2026 FocalTech Systems Co., Ltd.
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

#include "focaltech_moc.h"

#include <ctype.h>

#define FP_COMPONENT "focaltech_moc"

#include "drivers_api.h"

G_DEFINE_TYPE (FpiDeviceFocaltechMoc, fpi_device_focaltech_moc, FP_TYPE_SDCP_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x2808,  .pid = 0x9e48,  },
  { .vid = 0x2808,  .pid = 0xd979,  },
  { .vid = 0x2808,  .pid = 0xa27a,  },
  { .vid = 0x2808,  .pid = 0xa959,  },
  { .vid = 0x2808,  .pid = 0xa99a,  },
  { .vid = 0x2808,  .pid = 0xa57a,  },
  { .vid = 0x2808,  .pid = 0xa78a,  },
  { .vid = 0x2808,  .pid = 0x1579,  },
  { .vid = 0x2808,  .pid = 0x077A,  },
  { .vid = 0x2808,  .pid = 0x079A,  },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },   /* terminating entry */
};

/* Device commands */
#define FOCALTECH_MOC_CMD_FP_INFO 0x80
#define FOCALTECH_MOC_CMD_OPERATE_FP 0x82
#define FOCALTECH_MOC_CMD_SDCP_CONNECT 0xa0
#define FOCALTECH_MOC_CMD_SDCP_PRE_ENROLL 0xa2
#define FOCALTECH_MOC_CMD_SDCP_ENROLL_COMMIT 0xa3
#define FOCALTECH_MOC_CMD_SDCP_IDENTIFY 0xa4
#define FOCALTECH_MOC_CMD_SDCP_MAX_ENROLL_SAMPLES 0xa5
#define FOCALTECH_MOC_CMD_SDCP_CAPTURE 0xa6
#define FOCALTECH_MOC_CMD_SDCP_CANCEL_ENROLL 0xa7
#define FOCALTECH_MOC_CMD_SDCP_DELETE_TEMPLATE 0xa8
#define FOCALTECH_MOC_CMD_ENUMERATE_TEMPLATE 0xab
#define FOCALTECH_MOC_CMD_DELETE_ALL_TEMPLATE 0xac

/* Response status codes */
#define FOCALTECH_MOC_RSP_OK 0x04
#define FOCALTECH_MOC_RSP_PARA_ERR 0x05
#define FOCALTECH_MOC_RSP_INVALID 0x09

/*
 * ConnectResponse layout per the SDCP specification:
 * m || r_d || cert_m || pk_d || pk_f || h_f || s_m || s_d.
 * cert_m has no length field; it spans whatever remains after the
 * fixed-size fields. h_f is 32 bytes (SDCP_MAC_SIZE).
 */
#define FOCALTECH_MOC_CONNECT_RESP_FIXED_SIZE \
  (SDCP_MAC_SIZE            \
   + SDCP_RANDOM_SIZE      \
   + 2 * SDCP_PUBLIC_KEY_SIZE \
   + SDCP_MAC_SIZE          \
   + 2 * SDCP_SIGNATURE_SIZE)
#define FOCALTECH_MOC_CONNECT_RESP_MAX_SIZE 1600

typedef void (*SynCmdMsgCallback) (FpiDeviceFocaltechMoc *self,
                                   uint8_t               *buffer_in,
                                   gsize                  length_in,
                                   GError                *error);

typedef struct
{
  SynCmdMsgCallback callback;
  uint8_t          *cmd_buf;
  gsize             length_out;
  gsize             length_in;
  gboolean          cancelable;
  int               retries_left;
  gboolean          is_recovery_read;
  gboolean          recovery_bounded;
  GError           *original_error;
} CommandData;

typedef struct
{
  uint8_t h;
  uint8_t l;
} FpCmdLen;

typedef struct
{
  uint8_t  magic;
  FpCmdLen len;
} FpCmdHeader;

typedef struct
{
  FpCmdHeader header;
  uint8_t     code;
  uint8_t     payload[0];
} FpCmd;

typedef struct
{
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
  uint8_t b0;
  uint8_t b1;
#else
  uint8_t b1;
  uint8_t b0;
#endif
} u16_bytes_t;

typedef union
{
  u16_bytes_t s;
  uint16_t    v;
} u_u16_bytes_t;

static inline uint16_t
get_u16_from_u8_lh (uint8_t l, uint8_t h)
{
  u_u16_bytes_t u_u16_bytes;

  u_u16_bytes.v = 0;
  u_u16_bytes.s.b0 = l;
  u_u16_bytes.s.b1 = h;

  return u_u16_bytes.v;
}

static inline uint8_t
get_u8_l_from_u16 (uint16_t v)
{
  u_u16_bytes_t u_u16_bytes;

  u_u16_bytes.v = v;

  return u_u16_bytes.s.b0;
}

static inline uint8_t
get_u8_h_from_u16 (uint16_t v)
{
  u_u16_bytes_t u_u16_bytes;

  u_u16_bytes.v = v;

  return u_u16_bytes.s.b1;
}

static uint8_t
fp_cmd_bcc (uint8_t *data, uint16_t len)
{
  int i;
  uint8_t bcc = 0;

  for (i = 0; i < len; i++)
    bcc ^= data[i];

  return bcc;
}

static uint8_t *
focaltech_moc_compose_cmd (uint8_t cmd, const uint8_t *data, uint16_t len)
{
  g_autofree uint8_t *cmd_buf = NULL;
  FpCmd *fp_cmd = NULL;
  uint8_t *bcc = NULL;
  uint16_t header_len = len + sizeof (*bcc);

  cmd_buf = g_new0 (uint8_t, sizeof (FpCmd) + header_len);

  fp_cmd = (FpCmd *) cmd_buf;

  fp_cmd->header.magic = 0x02;
  fp_cmd->header.len.l = get_u8_l_from_u16 (header_len);
  fp_cmd->header.len.h = get_u8_h_from_u16 (header_len);
  fp_cmd->code = cmd;

  if (data != NULL)
    memcpy (fp_cmd->payload, data, len);

  bcc = fp_cmd->payload + len;
  *bcc = fp_cmd_bcc ((uint8_t *) &fp_cmd->header.len, bcc - (uint8_t *) &fp_cmd->header.len);

  return g_steal_pointer (&cmd_buf);
}

static int
focaltech_moc_check_cmd (uint8_t *response_buf, uint16_t len)
{
  int ret = -1;
  FpCmd *fp_cmd = NULL;
  uint8_t *bcc = NULL;
  uint16_t header_len;
  uint16_t data_len;

  fp_cmd = (FpCmd *) response_buf;

  if (len < sizeof (FpCmd) + sizeof (*bcc))
    return ret;

  if (fp_cmd->header.magic != 0x02)
    return ret;

  header_len = get_u16_from_u8_lh (fp_cmd->header.len.l, fp_cmd->header.len.h);

  if (header_len < sizeof (*bcc))
    return ret;

  if ((sizeof (FpCmd) + header_len) > len)
    return ret;

  data_len = header_len - sizeof (*bcc);

  bcc = fp_cmd->payload + data_len;

  if (fp_cmd_bcc ((uint8_t *) &fp_cmd->header.len,
                  bcc - (uint8_t *) &fp_cmd->header.len) != *bcc)
    return ret;

  ret = 0;
  return ret;
}

static gsize
fp_cmd_payload_len (uint8_t *buffer_in)
{
  FpCmd *fp_cmd = (FpCmd *) buffer_in;
  uint16_t header_len;

  header_len = get_u16_from_u8_lh (fp_cmd->header.len.l, fp_cmd->header.len.h);

  return header_len - sizeof (uint8_t);
}

static gboolean
focaltech_moc_require_payload_len (FpiDeviceFocaltechMoc *self,
                                   uint8_t               *buffer_in,
                                   gsize                  minimum_length,
                                   const char            *context)
{
  gsize payload_len = fp_cmd_payload_len (buffer_in);

  if (payload_len >= minimum_length)
    return TRUE;

  fpi_ssm_mark_failed (self->task_ssm,
                       fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                 "%s: short response (%zu < %zu)",
                                                 context, payload_len, minimum_length));
  return FALSE;
}

static void
fp_cmd_receive_cb (FpiUsbTransfer *transfer,
                   FpDevice       *device,
                   gpointer        userdata,
                   GError         *error)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);
  CommandData *data = userdata;
  int ssm_state = 0;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&error));
      return;
    }

  if (data == NULL)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  ssm_state = fpi_ssm_get_cur_state (transfer->ssm);

  /* skip zero-length packets */
  if (transfer->actual_length == 0)
    {
      fpi_ssm_jump_to_state (transfer->ssm, ssm_state);
      return;
    }

  if (focaltech_moc_check_cmd (transfer->buffer, transfer->actual_length) != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  if (data->callback)
    data->callback (self, transfer->buffer, transfer->actual_length, NULL);

  fpi_ssm_mark_completed (transfer->ssm);
}

typedef enum {
  FOCALTECH_CMD_SEND = 0,
  FOCALTECH_CMD_GET,
  FOCALTECH_CMD_NUM_STATES,
} FocaltechCmdState;

static void
fp_cmd_run_state (FpiSsm   *ssm,
                  FpDevice *device)
{
  FpiUsbTransfer *transfer;
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FOCALTECH_CMD_SEND:
      if (self->cmd_transfer)
        {
          self->cmd_transfer->ssm = ssm;
          fpi_usb_transfer_submit (g_steal_pointer (&self->cmd_transfer),
                                   FOCALTECH_MOC_CMD_TIMEOUT,
                                   NULL,
                                   fpi_ssm_usb_transfer_cb,
                                   NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }

      break;

    case FOCALTECH_CMD_GET:
      {
        CommandData *data = fpi_ssm_get_data (ssm);
        guint timeout = FOCALTECH_MOC_CMD_TIMEOUT;

        if (self->cmd_len_in == 0)
          {
            if (data->callback)
              data->callback (self, NULL, 0, 0);

            fpi_ssm_mark_completed (ssm);
            return;
          }

        /* when the send already failed no reply is guaranteed, so a
         * recovery read must not wait forever */
        if (self->cmd_cancelable && !data->recovery_bounded)
          timeout = 0;

        transfer = fpi_usb_transfer_new (device);
        transfer->ssm = ssm;
        fpi_usb_transfer_fill_bulk (transfer, self->bulk_in_ep, self->cmd_len_in);
        fpi_usb_transfer_submit (transfer,
                                 timeout,
                                 self->cmd_cancelable ? fpi_device_get_cancellable (device) : NULL,
                                 fp_cmd_receive_cb,
                                 data);
        break;
      }

    }

}

static void focaltech_moc_get_cmd_full (FpDevice         *device,
                                        guint8           *buffer_out,
                                        gsize             length_out,
                                        gsize             length_in,
                                        gboolean          can_be_cancelled,
                                        SynCmdMsgCallback callback,
                                        int               retries);

static gboolean
focaltech_moc_cmd_is_idempotent (uint8_t code)
{
  switch (code)
    {
    case FOCALTECH_MOC_CMD_FP_INFO:
    case FOCALTECH_MOC_CMD_OPERATE_FP:
    case FOCALTECH_MOC_CMD_SDCP_MAX_ENROLL_SAMPLES:
    case FOCALTECH_MOC_CMD_ENUMERATE_TEMPLATE:
      return TRUE;

    default:
      return FALSE;
    }
}

static void
fp_cmd_ssm_done_data_free (CommandData *data)
{
  g_clear_error (&data->original_error);
  g_free (data->cmd_buf);
  g_free (data);
}

static void fp_cmd_ssm_done (FpiSsm   *ssm,
                             FpDevice *device,
                             GError   *error);

static void
focaltech_moc_start_cmd_ssm (FpiDeviceFocaltechMoc *self,
                             CommandData           *data)
{
  self->cmd_len_in = data->length_in + 1;
  self->cmd_cancelable = data->cancelable;

  self->cmd_ssm = fpi_ssm_new (FP_DEVICE (self),
                               fp_cmd_run_state,
                               FOCALTECH_CMD_NUM_STATES);

  fpi_ssm_set_data (self->cmd_ssm, data, (GDestroyNotify) fp_cmd_ssm_done_data_free);

  fpi_ssm_start (self->cmd_ssm, fp_cmd_ssm_done);
}

static void
fp_cmd_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  g_autoptr(GError) local_error = error;
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);
  CommandData *data = fpi_ssm_get_data (ssm);
  gboolean recoverable;

  self->cmd_ssm = NULL;

  if (local_error == NULL)
    return;

  recoverable = data->retries_left > 0 &&
                !g_error_matches (local_error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE) &&
                !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  /*
   * A failed transfer does not prove the device never executed the
   * command. First try to pick up a reply that may still be pending so
   * a replay cannot leave a stale reply in the stream; only when
   * nothing is pending replay commands without side effects.
   */
  if (recoverable && !data->is_recovery_read)
    {
      CommandData *recovery = g_new0 (CommandData, 1);

      fp_warn ("focaltechmoc cmd failed (%s), checking for a pending reply",
               local_error->message);

      recovery->callback = data->callback;
      recovery->cmd_buf = g_memdup2 (data->cmd_buf, data->length_out);
      recovery->length_out = data->length_out;
      recovery->length_in = data->length_in;
      recovery->cancelable = data->cancelable;
      recovery->retries_left = data->retries_left;
      recovery->is_recovery_read = TRUE;
      recovery->recovery_bounded =
        fpi_ssm_get_cur_state (ssm) == FOCALTECH_CMD_SEND;
      recovery->original_error = g_steal_pointer (&local_error);

      focaltech_moc_start_cmd_ssm (self, recovery);
      return;
    }

  if (recoverable && data->is_recovery_read &&
      focaltech_moc_cmd_is_idempotent (data->cmd_buf[offsetof (FpCmd, code)]))
    {
      fp_warn ("focaltechmoc no pending reply, retrying command");
      focaltech_moc_get_cmd_full (device,
                                  g_memdup2 (data->cmd_buf, data->length_out),
                                  data->length_out, data->length_in,
                                  data->cancelable, data->callback,
                                  data->retries_left - 1);
      return;
    }

  if (data->callback)
    {
      data->callback (self, NULL, 0,
                      data->original_error != NULL ?
                      g_steal_pointer (&data->original_error) :
                      g_steal_pointer (&local_error));
    }
}

static void
focaltech_moc_get_cmd_full (FpDevice *device, guint8 *buffer_out,
                            gsize length_out, gsize length_in,
                            gboolean can_be_cancelled,
                            SynCmdMsgCallback callback,
                            int retries)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  g_autoptr(FpiUsbTransfer) transfer = NULL;
  CommandData *data = g_new0 (CommandData, 1);

  data->callback = callback;
  data->cmd_buf = g_memdup2 (buffer_out, length_out);
  data->length_out = length_out;
  data->length_in = length_in;
  data->cancelable = can_be_cancelled;
  data->retries_left = retries;

  transfer = fpi_usb_transfer_new (device);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, self->bulk_out_ep, buffer_out,
                                   length_out, g_free);

  self->cmd_transfer = g_steal_pointer (&transfer);

  focaltech_moc_start_cmd_ssm (self, data);
}

static void
focaltech_moc_get_cmd (FpDevice *device, guint8 *buffer_out,
                       gsize length_out, gsize length_in,
                       gboolean can_be_cancelled,
                       SynCmdMsgCallback callback)
{
  focaltech_moc_get_cmd_full (device, buffer_out, length_out, length_in,
                              can_be_cancelled, callback, 1);
}

static void
task_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  self->num_frames = 0;
  self->task_ssm = NULL;

  if (error)
    fpi_device_action_error (device, g_steal_pointer (&error));
}

static const char *
get_g_usb_device_direction_des (GUsbDeviceDirection dir)
{
  switch (dir)
    {
    case G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST:
      return "G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST";

    case G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE:
      return "G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE";

    default:
      return "unknown";
    }
}

static gboolean
usb_claim_interface_probe (FpDevice *device, gboolean claim, GError **error)
{
  g_autoptr(GPtrArray) interfaces = NULL;
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);
  int i;

  interfaces = g_usb_device_get_interfaces (fpi_device_get_usb_device (device), error);
  if (interfaces == NULL)
    return FALSE;

  for (i = 0; i < interfaces->len; i++)
    {
      GUsbInterface *cur_iface = g_ptr_array_index (interfaces, i);
      g_autoptr(GPtrArray) endpoints = g_usb_interface_get_endpoints (cur_iface);

      fp_dbg ("class:%x, subclass:%x, protocol:%x",
              g_usb_interface_get_class (cur_iface),
              g_usb_interface_get_subclass (cur_iface),
              g_usb_interface_get_protocol (cur_iface));

      if (claim)
        {
          for (int j = 0; j < endpoints->len; j++)
            {
              GUsbEndpoint *endpoint = g_ptr_array_index (endpoints, j);
              GBytes *bytes = g_usb_endpoint_get_extra (endpoint);

              fp_dbg ("bytes size:%ld", g_bytes_get_size (bytes));

              fp_dbg ("kind:%x, max packet size:%d, poll interval:%d, refresh:%x, "
                      "sync address:%x, address:%x, number:%d, direction:%s",
                      g_usb_endpoint_get_kind (endpoint),
                      g_usb_endpoint_get_maximum_packet_size (endpoint),
                      g_usb_endpoint_get_polling_interval (endpoint),
                      g_usb_endpoint_get_refresh (endpoint),
                      g_usb_endpoint_get_synch_address (endpoint),
                      g_usb_endpoint_get_address (endpoint),
                      g_usb_endpoint_get_number (endpoint),
                      get_g_usb_device_direction_des (g_usb_endpoint_get_direction (endpoint)));

              if (g_usb_endpoint_get_direction (endpoint) == G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST)
                self->bulk_in_ep = g_usb_endpoint_get_address (endpoint);
              else
                self->bulk_out_ep = g_usb_endpoint_get_address (endpoint);
            }

          if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                             g_usb_interface_get_number (cur_iface),
                                             0, error))
            return FALSE;
        }
      else if (!g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                                g_usb_interface_get_number (cur_iface),
                                                0, error))
        {
          return FALSE;
        }
    }

  return TRUE;
}

static void
task_ssm_init_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  if (error)
    usb_claim_interface_probe (device, FALSE, NULL);

  fpi_sdcp_device_open_complete (FP_SDCP_DEVICE (device), g_steal_pointer (&error));
}

struct EnrollTimes
{
  uint8_t enroll_times;
};

static void
focaltech_moc_get_enroll_times (FpiDeviceFocaltechMoc *self,
                                uint8_t               *buffer_in,
                                gsize                  length_in,
                                GError                *error)
{
  FpCmd *fp_cmd = NULL;
  struct EnrollTimes *enroll_times = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  enroll_times = (struct EnrollTimes *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Can't get response!!"));
    }
  else
    {
      if (!focaltech_moc_require_payload_len (self, buffer_in,
                                              sizeof (struct EnrollTimes),
                                              "enroll times"))
        return;

      fp_dbg ("focaltechmoc enroll_times: %d", enroll_times->enroll_times + 1);
      fpi_device_set_nr_enroll_stages (FP_DEVICE (self), enroll_times->enroll_times + 1);
      fpi_ssm_next_state (self->task_ssm);
    }
}

static void
focaltech_moc_release_finger (FpiDeviceFocaltechMoc *self,
                              uint8_t               *buffer_in,
                              gsize                  length_in,
                              GError                *error)
{
  FpCmd *fp_cmd = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Can't get response!!"));
    }
  else
    {
      fpi_ssm_next_state (self->task_ssm);
    }
}

enum dev_init_states {
  DEV_INIT_GET_ENROLL_TIMES,
  DEV_INIT_RELEASE_FINGER,
  DEV_INIT_STATES,
};

static void
dev_init_handler (FpiSsm *ssm, FpDevice *device)
{
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEV_INIT_GET_ENROLL_TIMES:
      cmd_len = 0;
      resp_len = sizeof (struct EnrollTimes);
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_MAX_ENROLL_SAMPLES, NULL, cmd_len);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                             sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                             1,
                             focaltech_moc_get_enroll_times);
      break;

    case DEV_INIT_RELEASE_FINGER:
      {
        uint8_t d1 = 0x78;
        cmd_len = sizeof (d1);
        resp_len = 0;
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_OPERATE_FP, &d1, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_release_finger);
        break;
      }
    }
}

static void
focaltech_moc_open (FpSdcpDevice *sdcp_device)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = FP_DEVICE (sdcp_device);
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    {
      fpi_sdcp_device_open_complete (sdcp_device, g_steal_pointer (&error));
      return;
    }

  if (!usb_claim_interface_probe (device, TRUE, &error))
    {
      fpi_sdcp_device_open_complete (sdcp_device, g_steal_pointer (&error));
      return;
    }

  self->task_ssm = fpi_ssm_new (device, dev_init_handler, DEV_INIT_STATES);
  fpi_ssm_start (self->task_ssm, task_ssm_init_done);
}

static void
task_ssm_exit_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  if (!error)
    usb_claim_interface_probe (device, FALSE, &error);

  fpi_device_close_complete (FP_DEVICE (self), error);
  self->task_ssm = NULL;
}

enum dev_exit_states {
  DEV_EXIT_START,
  DEV_EXIT_STATES,
};

static void
dev_exit_handler (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEV_EXIT_START:
      fpi_ssm_next_state (self->task_ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
focaltech_moc_close (FpDevice *device)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  fp_info ("Focaltechmoc dev_exit");
  self->task_ssm = fpi_ssm_new (FP_DEVICE (self), dev_exit_handler, DEV_EXIT_STATES);
  fpi_ssm_start (self->task_ssm, task_ssm_exit_done);
}

static void
focaltech_moc_sdcp_connect_cb (FpiDeviceFocaltechMoc *self,
                               uint8_t               *buffer_in,
                               gsize                  length_in,
                               GError                *error)
{
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  uint8_t *payload = NULL;
  gsize payload_len;
  gsize cert_len;
  gsize pos = 0;

  g_autoptr(GBytes) device_random = NULL;
  g_autoptr(GBytes) mac = NULL;
  g_autoptr(FpiSdcpClaim) claim = NULL;

  if (error)
    {
      fpi_sdcp_device_connect_complete (sdcp_device, NULL, NULL, NULL, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  payload = (uint8_t *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_sdcp_device_connect_complete (sdcp_device, NULL, NULL, NULL,
                                        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                  "SDCP connect failed (status 0x%02x)",
                                                                  fp_cmd->code));
      return;
    }

  payload_len = fp_cmd_payload_len (buffer_in);

  if (payload_len <= FOCALTECH_MOC_CONNECT_RESP_FIXED_SIZE)
    {
      fpi_sdcp_device_connect_complete (sdcp_device, NULL, NULL, NULL,
                                        fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                                  "SDCP ConnectResponse too short (%zu)",
                                                                  payload_len));
      return;
    }

  cert_len = payload_len - FOCALTECH_MOC_CONNECT_RESP_FIXED_SIZE;

  claim = fpi_sdcp_claim_new ();

  mac = g_bytes_new (payload + pos, SDCP_MAC_SIZE);
  pos += SDCP_MAC_SIZE;

  device_random = g_bytes_new (payload + pos, SDCP_RANDOM_SIZE);
  pos += SDCP_RANDOM_SIZE;

  claim->model_certificate = g_bytes_new (payload + pos, cert_len);
  pos += cert_len;

  claim->device_public_key = g_bytes_new (payload + pos, SDCP_PUBLIC_KEY_SIZE);
  pos += SDCP_PUBLIC_KEY_SIZE;

  claim->firmware_public_key = g_bytes_new (payload + pos, SDCP_PUBLIC_KEY_SIZE);
  pos += SDCP_PUBLIC_KEY_SIZE;

  claim->firmware_hash = g_bytes_new (payload + pos, SDCP_MAC_SIZE);
  pos += SDCP_MAC_SIZE;

  claim->model_signature = g_bytes_new (payload + pos, SDCP_SIGNATURE_SIZE);
  pos += SDCP_SIGNATURE_SIZE;

  claim->device_signature = g_bytes_new (payload + pos, SDCP_SIGNATURE_SIZE);
  pos += SDCP_SIGNATURE_SIZE;

  fpi_sdcp_device_connect_complete (sdcp_device, device_random, claim, mac, NULL);
}

static void
focaltech_moc_connect (FpSdcpDevice *sdcp_device)
{
  FpDevice *device = FP_DEVICE (sdcp_device);
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len;

  g_autofree uint8_t *payload = NULL;

  g_autoptr(GBytes) host_random = NULL;
  g_autoptr(GBytes) host_public_key = NULL;
  const guint8 *host_random_data;
  const guint8 *host_public_key_data;
  gsize host_random_len = 0;
  gsize host_public_key_len = 0;

  fpi_sdcp_device_get_connect_data (sdcp_device, &host_random, &host_public_key);
  if (!host_random || !host_public_key)
    {
      fpi_sdcp_device_connect_complete (sdcp_device, NULL, NULL, NULL,
                                        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                  "Failed to get SDCP connect data"));
      return;
    }

  host_random_data = g_bytes_get_data (host_random, &host_random_len);
  host_public_key_data = g_bytes_get_data (host_public_key, &host_public_key_len);

  cmd_len = host_random_len + host_public_key_len;
  payload = g_malloc0 (cmd_len);
  memcpy (payload, host_random_data, host_random_len);
  memcpy (payload + host_random_len, host_public_key_data, host_public_key_len);

  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CONNECT, payload, cmd_len);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                         sizeof (FpCmd) + FOCALTECH_MOC_CONNECT_RESP_MAX_SIZE + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_sdcp_connect_cb);
}

struct UidList
{
  uint8_t actived[FOCALTECH_MOC_MAX_FINGERS];
  uint8_t uid[FOCALTECH_MOC_MAX_FINGERS][SDCP_ENROLLMENT_ID_SIZE];
};

static void
focaltech_moc_list_cb (FpiDeviceFocaltechMoc *self,
                       uint8_t               *buffer_in,
                       gsize                  length_in,
                       GError                *error)
{
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  struct UidList *uid_list = NULL;

  g_autoptr(GPtrArray) ids = NULL;

  if (error)
    {
      fpi_sdcp_device_list_complete (sdcp_device, NULL, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;

  if (fp_cmd->code == FOCALTECH_MOC_RSP_INVALID)
    {
      /* no templates stored */
      ids = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
      fpi_sdcp_device_list_complete (sdcp_device, g_steal_pointer (&ids), NULL);
      return;
    }

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_sdcp_device_list_complete (sdcp_device, NULL,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                               "Enumerate failed (status 0x%02x)",
                                                               fp_cmd->code));
      return;
    }

  if (fp_cmd_payload_len (buffer_in) < sizeof (struct UidList))
    {
      fpi_sdcp_device_list_complete (sdcp_device, NULL,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                               "Enumerate response too short (%zu)",
                                                               fp_cmd_payload_len (buffer_in)));
      return;
    }

  uid_list = (struct UidList *) (fp_cmd + 1);
  ids = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

  for (size_t i = 0; i < FOCALTECH_MOC_MAX_FINGERS; i++)
    {
      if (uid_list->actived[i] == 0)
        continue;

      g_ptr_array_add (ids, g_bytes_new (uid_list->uid[i], SDCP_ENROLLMENT_ID_SIZE));
    }

  fp_dbg ("focaltechmoc list: %u prints", ids->len);
  fpi_sdcp_device_list_complete (sdcp_device, g_steal_pointer (&ids), NULL);
}

static void
focaltech_moc_list (FpSdcpDevice *sdcp_device)
{
  FpDevice *device = FP_DEVICE (sdcp_device);
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = sizeof (struct UidList);

  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_ENUMERATE_TEMPLATE, NULL, cmd_len);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                         sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_list_cb);
}

enum FprintError {
  ERROR_NONE,
  ERROR_QUALITY,
  ERROR_SHORT,
  ERROR_LEFT,
  ERROR_RIGHT,
  ERROR_NONFINGER,
  ERROR_NOMOVE,
  ERROR_OTHER,
};

struct CaptureResult
{
  uint8_t error;
  uint8_t remain;
};

enum identify_states {
  MOC_IDENTIFY_RELEASE_FINGER,
  MOC_IDENTIFY_WAIT_FINGER,
  MOC_IDENTIFY_WAIT_FINGER_DELAY,
  MOC_IDENTIFY_CAPTURE,
  MOC_IDENTIFY_MATCH,
  MOC_IDENTIFY_NUM_STATES,
};

static void
focaltech_moc_identify_wait_finger_cb (FpiDeviceFocaltechMoc *self,
                                       uint8_t               *buffer_in,
                                       gsize                  length_in,
                                       GError                *error)
{
  FpCmd *fp_cmd = NULL;
  uint8_t *finger_status = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  finger_status = (uint8_t *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Can't get response!!"));
    }
  else
    {

      if (!focaltech_moc_require_payload_len (self, buffer_in,
                                              sizeof (uint8_t),
                                              "finger status"))
        return;

      if (*finger_status == 0x01)
        fpi_ssm_jump_to_state (self->task_ssm, MOC_IDENTIFY_CAPTURE);
      else
        fpi_ssm_jump_to_state (self->task_ssm, MOC_IDENTIFY_WAIT_FINGER_DELAY);
    }
}

static void
focaltech_moc_identify_wait_finger_delay (FpDevice *device,
                                          void     *user_data)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  fpi_ssm_jump_to_state (self->task_ssm, MOC_IDENTIFY_WAIT_FINGER);
}

static void
focaltech_moc_identify_capture_cb (FpiDeviceFocaltechMoc *self,
                                   uint8_t               *buffer_in,
                                   gsize                  length_in,
                                   GError                *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  struct CaptureResult *capture_result = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  capture_result = (struct CaptureResult *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Can't get response!!"));
    }
  else
    {
      if (!focaltech_moc_require_payload_len (self, buffer_in,
                                              sizeof (struct CaptureResult),
                                              "identify capture"))
        return;

      if (capture_result->error == ERROR_NONE)
        {
          fpi_ssm_next_state (self->task_ssm);
        }
      else
        {
          fpi_sdcp_device_identify_retry (FP_SDCP_DEVICE (self),
                                          fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));

          if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
            fpi_device_verify_complete (device, NULL);
          else
            fpi_device_identify_complete (device, NULL);

          fpi_ssm_mark_completed (self->task_ssm);
        }
    }
}

static void
focaltech_moc_identify_match_cb (FpiDeviceFocaltechMoc *self,
                                 uint8_t               *buffer_in,
                                 gsize                  length_in,
                                 GError                *error)
{
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  uint8_t *payload = NULL;

  g_autoptr(GBytes) id = NULL;
  g_autoptr(GBytes) mac = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  payload = (uint8_t *) (fp_cmd + 1);

  if (fp_cmd->code == FOCALTECH_MOC_RSP_INVALID)
    {
      /* no identify result available: report a clean no-match */
      fpi_sdcp_device_identify_complete (sdcp_device, NULL, NULL, NULL);
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Identify failed (status 0x%02x)",
                                                     fp_cmd->code));
      return;
    }

  if (!focaltech_moc_require_payload_len (self, buffer_in,
                                          SDCP_ENROLLMENT_ID_SIZE + SDCP_MAC_SIZE,
                                          "identify"))
    return;

  id = g_bytes_new (payload, SDCP_ENROLLMENT_ID_SIZE);
  mac = g_bytes_new (payload + SDCP_ENROLLMENT_ID_SIZE, SDCP_MAC_SIZE);

  fpi_sdcp_device_identify_complete (sdcp_device, id, mac, NULL);

  fpi_ssm_next_state (self->task_ssm);
}

static void
focaltech_identify_run_state (FpiSsm *ssm, FpDevice *device)
{
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MOC_IDENTIFY_RELEASE_FINGER:
      {
        uint8_t d1 = 0x78;
        cmd_len = sizeof (d1);
        resp_len = 0;
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_OPERATE_FP, &d1, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_release_finger);
        break;
      }

    case MOC_IDENTIFY_WAIT_FINGER:
      {
        uint8_t data = 0x02;
        cmd_len = sizeof (uint8_t);
        resp_len = sizeof (uint8_t);
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_FP_INFO, &data, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_identify_wait_finger_cb);
        break;
      }

    case MOC_IDENTIFY_WAIT_FINGER_DELAY:
      fpi_device_add_timeout (device, 50,
                              focaltech_moc_identify_wait_finger_delay,
                              NULL, NULL);
      break;

    case MOC_IDENTIFY_CAPTURE:
      cmd_len = 0;
      resp_len = sizeof (struct CaptureResult);
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CAPTURE, NULL, cmd_len);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                             sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                             1,
                             focaltech_moc_identify_capture_cb);
      break;

    case MOC_IDENTIFY_MATCH:
      {
        g_autoptr(GBytes) nonce = NULL;
        const guint8 *nonce_data;
        gsize nonce_len = 0;

        fpi_sdcp_device_get_identify_data (FP_SDCP_DEVICE (device), &nonce);
        nonce_data = g_bytes_get_data (nonce, &nonce_len);

        cmd_len = nonce_len;
        resp_len = SDCP_ENROLLMENT_ID_SIZE + SDCP_MAC_SIZE;
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_IDENTIFY, nonce_data, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_identify_match_cb);
        break;
      }
    }
}

static void
focaltech_moc_identify (FpSdcpDevice *sdcp_device)
{
  FpDevice *device = FP_DEVICE (sdcp_device);
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  self->task_ssm = fpi_ssm_new (device,
                                focaltech_identify_run_state,
                                MOC_IDENTIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, task_ssm_done);
}

enum moc_enroll_states {
  MOC_ENROLL_RELEASE_FINGER,
  MOC_ENROLL_PRE_ENROLL,
  MOC_ENROLL_WAIT_FINGER,
  MOC_ENROLL_WAIT_FINGER_DELAY,
  MOC_ENROLL_ENROLL_CAPTURE,
  MOC_ENROLL_COMMIT,
  MOC_ENROLL_NUM_STATES,
};

static void
focaltech_moc_enroll_wait_finger_cb (FpiDeviceFocaltechMoc *self,
                                     uint8_t               *buffer_in,
                                     gsize                  length_in,
                                     GError                *error)
{
  FpCmd *fp_cmd = NULL;
  uint8_t *finger_status = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  finger_status = (uint8_t *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_jump_to_state (self->task_ssm, MOC_ENROLL_WAIT_FINGER_DELAY);
    }
  else
    {

      if (!focaltech_moc_require_payload_len (self, buffer_in,
                                              sizeof (uint8_t),
                                              "finger status"))
        return;

      if (*finger_status == 0x01)
        fpi_ssm_jump_to_state (self->task_ssm, MOC_ENROLL_ENROLL_CAPTURE);
      else
        fpi_ssm_jump_to_state (self->task_ssm, MOC_ENROLL_WAIT_FINGER_DELAY);
    }
}

static void
focaltech_moc_enroll_wait_finger_delay (FpDevice *device,
                                        void     *user_data
                                       )
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  fpi_ssm_jump_to_state (self->task_ssm, MOC_ENROLL_WAIT_FINGER);
}

static void
focaltech_moc_pre_enroll_cb (FpiDeviceFocaltechMoc *self,
                             uint8_t               *buffer_in,
                             gsize                  length_in,
                             GError                *error)
{
  FpCmd *fp_cmd = NULL;
  uint8_t *nonce = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  nonce = (uint8_t *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      if (fp_cmd->code == FOCALTECH_MOC_RSP_PARA_ERR)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                         "device data full!!"));
        }
      else
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Can't get response!!"));
        }
      return;
    }

  if (!focaltech_moc_require_payload_len (self, buffer_in, SDCP_NONCE_SIZE, "pre enroll"))
    return;

  g_clear_pointer (&self->enrollment_nonce, g_bytes_unref);
  self->enrollment_nonce = g_bytes_new (nonce, SDCP_NONCE_SIZE);

  fpi_ssm_next_state (self->task_ssm);
}

static void
focaltech_moc_enroll_capture_cb (FpiDeviceFocaltechMoc *self,
                                 uint8_t               *buffer_in,
                                 gsize                  length_in,
                                 GError                *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  struct CaptureResult *capture_result = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;
  capture_result = (struct CaptureResult *) (fp_cmd + 1);

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Can't get response!!"));
    }
  else
    {
      if (!focaltech_moc_require_payload_len (self, buffer_in,
                                              sizeof (struct CaptureResult),
                                              "enroll capture"))
        return;

      if (capture_result->error == ERROR_NONE)
        {
          self->num_frames += 1;
          fpi_device_enroll_progress (device, self->num_frames, NULL, NULL);
          fp_info ("focaltechmoc remain: %d", capture_result->remain);
        }
      else
        {
          fpi_device_enroll_progress (device, self->num_frames, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
        }

      if (self->num_frames == fp_device_get_nr_enroll_stages (device))
        fpi_ssm_next_state (self->task_ssm);
      else
        fpi_ssm_jump_to_state (self->task_ssm, MOC_ENROLL_WAIT_FINGER);
    }
}

static void
focaltech_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case MOC_ENROLL_RELEASE_FINGER:
      {
        uint8_t d1 = 0x78;
        cmd_len = sizeof (d1);
        resp_len = 0;
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_OPERATE_FP, &d1, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_release_finger);
        break;
      }

    case MOC_ENROLL_PRE_ENROLL:
      cmd_len = 0;
      resp_len = SDCP_NONCE_SIZE;
      /* the device may open a session even if reading the reply fails */
      self->enroll_session_open = TRUE;
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_PRE_ENROLL, NULL, cmd_len);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                             sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                             1,
                             focaltech_moc_pre_enroll_cb);
      break;

    case MOC_ENROLL_WAIT_FINGER:
      {
        uint8_t data = 0x02;
        cmd_len = sizeof (uint8_t);
        resp_len = sizeof (uint8_t);
        cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_FP_INFO, &data, cmd_len);
        focaltech_moc_get_cmd (device, cmd_buf,
                               sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                               sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                               1,
                               focaltech_moc_enroll_wait_finger_cb);
        break;
      }

    case MOC_ENROLL_WAIT_FINGER_DELAY:
      fpi_device_add_timeout (device, 50,
                              focaltech_moc_enroll_wait_finger_delay,
                              NULL, NULL);
      break;

    case MOC_ENROLL_ENROLL_CAPTURE:
      cmd_len = 0;
      resp_len = sizeof (struct CaptureResult);
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CAPTURE, NULL, cmd_len);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                             sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                             1,
                             focaltech_moc_enroll_capture_cb);
      break;

    case MOC_ENROLL_COMMIT:
      g_assert (self->enrollment_nonce);
      fpi_sdcp_device_enroll_commit (FP_SDCP_DEVICE (device),
                                     self->enrollment_nonce, NULL);
      break;
    }
}

static void
focaltech_moc_enroll_abort_cb (FpiDeviceFocaltechMoc *self,
                               uint8_t               *buffer_in,
                               gsize                  length_in,
                               GError                *error)
{
  if (error)
    {
      fp_warn ("focaltechmoc enroll abort failed: %s", error->message);
      g_error_free (error);
    }

  fpi_device_action_error (FP_DEVICE (self),
                           g_steal_pointer (&self->enroll_error));
}

static void
enroll_task_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);
  gboolean session_pending = self->enroll_session_open;
  guint8 *cmd_buf = NULL;

  self->num_frames = 0;
  self->task_ssm = NULL;
  self->enroll_session_open = FALSE;
  g_clear_pointer (&self->enrollment_nonce, g_bytes_unref);

  if (error == NULL)
    return;

  if (!session_pending)
    {
      fpi_device_action_error (device, g_steal_pointer (&error));
      return;
    }

  /* close the device-side enrollment session before completing the action */
  self->enroll_error = g_steal_pointer (&error);
  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CANCEL_ENROLL, NULL, 0);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + sizeof (uint8_t),
                         sizeof (FpCmd) + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_enroll_abort_cb);
}

static void
focaltech_moc_enroll (FpSdcpDevice *sdcp_device)
{
  FpDevice *device = FP_DEVICE (sdcp_device);
  FpiDeviceFocaltechMoc *self = FPI_DEVICE_FOCALTECH_MOC (device);

  self->task_ssm = fpi_ssm_new (device,
                                focaltech_enroll_run_state,
                                MOC_ENROLL_NUM_STATES);
  fpi_ssm_start (self->task_ssm, enroll_task_ssm_done);
}

static void
focaltech_moc_commit_abort_cb (FpiDeviceFocaltechMoc *self,
                               uint8_t               *buffer_in,
                               gsize                  length_in,
                               GError                *error)
{
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);

  if (error)
    {
      fp_warn ("focaltechmoc enroll abort failed: %s", error->message);
      g_error_free (error);
    }

  fpi_sdcp_device_enroll_commit_complete (sdcp_device,
                                          g_steal_pointer (&self->enroll_error));
  fpi_ssm_mark_completed (self->task_ssm);
}

static void
focaltech_moc_enroll_commit_cb (FpiDeviceFocaltechMoc *self,
                                uint8_t               *buffer_in,
                                gsize                  length_in,
                                GError                *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);
  FpCmd *fp_cmd = NULL;
  guint8 *cmd_buf = NULL;

  g_clear_pointer (&self->enrollment_nonce, g_bytes_unref);

  if (error)
    {
      /* the device may still hold the session open: abort it first */
      self->enroll_error = g_steal_pointer (&error);
      self->enroll_session_open = FALSE;
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CANCEL_ENROLL, NULL, 0);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + sizeof (uint8_t),
                             sizeof (FpCmd) + sizeof (uint8_t),
                             FALSE,
                             focaltech_moc_commit_abort_cb);
      return;
    }

  /* a status reply means the device processed the commit and closed the session */
  self->enroll_session_open = FALSE;

  fp_cmd = (FpCmd *) buffer_in;

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK)
    {
      fpi_sdcp_device_enroll_commit_complete (sdcp_device,
                                              fpi_device_error_new_msg (FP_DEVICE_ERROR_UNTRUSTED,
                                                                        "Enrollment was rejected by the device (status 0x%02x)",
                                                                        fp_cmd->code));
      fpi_ssm_mark_completed (self->task_ssm);
      return;
    }

  fpi_sdcp_device_enroll_commit_complete (sdcp_device, NULL);
  fpi_ssm_next_state (self->task_ssm);
}

static void
focaltech_moc_cancel_enroll_cb (FpiDeviceFocaltechMoc *self,
                                uint8_t               *buffer_in,
                                gsize                  length_in,
                                GError                *error)
{
  FpSdcpDevice *sdcp_device = FP_SDCP_DEVICE (self);

  self->enroll_session_open = FALSE;
  g_clear_pointer (&self->enrollment_nonce, g_bytes_unref);

  if (error)
    fpi_sdcp_device_enroll_commit_complete (sdcp_device, error);
  else
    fpi_sdcp_device_enroll_commit_complete (sdcp_device,
                                            fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                      "Enrollment aborted"));

  fpi_ssm_mark_completed (self->task_ssm);
}

static void
focaltech_moc_enroll_commit (FpSdcpDevice *sdcp_device,
                             GBytes       *id)
{
  FpDevice *device = FP_DEVICE (sdcp_device);
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = 0;
  const guint8 *id_data;
  gsize id_len = 0;

  if (id == NULL)
    {
      cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_CANCEL_ENROLL, NULL, 0);
      focaltech_moc_get_cmd (device, cmd_buf,
                             sizeof (FpCmd) + sizeof (uint8_t),
                             sizeof (FpCmd) + sizeof (uint8_t),
                             FALSE,
                             focaltech_moc_cancel_enroll_cb);
      return;
    }

  id_data = g_bytes_get_data (id, &id_len);

  cmd_len = id_len;
  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_ENROLL_COMMIT, id_data, cmd_len);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                         sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_enroll_commit_cb);
}

static void
focaltech_moc_delete_cb (FpiDeviceFocaltechMoc *self,
                         uint8_t               *buffer_in,
                         gsize                  length_in,
                         GError                *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpCmd *fp_cmd = NULL;

  if (error)
    {
      fpi_device_delete_complete (device, error);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK && fp_cmd->code != FOCALTECH_MOC_RSP_INVALID)
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                            "Delete failed (status 0x%02x)",
                                                            fp_cmd->code));
      return;
    }

  fpi_device_delete_complete (device, NULL);
}

static void
focaltech_moc_delete_print (FpDevice *device)
{
  FpPrint *print = NULL;
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = 0;
  const guint8 *id_data;
  gsize id_len = 0;

  g_autoptr(GBytes) id = NULL;

  fpi_device_get_delete_data (device, &print);
  fpi_sdcp_device_get_print_id (print, &id);

  if (!id || g_bytes_get_size (id) != SDCP_ENROLLMENT_ID_SIZE)
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  id_data = g_bytes_get_data (id, &id_len);

  cmd_len = id_len;
  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_SDCP_DELETE_TEMPLATE, id_data, cmd_len);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                         sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_delete_cb);
}

static void
focaltech_moc_clear_storage_cb (FpiDeviceFocaltechMoc *self,
                                uint8_t               *buffer_in,
                                gsize                  length_in,
                                GError                *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpCmd *fp_cmd = NULL;

  if (error)
    {
      fp_warn ("Mass erase command completed with warning: %s", error->message);
      fpi_device_clear_storage_complete (device, error);
      return;
    }

  if (length_in == 0)
    {
      fpi_device_clear_storage_complete (device, NULL);
      return;
    }

  fp_cmd = (FpCmd *) buffer_in;

  if (fp_cmd->code != FOCALTECH_MOC_RSP_OK && fp_cmd->code != FOCALTECH_MOC_RSP_INVALID)
    {
      fpi_device_clear_storage_complete (device,
                                         fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                   "Clear storage failed: unexpected response code 0x%02x",
                                                                   fp_cmd->code));
      return;
    }

  fpi_device_clear_storage_complete (device, NULL);
}

static void
focaltech_moc_clear_storage (FpDevice *device)
{
  guint8 *cmd_buf = NULL;
  uint16_t cmd_len = 0;
  uint16_t resp_len = sizeof (uint8_t);

  cmd_buf = focaltech_moc_compose_cmd (FOCALTECH_MOC_CMD_DELETE_ALL_TEMPLATE, NULL, cmd_len);
  focaltech_moc_get_cmd (device, cmd_buf,
                         sizeof (FpCmd) + cmd_len + sizeof (uint8_t),
                         sizeof (FpCmd) + resp_len + sizeof (uint8_t),
                         FALSE,
                         focaltech_moc_clear_storage_cb);
}

static void
fpi_device_focaltech_moc_init (FpiDeviceFocaltechMoc *self)
{
  G_DEBUG_HERE ();
}

static void
fpi_device_focaltech_moc_class_init (FpiDeviceFocaltechMocClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpSdcpDeviceClass *sdcp_dev_class = FP_SDCP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = FOCALTECH_MOC_DRIVER_FULLNAME;

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = FOCALTECH_MOC_MAX_FINGERS;
  dev_class->temp_hot_seconds = -1;

  sdcp_dev_class->open = focaltech_moc_open;
  sdcp_dev_class->connect = focaltech_moc_connect;
  sdcp_dev_class->list = focaltech_moc_list;
  sdcp_dev_class->enroll = focaltech_moc_enroll;
  sdcp_dev_class->enroll_commit = focaltech_moc_enroll_commit;
  sdcp_dev_class->identify = focaltech_moc_identify;

  dev_class->close = focaltech_moc_close;
  dev_class->delete = focaltech_moc_delete_print;
  dev_class->clear_storage = focaltech_moc_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);

  sdcp_dev_class->ignore_device_certificate = FALSE;
  sdcp_dev_class->ignore_device_signatures = FALSE;
}
