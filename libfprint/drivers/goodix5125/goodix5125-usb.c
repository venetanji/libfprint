/*
 * Goodix 27c6:5125 synchronous USB building blocks
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
      if (fpi_usb_transfer_submit_sync (transfer, timeout_ms, &local_error))
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
  if (!fpi_usb_transfer_submit_sync (transfer, timeout_ms, error))
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
  if (!set_proto_error (result, "Goodix NOP ACK decoding", error))
    return FALSE;
  result = goodix5125_proto_ack_validate (&ack, GOODIX5125_COMMAND_NOP,
                                          &has_data);
  return set_proto_error (result, "Goodix NOP ACK validation", error);
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

  g_autoptr(GBytes) ack_bytes = NULL;
  g_autoptr(GBytes) reply_bytes = NULL;
  Goodix5125InnerFrame ack;
  Goodix5125InnerFrame reply;
  Goodix5125ProtoResult result;
  gsize inner_capacity;
  gsize inner_length = 0;
  gsize raw_length = 0;
  gconstpointer raw;
  gboolean has_data = FALSE;

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

  ack_bytes = goodix5125_usb_outer_read_sync (
    device, GOODIX5125_OUTER_FLAG_MESSAGE, timeout_ms, error);
  if (!ack_bytes)
    return NULL;
  raw = g_bytes_get_data (ack_bytes, &raw_length);
  result = goodix5125_proto_inner_decode (raw, raw_length,
                                          GOODIX5125_CHECKSUM_NORMAL, &ack);
  if (!set_proto_error (result, "Goodix ACK decoding", error))
    return NULL;
  result = goodix5125_proto_ack_validate (&ack, command, &has_data);
  if (!set_proto_error (result, "Goodix ACK validation", error))
    return NULL;

  if (!expect_reply)
    return g_bytes_new_static ("", 0);

  /* The ChicagoHS firmware does not set the ACK data bit consistently.
   * In particular, it may clear it and still queue the command reply.  The
   * known-good OEM/Python sequence therefore validates the ACK but treats
   * this bit as advisory.  Whether a reply is expected is a property of the
   * command call above, so always read it here when expect_reply is TRUE. */

  reply_bytes = goodix5125_usb_outer_read_sync (device, reply_flag,
                                                timeout_ms, error);
  if (!reply_bytes)
    return NULL;
  if (reply_flag != GOODIX5125_OUTER_FLAG_MESSAGE)
    return g_steal_pointer (&reply_bytes);

  raw = g_bytes_get_data (reply_bytes, &raw_length);
  result = goodix5125_proto_inner_decode (raw, raw_length,
                                          reply_checksum_mode, &reply);
  if (!set_proto_error (result, "Goodix reply decoding", error))
    return NULL;
  if (reply.command != command)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Unexpected Goodix reply command 0x%02x, expected 0x%02x",
                   reply.command, command);
      return NULL;
    }
  return g_bytes_new (reply.payload, reply.payload_len);
}
