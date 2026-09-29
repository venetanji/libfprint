/*
 * Goodix 27c6:5125 asynchronous USB building blocks
 * Copyright (C) 2026 Goodix 5125 driver contributors
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "goodix5125-usb-async.h"
#include "goodix5125-usb.h"

#include <gio/gio.h>

#define MAX_IN_FRAME 0x10000
#define USB_PACKET_SIZE 0x40

typedef struct
{
  Goodix5125Command      command;
  Goodix5125ChecksumMode reply_checksum_mode;
  Goodix5125OuterFlag    reply_flag;
  guint                  timeout_ms;
  gboolean               expect_reply;
  gboolean               acked;
  guint                  n_rx;
} Goodix5125CommandAsyncData;

static gboolean
set_proto_error (Goodix5125ProtoResult result,
                 const char           *operation,
                 GError              **error)
{
  if (result == GOODIX5125_PROTO_OK)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "%s failed with protocol status %u", operation, result);
  return FALSE;
}

static gboolean
validate_async_wait (guint         timeout_ms,
                     GCancellable *cancellable,
                     GError      **error)
{
  if (timeout_ms != 0 || cancellable != NULL)
    return TRUE;

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "An unbounded Goodix USB wait requires a cancellable");
  return FALSE;
}

static void
outer_write_transfer_cb (FpiUsbTransfer *transfer,
                         FpDevice       *device,
                         gpointer        user_data,
                         GError         *error)
{
  g_autoptr(GTask) task = G_TASK (user_data);

  if (error != NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

void
goodix5125_usb_outer_write_async (FpDevice           *device,
                                  Goodix5125OuterFlag flag,
                                  const guint8       *payload,
                                  gsize               payload_length,
                                  guint               timeout_ms,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(FpiUsbTransfer) transfer = NULL;
  g_autofree guint8 *frame = NULL;
  g_autoptr(GError) error = NULL;
  Goodix5125ProtoResult result;
  gsize frame_length;
  gsize padded_length;
  gsize written = 0;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (device, cancellable, callback, user_data);
  g_task_set_source_tag (task, goodix5125_usb_outer_write_async);

  if (!validate_async_wait (timeout_ms, cancellable, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (payload_length > G_MAXUINT16)
    {
      set_proto_error (GOODIX5125_PROTO_INVALID_LENGTH,
                       "Goodix outer frame encoding", &error);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  frame_length = GOODIX5125_OUTER_HEADER_SIZE + payload_length;
  padded_length = ((frame_length + USB_PACKET_SIZE - 1) /
                   USB_PACKET_SIZE) * USB_PACKET_SIZE;
  frame = g_malloc0 (padded_length);
  result = goodix5125_proto_outer_encode (flag, payload, payload_length,
                                          frame, padded_length, &written);
  if (!set_proto_error (result, "Goodix outer frame encoding", &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  g_assert_cmpuint (written, ==, frame_length);

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (transfer, GOODIX5125_USB_EP_OUT,
                                   g_steal_pointer (&frame), padded_length,
                                   g_free);
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer), timeout_ms,
                           cancellable, outer_write_transfer_cb,
                           g_steal_pointer (&task));
}

gboolean
goodix5125_usb_outer_write_finish (FpDevice     *device,
                                   GAsyncResult *result,
                                   GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, device), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result, goodix5125_usb_outer_write_async), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
outer_read_transfer_cb (FpiUsbTransfer *transfer,
                        FpDevice       *device,
                        gpointer        user_data,
                        GError         *error)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  Goodix5125OuterFlag expected_flag;
  Goodix5125OuterFrame frame;
  Goodix5125ProtoResult result;
  g_autoptr(GError) local_error = NULL;

  if (error == NULL)
    goodix5125_usb_in_check_empty (transfer, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  result = goodix5125_proto_outer_decode (transfer->buffer,
                                          transfer->actual_length, &frame);
  if (!set_proto_error (result, "Goodix outer frame decoding", &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  expected_flag = GPOINTER_TO_UINT (g_task_get_task_data (task));
  if (frame.flag != expected_flag)
    {
      g_task_return_new_error (
        task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Unexpected Goodix outer flag 0x%02x, expected 0x%02x",
        frame.flag, expected_flag);
      return;
    }

  g_task_return_pointer (task,
                         g_bytes_new (frame.payload, frame.payload_len),
                         (GDestroyNotify) g_bytes_unref);
}

void
goodix5125_usb_outer_read_async (FpDevice           *device,
                                 Goodix5125OuterFlag expected_flag,
                                 guint               timeout_ms,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(FpiUsbTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (device, cancellable, callback, user_data);
  g_task_set_source_tag (task, goodix5125_usb_outer_read_async);

  if (!validate_async_wait (timeout_ms, cancellable, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (!goodix5125_proto_flag_is_supported (expected_flag))
    {
      set_proto_error (GOODIX5125_PROTO_INVALID_FLAG,
                       "Goodix expected outer flag", &error);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_set_task_data (task, GUINT_TO_POINTER (expected_flag), NULL);
  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (transfer, GOODIX5125_USB_EP_IN, MAX_IN_FRAME);
  fpi_usb_transfer_submit (g_steal_pointer (&transfer), timeout_ms,
                           cancellable, outer_read_transfer_cb,
                           g_steal_pointer (&task));
}

GBytes *
goodix5125_usb_outer_read_finish (FpDevice     *device,
                                  GAsyncResult *result,
                                  GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, device), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result, goodix5125_usb_outer_read_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void command_read_next (FpDevice *device,
                               GTask    *task);

/* The ACK is optional (the MCU drops it when the reply follows at once) and
 * stale replies of earlier commands are skipped; see
 * goodix5125_proto_classify_rx(). */
