/*
 * Goodix 27c6:5125 synchronous USB building blocks
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * These helpers are limited to short initialization exchanges. Long waits and
 * capture loops are implemented asynchronously by the image-device state
 * machine.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "goodix5125-usb.h"

#include <gio/gio.h>

#define MAX_IN_FRAME 0x10000
#define USB_PACKET_SIZE 0x40
#define MAX_DRAIN_FRAMES 32

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

/* A bulk IN transfer that completed without data is reported as a timeout:
 * device emulation (umockdev) replays a read that timed out on the device
 * as an empty completion instead of letting it time out again. */
gboolean
goodix5125_usb_in_check_empty (FpiUsbTransfer *transfer,
                               GError        **error)
{
  if (transfer->actual_length > 0)
    return TRUE;
  g_set_error_literal (error, G_USB_DEVICE_ERROR,
                       G_USB_DEVICE_ERROR_TIMED_OUT,
                       "Goodix USB read completed without data");
  return FALSE;
}

gboolean
goodix5125_usb_drain_sync (FpDevice *device,
                           guint     timeout_ms,
                           GError  **error)
{
  g_return_val_if_fail (FP_IS_DEVICE (device), FALSE);

  for (guint frame = 0; frame < MAX_DRAIN_FRAMES; frame++)
    {
      g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (device);
      g_autoptr(GError) local_error = NULL;

      fpi_usb_transfer_fill_bulk (transfer, GOODIX5125_USB_EP_IN,
                                  MAX_IN_FRAME);
      if (fpi_usb_transfer_submit_sync (transfer, timeout_ms, &local_error) &&
          goodix5125_usb_in_check_empty (transfer, &local_error))
        continue;

      if (g_error_matches (local_error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Goodix USB input did not become idle while draining");
  return FALSE;
}

gboolean
goodix5125_usb_outer_write_sync (FpDevice           *device,
                                 Goodix5125OuterFlag flag,
                                 const guint8       *payload,
                                 gsize               payload_length,
                                 guint               timeout_ms,
                                 GError            **error)
{
  g_autoptr(FpiUsbTransfer) transfer = NULL;
  g_autofree guint8 *frame = NULL;
  gsize frame_length;
  gsize padded_length;
  gsize written = 0;
  Goodix5125ProtoResult result;

  g_return_val_if_fail (FP_IS_DEVICE (device), FALSE);

  frame_length = GOODIX5125_OUTER_HEADER_SIZE + payload_length;
  padded_length = ((frame_length + USB_PACKET_SIZE - 1) /
                   USB_PACKET_SIZE) * USB_PACKET_SIZE;
  frame = g_malloc0 (padded_length);
  result = goodix5125_proto_outer_encode (flag, payload, payload_length,
                                          frame, padded_length, &written);
  if (!set_proto_error (result, "Goodix outer frame encoding", error))
    return FALSE;
  g_assert_cmpuint (written, ==, frame_length);

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (transfer, GOODIX5125_USB_EP_OUT,
                                   g_steal_pointer (&frame), padded_length,
                                   g_free);
  transfer->short_is_error = TRUE;
  return fpi_usb_transfer_submit_sync (transfer, timeout_ms, error);
}

static GBytes *
read_raw_sync (FpDevice *device,
               guint     timeout_ms,
               GError  **error)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (device);

  fpi_usb_transfer_fill_bulk (transfer, GOODIX5125_USB_EP_IN, MAX_IN_FRAME);
  if (!fpi_usb_transfer_submit_sync (transfer, timeout_ms, error) ||
      !goodix5125_usb_in_check_empty (transfer, error))
    return NULL;
  return g_bytes_new (transfer->buffer, transfer->actual_length);
}

GBytes *
goodix5125_usb_outer_read_sync (FpDevice           *device,
                                Goodix5125OuterFlag expected_flag,
                                guint               timeout_ms,
                                GError            **error)
{
  g_autoptr(FpiUsbTransfer) transfer = NULL;
  Goodix5125OuterFrame frame;
  Goodix5125ProtoResult result;

  g_return_val_if_fail (FP_IS_DEVICE (device), NULL);

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk (transfer, GOODIX5125_USB_EP_IN, MAX_IN_FRAME);
  if (!fpi_usb_transfer_submit_sync (transfer, timeout_ms, error) ||
      !goodix5125_usb_in_check_empty (transfer, error))
    return NULL;

  result = goodix5125_proto_outer_decode (transfer->buffer,
                                          transfer->actual_length, &frame);
  if (!set_proto_error (result, "Goodix outer frame decoding", error))
    return NULL;
  if (frame.flag != expected_flag)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Unexpected Goodix outer flag 0x%02x, expected 0x%02x",
                   frame.flag, expected_flag);
      return NULL;
    }
  return g_bytes_new (frame.payload, frame.payload_len);
}

