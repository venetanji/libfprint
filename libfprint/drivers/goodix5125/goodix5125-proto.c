/*
 * Goodix 5125 driver for libfprint
 * Copyright (C) 2026 Goodix 5125 driver contributors
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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

#include "goodix5125-proto.h"

#include <string.h>

#define GOODIX5125_INNER_CHECKSUM_TARGET 0xaa
#define GOODIX5125_NOP_TRAILER 0x88
#define GOODIX5125_CONFIG_CHECKSUM_SEED 0xa5a5

static guint16
read_uint16_le (const guint8 *data)
{
  return data[0] | ((guint16) data[1] << 8);
}

static void
write_uint16_le (guint8 *data,
                 guint16 value)
{
  data[0] = (guint8) (value & 0xff);
  data[1] = (guint8) (value >> 8);
}

gboolean
goodix5125_proto_flag_is_supported (guint8 value)
{
  switch (value)
    {
    case GOODIX5125_OUTER_FLAG_MESSAGE:
    case GOODIX5125_OUTER_FLAG_TLS:
    case GOODIX5125_OUTER_FLAG_TLS_DATA:
      return TRUE;

    default:
      return FALSE;
    }
}

gboolean
goodix5125_proto_command_is_supported (guint8 value)
{
  switch (value)
    {
    case GOODIX5125_COMMAND_NOP:
    case GOODIX5125_COMMAND_GET_IMAGE:
    case GOODIX5125_COMMAND_FDT_DOWN:
    case GOODIX5125_COMMAND_FDT_UP:
    case GOODIX5125_COMMAND_FDT_MODE:
    case GOODIX5125_COMMAND_NAVIGATION:
    case GOODIX5125_COMMAND_MCU_SLEEP:
    case GOODIX5125_COMMAND_MCU_IDLE:
    case GOODIX5125_COMMAND_WRITE_SENSOR_REGISTER:
    case GOODIX5125_COMMAND_READ_SENSOR_REGISTER:
    case GOODIX5125_COMMAND_UPLOAD_CONFIG:
    case GOODIX5125_COMMAND_SLEEP:
    case GOODIX5125_COMMAND_SET_POWERDOWN_FREQUENCY:
    case GOODIX5125_COMMAND_ENABLE:
    case GOODIX5125_COMMAND_RESET:
    case GOODIX5125_COMMAND_READ_OTP:
    case GOODIX5125_COMMAND_QUERY_STATE:
    case GOODIX5125_COMMAND_ACK:
    case GOODIX5125_COMMAND_REQUEST_TLS:
    case GOODIX5125_COMMAND_GET_POV_IMAGE:
    case GOODIX5125_COMMAND_TLS_ESTABLISHED:
    case GOODIX5125_COMMAND_WRITE_PSK:
    case GOODIX5125_COMMAND_READ_PSK_HASH:
      return TRUE;

    default:
      return FALSE;
    }
}

guint8
goodix5125_proto_outer_checksum (const guint8 header[3])
{
  guint sum;

  g_return_val_if_fail (header != NULL, 0);

  sum = (guint) header[0] + header[1] + header[2];
  return sum & 0xff;
}

guint8
goodix5125_proto_inner_checksum (const guint8 *data,
                                 gsize         data_len)
{
  guint8 sum = 0;
  gsize i;

  g_return_val_if_fail (data != NULL || data_len == 0, 0);

  for (i = 0; i < data_len; i++)
    sum += data[i];

  return GOODIX5125_INNER_CHECKSUM_TARGET - sum;
}

Goodix5125ProtoResult
goodix5125_proto_outer_encode (Goodix5125OuterFlag flag,
                               const guint8       *payload,
                               gsize               payload_len,
                               guint8             *output,
                               gsize               output_size,
                               gsize              *written)
{
  gsize frame_len;

  if (output == NULL || written == NULL || (payload == NULL && payload_len != 0))
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  *written = 0;

  if (!goodix5125_proto_flag_is_supported (flag))
    return GOODIX5125_PROTO_INVALID_FLAG;

  if (payload_len > G_MAXUINT16)
    return GOODIX5125_PROTO_INVALID_LENGTH;

  frame_len = GOODIX5125_OUTER_HEADER_SIZE + payload_len;
  if (output_size < frame_len)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  output[0] = flag;
  write_uint16_le (output + 1, (guint16) payload_len);
  output[3] = goodix5125_proto_outer_checksum (output);
  if (payload_len != 0)
    memcpy (output + GOODIX5125_OUTER_HEADER_SIZE, payload, payload_len);

  *written = frame_len;
  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_outer_decode (const guint8         *buffer,
                               gsize                 buffer_len,
                               Goodix5125OuterFrame *frame)
{
  guint16 payload_len;
  gsize frame_len;

  if (buffer == NULL || frame == NULL)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  if (buffer_len < GOODIX5125_OUTER_HEADER_SIZE)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  if (!goodix5125_proto_flag_is_supported (buffer[0]))
    return GOODIX5125_PROTO_INVALID_FLAG;

  if (buffer[3] != goodix5125_proto_outer_checksum (buffer))
    return GOODIX5125_PROTO_INVALID_CHECKSUM;

  payload_len = read_uint16_le (buffer + 1);
  frame_len = GOODIX5125_OUTER_HEADER_SIZE + payload_len;
  if (buffer_len < frame_len)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  frame->flag = buffer[0];
  frame->payload = buffer + GOODIX5125_OUTER_HEADER_SIZE;
  frame->payload_len = payload_len;
  frame->frame_len = frame_len;

  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_inner_encode (Goodix5125Command      command,
                               Goodix5125ChecksumMode checksum_mode,
                               const guint8          *payload,
                               gsize                  payload_len,
                               guint8                *output,
                               gsize                  output_size,
                               gsize                 *written)
{
  gsize frame_len;

  if (output == NULL || written == NULL || (payload == NULL && payload_len != 0))
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  *written = 0;

  if (!goodix5125_proto_command_is_supported (command))
    return GOODIX5125_PROTO_INVALID_COMMAND;

  if (checksum_mode != GOODIX5125_CHECKSUM_NORMAL &&
      checksum_mode != GOODIX5125_CHECKSUM_TRAILER_88)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  if (checksum_mode == GOODIX5125_CHECKSUM_TRAILER_88 &&
      command != GOODIX5125_COMMAND_NOP)
    return GOODIX5125_PROTO_INVALID_COMMAND;

  if (payload_len > G_MAXUINT16 - GOODIX5125_INNER_TRAILER_SIZE)
    return GOODIX5125_PROTO_INVALID_LENGTH;

  frame_len = GOODIX5125_INNER_HEADER_SIZE + payload_len +
              GOODIX5125_INNER_TRAILER_SIZE;
  if (output_size < frame_len)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  output[0] = command;
  write_uint16_le (output + 1,
                   (guint16) (payload_len + GOODIX5125_INNER_TRAILER_SIZE));
  if (payload_len != 0)
    memcpy (output + GOODIX5125_INNER_HEADER_SIZE, payload, payload_len);

  if (checksum_mode == GOODIX5125_CHECKSUM_NORMAL)
    output[frame_len - 1] = goodix5125_proto_inner_checksum (output, frame_len - 1);
  else
    output[frame_len - 1] = GOODIX5125_NOP_TRAILER;

  *written = frame_len;
  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_inner_decode (const guint8          *buffer,
                               gsize                  buffer_len,
                               Goodix5125ChecksumMode checksum_mode,
                               Goodix5125InnerFrame  *frame)
{
  guint16 body_len;
  gsize frame_len;

  if (buffer == NULL || frame == NULL)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  if (checksum_mode != GOODIX5125_CHECKSUM_NORMAL &&
      checksum_mode != GOODIX5125_CHECKSUM_TRAILER_88)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  if (buffer_len < GOODIX5125_INNER_HEADER_SIZE + GOODIX5125_INNER_TRAILER_SIZE)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  if (!goodix5125_proto_command_is_supported (buffer[0]))
    return GOODIX5125_PROTO_INVALID_COMMAND;

  body_len = read_uint16_le (buffer + 1);
  if (body_len < GOODIX5125_INNER_TRAILER_SIZE)
    return GOODIX5125_PROTO_INVALID_LENGTH;

  frame_len = GOODIX5125_INNER_HEADER_SIZE + body_len;
  if (buffer_len < frame_len)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  if (checksum_mode == GOODIX5125_CHECKSUM_NORMAL)
    {
      if (goodix5125_proto_inner_checksum (buffer, frame_len - 1) != buffer[frame_len - 1])
        return GOODIX5125_PROTO_INVALID_CHECKSUM;
    }
  else if (buffer[frame_len - 1] != GOODIX5125_NOP_TRAILER)
    {
      return GOODIX5125_PROTO_INVALID_CHECKSUM;
    }

  frame->command = buffer[0];
  frame->payload = buffer + GOODIX5125_INNER_HEADER_SIZE;
  frame->payload_len = body_len - GOODIX5125_INNER_TRAILER_SIZE;
  frame->frame_len = frame_len;

  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_classify_rx (const guint8          *buffer,
                              gsize                  buffer_len,
                              Goodix5125Command      command,
                              Goodix5125OuterFlag    reply_flag,
                              Goodix5125ChecksumMode reply_checksum_mode,
                              Goodix5125RxKind      *kind,
                              const guint8         **payload,
                              gsize                 *payload_len)
{
  Goodix5125OuterFrame outer;
  Goodix5125InnerFrame inner;
  Goodix5125ProtoResult result;
  gboolean has_data;

  if (kind == NULL || payload == NULL || payload_len == NULL)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;
  *kind = GOODIX5125_RX_SKIP;
  *payload = NULL;
  *payload_len = 0;

  result = goodix5125_proto_outer_decode (buffer, buffer_len, &outer);
  if (result != GOODIX5125_PROTO_OK)
    return result;

  if (outer.flag != GOODIX5125_OUTER_FLAG_MESSAGE)
    {
      if (outer.flag == reply_flag)
        {
          *kind = GOODIX5125_RX_REPLY;
          *payload = outer.payload;
          *payload_len = outer.payload_len;
        }
      return GOODIX5125_PROTO_OK;
    }

  if (outer.payload_len < 1)
    return GOODIX5125_PROTO_BUFFER_TOO_SMALL;

  if (outer.payload[0] == GOODIX5125_COMMAND_ACK)
    {
      result = goodix5125_proto_inner_decode (outer.payload, outer.payload_len,
                                              GOODIX5125_CHECKSUM_NORMAL, &inner);
      if (result != GOODIX5125_PROTO_OK)
        return result;
      if (inner.payload_len != GOODIX5125_ACK_PAYLOAD_SIZE ||
          inner.payload[0] != command)
        return GOODIX5125_PROTO_OK;   /* stale ACK of another command */
      result = goodix5125_proto_ack_validate (&inner, command, &has_data);
      if (result == GOODIX5125_PROTO_OK)
        *kind = GOODIX5125_RX_ACK;
      return result;
    }

  if (reply_flag != GOODIX5125_OUTER_FLAG_MESSAGE ||
      outer.payload[0] != command)
    return GOODIX5125_PROTO_OK;       /* stale reply of another command */

  result = goodix5125_proto_inner_decode (outer.payload, outer.payload_len,
                                          reply_checksum_mode, &inner);
  if (result != GOODIX5125_PROTO_OK)
    return result;
  *kind = GOODIX5125_RX_REPLY;
  *payload = inner.payload;
  *payload_len = inner.payload_len;
  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_ack_validate (const Goodix5125InnerFrame *frame,
                               Goodix5125Command           expected_command,
                               gboolean                   *has_data)
{
  guint8 flags;

  if (frame == NULL || has_data == NULL)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  *has_data = FALSE;

  if (!goodix5125_proto_command_is_supported (expected_command))
    return GOODIX5125_PROTO_INVALID_COMMAND;

  if (frame->command != GOODIX5125_COMMAND_ACK ||
      frame->payload == NULL ||
      frame->payload_len != GOODIX5125_ACK_PAYLOAD_SIZE ||
      frame->payload[0] != expected_command)
    return GOODIX5125_PROTO_INVALID_ACK;

  flags = frame->payload[1];
  if ((flags & 0x01) == 0)
    return GOODIX5125_PROTO_INVALID_ACK;

  *has_data = (flags & 0x02) != 0;
  return GOODIX5125_PROTO_OK;
}