static void
command_rx_cb (FpiUsbTransfer *transfer,
               FpDevice       *device,
               gpointer        user_data,
               GError         *error)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  Goodix5125CommandAsyncData *data = g_task_get_task_data (task);
  g_autoptr(GError) local_error = error;
  Goodix5125ProtoResult proto_result;
  Goodix5125RxKind kind;
  const guint8 *body;
  gsize body_length = 0;

  if (local_error == NULL)
    goodix5125_usb_in_check_empty (transfer, &local_error);
  if (local_error != NULL)
    {
      if (!data->expect_reply &&
          g_error_matches (local_error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          fp_dbg ("ACK of command 0x%02x lost, assuming success",
                  data->command);
          g_task_return_pointer (task, g_bytes_new_static ("", 0),
                                 (GDestroyNotify) g_bytes_unref);
          return;
        }
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  proto_result = goodix5125_proto_classify_rx (
    transfer->buffer, transfer->actual_length, data->command,
    data->reply_flag, data->reply_checksum_mode, &kind, &body, &body_length);
  if (!set_proto_error (proto_result, "Goodix reply decoding", &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  switch (kind)
    {
    case GOODIX5125_RX_ACK:
      data->acked = TRUE;
      if (!data->expect_reply)
        {
          g_task_return_pointer (task, g_bytes_new_static ("", 0),
                                 (GDestroyNotify) g_bytes_unref);
          return;
        }
      break;

    case GOODIX5125_RX_REPLY:
      if (!data->acked)
        fp_dbg ("ACK of command 0x%02x lost before its reply", data->command);
      g_task_return_pointer (task, g_bytes_new (body, body_length),
                             (GDestroyNotify) g_bytes_unref);
      return;

    case GOODIX5125_RX_SKIP:
    default:
      fp_dbg ("skipping an unrelated message while waiting for 0x%02x",
              data->command);
      break;
    }

  if (++data->n_rx >= GOODIX5125_MAX_RX_MESSAGES)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "No reply to Goodix command 0x%02x among %u "
                               "messages", data->command,
                               GOODIX5125_MAX_RX_MESSAGES);
      return;
    }
  command_read_next (device, g_steal_pointer (&task));
}

static void
command_read_next (FpDevice *device,
                   GTask    *task)
{
  Goodix5125CommandAsyncData *data = g_task_get_task_data (task);
  GCancellable *cancellable = g_task_get_cancellable (task);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (device);
  guint timeout_ms = data->timeout_ms;

  if (!data->expect_reply)
    timeout_ms = timeout_ms ? MIN (timeout_ms, GOODIX5125_ACK_ONLY_TIMEOUT_MS) :
                 GOODIX5125_ACK_ONLY_TIMEOUT_MS;
  fpi_usb_transfer_fill_bulk (transfer, GOODIX5125_USB_EP_IN, MAX_IN_FRAME);
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable, command_rx_cb,
                           task);
}

static void
command_write_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  FpDevice *device = FP_DEVICE (source_object);
  g_autoptr(GError) error = NULL;

  if (!goodix5125_usb_outer_write_finish (device, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  command_read_next (device, g_steal_pointer (&task));
}

void
goodix5125_usb_command_async (FpDevice              *device,
                              Goodix5125Command      command,
                              Goodix5125ChecksumMode request_checksum_mode,
                              Goodix5125ChecksumMode reply_checksum_mode,
                              const guint8          *payload,
                              gsize                  payload_length,
                              gboolean               expect_reply,
                              Goodix5125OuterFlag    reply_flag,
                              guint                  timeout_ms,
                              GCancellable          *cancellable,
                              GAsyncReadyCallback    callback,
                              gpointer               user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree guint8 *inner = NULL;
  g_autoptr(GError) error = NULL;
  Goodix5125CommandAsyncData *data;
  Goodix5125ProtoResult result;
  gsize inner_capacity;
  gsize inner_length = 0;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (device, cancellable, callback, user_data);
  g_task_set_source_tag (task, goodix5125_usb_command_async);

  if (!validate_async_wait (timeout_ms, cancellable, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (payload_length > G_MAXUINT16 - GOODIX5125_INNER_TRAILER_SIZE)
    {
      set_proto_error (GOODIX5125_PROTO_INVALID_LENGTH,
                       "Goodix command encoding", &error);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  inner_capacity = GOODIX5125_INNER_HEADER_SIZE + payload_length +
                   GOODIX5125_INNER_TRAILER_SIZE;
  inner = g_malloc (inner_capacity);
  result = goodix5125_proto_inner_encode (
    command, request_checksum_mode, payload, payload_length,
    inner, inner_capacity, &inner_length);
  if (!set_proto_error (result, "Goodix command encoding", &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  data = g_new0 (Goodix5125CommandAsyncData, 1);
  data->command = command;
  data->reply_checksum_mode = reply_checksum_mode;
  data->reply_flag = reply_flag;
  data->timeout_ms = timeout_ms;
  data->expect_reply = expect_reply;
  g_task_set_task_data (task, data, g_free);

  goodix5125_usb_outer_write_async (
    device, GOODIX5125_OUTER_FLAG_MESSAGE, inner, inner_length, timeout_ms,
    cancellable, command_write_cb, g_steal_pointer (&task));
}

GBytes *
goodix5125_usb_command_finish (FpDevice     *device,
                               GAsyncResult *result,
                               GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, device), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result, goodix5125_usb_command_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