gboolean
goodix5125_usb_nop_sync (FpDevice *device,
                         guint     timeout_ms,
                         GError  **error)
{
  static const guint8 payload[4] = { 0 };
  guint8 inner[GOODIX5125_INNER_HEADER_SIZE + sizeof (payload) +
               GOODIX5125_INNER_TRAILER_SIZE];
  gsize inner_length = 0;

  g_autoptr(GBytes) ack_bytes = NULL;
  g_autoptr(GError) local_error = NULL;
  Goodix5125InnerFrame ack;
  Goodix5125ProtoResult result;
  gsize raw_length = 0;
  gconstpointer raw;
  gboolean has_data = FALSE;

  result = goodix5125_proto_inner_encode (
    GOODIX5125_COMMAND_NOP, GOODIX5125_CHECKSUM_TRAILER_88,
    payload, sizeof (payload), inner, sizeof (inner), &inner_length);
  if (!set_proto_error (result, "Goodix NOP encoding", error) ||
      !goodix5125_usb_outer_write_sync (device,
                                        GOODIX5125_OUTER_FLAG_MESSAGE,
                                        inner, inner_length,
                                        timeout_ms, error))
    return FALSE;

  ack_bytes = goodix5125_usb_outer_read_sync (
    device, GOODIX5125_OUTER_FLAG_MESSAGE, timeout_ms, &local_error);
  if (!ack_bytes)
    {
      /* The OEM sequence treats a NOP timeout as a valid no-response case. */
      if (g_error_matches (local_error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        return TRUE;
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  raw = g_bytes_get_data (ack_bytes, &raw_length);
  result = goodix5125_proto_inner_decode (raw, raw_length,
                                          GOODIX5125_CHECKSUM_NORMAL, &ack);
  if (result == GOODIX5125_PROTO_OK)
    result = goodix5125_proto_ack_validate (&ack, GOODIX5125_COMMAND_NOP,
                                            &has_data);
  /* A NOP only flushes the MCU; a stale message instead of its ACK is fine. */
  if (result != GOODIX5125_PROTO_OK)
    fp_dbg ("ignoring a non-ACK answer to NOP (status %u)", result);
  return TRUE;
}

GBytes *
goodix5125_usb_command_sync (FpDevice              *device,
                             Goodix5125Command      command,
                             Goodix5125ChecksumMode request_checksum_mode,
                             Goodix5125ChecksumMode reply_checksum_mode,
                             const guint8          *payload,
                             gsize                  payload_length,
                             gboolean               expect_reply,
                             Goodix5125OuterFlag    reply_flag,
                             guint                  timeout_ms,
                             GError               **error)
{
  g_autofree guint8 *inner = NULL;
  Goodix5125ProtoResult result;
  gsize inner_capacity;
  gsize inner_length = 0;
  gboolean acked = FALSE;

  inner_capacity = GOODIX5125_INNER_HEADER_SIZE + payload_length +
                   GOODIX5125_INNER_TRAILER_SIZE;
  inner = g_malloc (inner_capacity);
  result = goodix5125_proto_inner_encode (command, request_checksum_mode,
                                          payload, payload_length,
                                          inner, inner_capacity,
                                          &inner_length);
  if (!set_proto_error (result, "Goodix command encoding", error) ||
      !goodix5125_usb_outer_write_sync (device,
                                        GOODIX5125_OUTER_FLAG_MESSAGE,
                                        inner, inner_length,
                                        timeout_ms, error))
    return NULL;

  /* The ACK is optional (the MCU drops it when the reply follows at once);
   * the ChicagoHS firmware also does not set its data bit reliably, so the
   * caller's contract decides whether a reply is awaited. */
  for (guint n = 0; n < GOODIX5125_MAX_RX_MESSAGES; n++)
    {
      g_autoptr(GBytes) raw = NULL;
      g_autoptr(GError) local_error = NULL;
      Goodix5125RxKind kind;
      const guint8 *data;
      const guint8 *body;
      gsize length = 0;
      gsize body_length = 0;

      raw = read_raw_sync (device,
                           expect_reply ? timeout_ms :
                           MIN (timeout_ms, GOODIX5125_ACK_ONLY_TIMEOUT_MS),
                           &local_error);
      if (!raw)
        {
          if (!expect_reply &&
              g_error_matches (local_error, G_USB_DEVICE_ERROR,
                               G_USB_DEVICE_ERROR_TIMED_OUT))
            {
              fp_dbg ("ACK of command 0x%02x lost, assuming success", command);
              return g_bytes_new_static ("", 0);
            }
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }

      data = g_bytes_get_data (raw, &length);
      result = goodix5125_proto_classify_rx (data, length, command, reply_flag,
                                             reply_checksum_mode, &kind,
                                             &body, &body_length);
      if (!set_proto_error (result, "Goodix reply decoding", error))
        return NULL;
      switch (kind)
        {
        case GOODIX5125_RX_ACK:
          acked = TRUE;
          if (!expect_reply)
            return g_bytes_new_static ("", 0);
          break;

        case GOODIX5125_RX_REPLY:
          if (!acked)
            fp_dbg ("ACK of command 0x%02x lost before its reply", command);
          return g_bytes_new (body, body_length);

        case GOODIX5125_RX_SKIP:
        default:
          fp_dbg ("skipping an unrelated message while waiting for 0x%02x",
                  command);
          break;
        }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "No reply to Goodix command 0x%02x among %u messages",
               command, GOODIX5125_MAX_RX_MESSAGES);
  return NULL;
}