guint8
goodix5125_proto_otp_crc8 (const guint8 *data,
                           gsize         data_len)
{
  guint8 crc = 0;
  gsize i;
  guint bit;

  g_return_val_if_fail (data != NULL || data_len == 0, 0);

  for (i = 0; i < data_len; i++)
    {
      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        crc = (crc & 0x80) != 0 ? (crc << 1) ^ 0x07 : crc << 1;
    }

  return ~crc;
}

Goodix5125ProtoResult
goodix5125_proto_config_checksum_calculate (const guint8 *config,
                                            gsize         config_len,
                                            guint16      *checksum)
{
  guint32 sum = GOODIX5125_CONFIG_CHECKSUM_SEED;
  gsize offset;

  if (config == NULL || checksum == NULL)
    return GOODIX5125_PROTO_INVALID_ARGUMENT;

  if (config_len != GOODIX5125_CONFIG_SIZE)
    return GOODIX5125_PROTO_INVALID_LENGTH;

  for (offset = 0; offset < GOODIX5125_CONFIG_CHECKSUM_OFFSET; offset += 2)
    sum += read_uint16_le (config + offset);

  *checksum = (guint16) - sum;
  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_config_checksum_write (guint8 *config,
                                        gsize   config_len)
{
  Goodix5125ProtoResult result;
  guint16 checksum;

  result = goodix5125_proto_config_checksum_calculate (config, config_len, &checksum);
  if (result != GOODIX5125_PROTO_OK)
    return result;

  write_uint16_le (config + GOODIX5125_CONFIG_CHECKSUM_OFFSET, checksum);
  return GOODIX5125_PROTO_OK;
}

Goodix5125ProtoResult
goodix5125_proto_config_checksum_verify (const guint8 *config,
                                         gsize         config_len)
{
  Goodix5125ProtoResult result;
  guint16 checksum;

  result = goodix5125_proto_config_checksum_calculate (config, config_len, &checksum);
  if (result != GOODIX5125_PROTO_OK)
    return result;

  if (read_uint16_le (config + GOODIX5125_CONFIG_CHECKSUM_OFFSET) != checksum)
    return GOODIX5125_PROTO_INVALID_CHECKSUM;

  return GOODIX5125_PROTO_OK;
}
