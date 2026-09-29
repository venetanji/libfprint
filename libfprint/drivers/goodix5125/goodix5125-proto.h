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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GOODIX5125_OUTER_HEADER_SIZE 4
#define GOODIX5125_INNER_HEADER_SIZE 3
#define GOODIX5125_INNER_TRAILER_SIZE 1
#define GOODIX5125_ACK_PAYLOAD_SIZE 2
#define GOODIX5125_CONFIG_SIZE 224
#define GOODIX5125_CONFIG_CHECKSUM_OFFSET 222

typedef enum {
  GOODIX5125_OUTER_FLAG_MESSAGE = 0xa0,
  GOODIX5125_OUTER_FLAG_TLS = 0xb0,
  GOODIX5125_OUTER_FLAG_TLS_DATA = 0xb2,
} Goodix5125OuterFlag;

/* Only commands used by the capture protocol are intentionally exposed. */
typedef enum {
  GOODIX5125_COMMAND_NOP = 0x00,
  GOODIX5125_COMMAND_GET_IMAGE = 0x20,
  GOODIX5125_COMMAND_FDT_DOWN = 0x32,
  GOODIX5125_COMMAND_FDT_UP = 0x34,
  GOODIX5125_COMMAND_FDT_MODE = 0x36,
  GOODIX5125_COMMAND_NAVIGATION = 0x50,
  GOODIX5125_COMMAND_MCU_SLEEP = 0x60,
  GOODIX5125_COMMAND_MCU_IDLE = 0x70,
  GOODIX5125_COMMAND_WRITE_SENSOR_REGISTER = 0x80,
  GOODIX5125_COMMAND_READ_SENSOR_REGISTER = 0x82,
  GOODIX5125_COMMAND_UPLOAD_CONFIG = 0x90,
  GOODIX5125_COMMAND_SLEEP = 0x92,
  GOODIX5125_COMMAND_SET_POWERDOWN_FREQUENCY = 0x94,
  GOODIX5125_COMMAND_ENABLE = 0x96,
  GOODIX5125_COMMAND_RESET = 0xa2,
  GOODIX5125_COMMAND_READ_OTP = 0xa6,
  GOODIX5125_COMMAND_QUERY_STATE = 0xae,
  GOODIX5125_COMMAND_ACK = 0xb0,
  GOODIX5125_COMMAND_REQUEST_TLS = 0xd0,
  GOODIX5125_COMMAND_TLS_ESTABLISHED = 0xd4,
  GOODIX5125_COMMAND_WRITE_PSK = 0xe0,
  GOODIX5125_COMMAND_READ_PSK_HASH = 0xe4,
} Goodix5125Command;

typedef enum {
  GOODIX5125_CHECKSUM_NORMAL,
  /* Some MCU replies, notably NAV, use a fixed 0x88 trailer instead of the
   * additive checksum. NOP requests use the same wire trailer. */
  GOODIX5125_CHECKSUM_TRAILER_88,
} Goodix5125ChecksumMode;

typedef enum {
  GOODIX5125_PROTO_OK,
  GOODIX5125_PROTO_INVALID_ARGUMENT,
  GOODIX5125_PROTO_BUFFER_TOO_SMALL,
  GOODIX5125_PROTO_INVALID_FLAG,
  GOODIX5125_PROTO_INVALID_COMMAND,
  GOODIX5125_PROTO_INVALID_LENGTH,
  GOODIX5125_PROTO_INVALID_CHECKSUM,
  GOODIX5125_PROTO_INVALID_ACK,
} Goodix5125ProtoResult;

typedef struct
{
  Goodix5125OuterFlag flag;
  const guint8       *payload;
  guint16             payload_len;
  gsize               frame_len;
} Goodix5125OuterFrame;

typedef struct
{
  Goodix5125Command command;
  const guint8     *payload;
  guint16           payload_len;
  gsize             frame_len;
} Goodix5125InnerFrame;

gboolean goodix5125_proto_flag_is_supported (guint8 value);
gboolean goodix5125_proto_command_is_supported (guint8 value);

guint8 goodix5125_proto_outer_checksum (const guint8 header[3]);
guint8 goodix5125_proto_inner_checksum (const guint8 *data,
                                        gsize         data_len);

Goodix5125ProtoResult goodix5125_proto_outer_encode (Goodix5125OuterFlag flag,
                                                     const guint8       *payload,
                                                     gsize               payload_len,
                                                     guint8             *output,
                                                     gsize               output_size,
                                                     gsize              *written);
Goodix5125ProtoResult goodix5125_proto_outer_decode (const guint8         *buffer,
                                                     gsize                 buffer_len,
                                                     Goodix5125OuterFrame *frame);

Goodix5125ProtoResult goodix5125_proto_inner_encode (Goodix5125Command      command,
                                                     Goodix5125ChecksumMode checksum_mode,
                                                     const guint8          *payload,
                                                     gsize                  payload_len,
                                                     guint8                *output,
                                                     gsize                  output_size,
                                                     gsize                 *written);
Goodix5125ProtoResult goodix5125_proto_inner_decode (const guint8          *buffer,
                                                     gsize                  buffer_len,
                                                     Goodix5125ChecksumMode checksum_mode,
                                                     Goodix5125InnerFrame  *frame);

Goodix5125ProtoResult goodix5125_proto_ack_validate (const Goodix5125InnerFrame *frame,
                                                     Goodix5125Command           expected_command,
                                                     gboolean                   *has_data);

/* What an IN transfer means for a command in flight.  The 27c6:5125 MCU
 * often drops the ACK when the reply follows immediately, and a reply of an
 * earlier command (e.g. a late FDT-down) can still be queued, so a command
 * reads until its own reply (or ACK) and skips everything else. */
typedef enum {
  GOODIX5125_RX_SKIP,   /* unrelated or stale message */
  GOODIX5125_RX_ACK,    /* successful ACK of this command */
  GOODIX5125_RX_REPLY,  /* reply of this command; payload set */
} Goodix5125RxKind;

Goodix5125ProtoResult goodix5125_proto_classify_rx (const guint8          *buffer,
                                                    gsize                  buffer_len,
                                                    Goodix5125Command      command,
                                                    Goodix5125OuterFlag    reply_flag,
                                                    Goodix5125ChecksumMode reply_checksum_mode,
                                                    Goodix5125RxKind      *kind,
                                                    const guint8         **payload,
                                                    gsize                 *payload_len);

guint8 goodix5125_proto_otp_crc8 (const guint8 *data,
                                  gsize         data_len);

Goodix5125ProtoResult goodix5125_proto_config_checksum_calculate (const guint8 *config,
                                                                  gsize         config_len,
                                                                  guint16      *checksum);
Goodix5125ProtoResult goodix5125_proto_config_checksum_write (guint8 *config,
                                                              gsize   config_len);
Goodix5125ProtoResult goodix5125_proto_config_checksum_verify (const guint8 *config,
                                                               gsize         config_len);

G_END_DECLS
