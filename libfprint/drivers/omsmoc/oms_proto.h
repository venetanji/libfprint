/*
 * OMS Microelectronics Match-On-Chip fingerprint driver for libfprint
 * Copyright (C) 2026 Liu Sen
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

/*
 * Protocol layer: builds/validates the "command/response" frames that are
 * exchanged with the fingerprint module and provides helpers to manipulate
 * the module index table (a bitmap of the enrolled fingerprint IDs).
 *
 * Layering note
 * -------------
 * This file is the *pure protocol* layer.  It knows nothing about USB
 * transfers or about the libfprint state machines: it only serializes and
 * deserializes data blobs.  The USB transport ("logical read"/"logical
 * write") and the higher level state machines live in omsmoc.c.
 *
 * Frame layout (identical for commands and responses):
 *
 *   byte 0..5    0xEF 0x01 0xFF 0xFF 0xFF 0xFF   (fixed prefix)
 *   byte 6       type (0x01 = command, 0x07 = response)
 *   byte 7..8    len, big endian = size of (data + 2 byte checksum)
 *   byte 9..     data
 *   ...          checksum, big endian sum of (type + len + data) bytes
 *   ...          zero padding up to the physical frame length
 *
 * Commands are always padded to OMS_CMD_FRAME_LEN (64) bytes before being
 * handed over to the transport layer.
 */

#pragma once

#include <glib.h>

/* ---------------------------------------------------------------- *
* Fixed protocol constants                                         *
* ---------------------------------------------------------------- */

/* Total physical length of every command sent to the module. */
#define OMS_CMD_FRAME_LEN 64u

/* Fixed prefix of every command/response frame. */
#define OMS_FRAME_PREFIX_LEN 6u
#define OMS_FRAME_PREFIX { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF }

/* Frame "type" bytes. */
#define OMS_FRAME_TYPE_CMD 0x01
#define OMS_FRAME_TYPE_RESP 0x07

/* Index table (bitmap) size in bytes: one bit per fingerprint ID. */
#define OMS_INDEX_TABLE_BYTES 32u
#define OMS_INDEX_TABLE_BITS (OMS_INDEX_TABLE_BYTES * 8)

/* ---------------------------------------------------------------- *
* Instruction (sub-)command codes used inside the "data" part       *
* ---------------------------------------------------------------- */

#define OMS_CMD_DELETE_CHAR 0x0C             /* delete one fingerprint        */
#define OMS_CMD_READ_INDEX_TABLE 0x1F        /* read enrollment bitmap        */
#define OMS_CMD_GET_VALID_TEMPLATE_NUM 0x1D  /* number of stored fingerprints */
#define OMS_CMD_CANCEL 0x30                  /* cancel idenrity or enroll     */
#define OMS_CMD_AUTO_ENROLL 0x31             /* enroll a new fingerprint      */
#define OMS_CMD_AUTO_IDENTIFY 0x32           /* search a fingerprint          */

/* AutoEnroll number of image captures the module will perform. */
#define OMS_AUTO_ENROLL_ROUNDS 6u

/* AutoIdentify / AutoEnroll replies use this first data byte as a
 * confirmation code. */
#define OMS_ACK_OK 0x00
#define OMS_ACK_NO_MATCH 0x09                /* no fingerprint matched */

/* ---------------------------------------------------------------- *
* Result of parsing one frame                                       *
* ---------------------------------------------------------------- */

typedef struct
{
  guint8        type;                    /* OMS_FRAME_TYPE_*              */
  const guint8 *data;                    /* points into the input buffer  */
  guint16       data_len;                /* length of @data               */
} OmsProtoFrame;

/* ---------------------------------------------------------------- *
* Module index table (bitmap of enrolled fingerprint IDs)           *
* ---------------------------------------------------------------- */

typedef struct
{
  guint8 bytes[OMS_INDEX_TABLE_BYTES];
} OmsIndexTable;

/* ---------------------------------------------------------------- *
* Prototypes                                                        *
* ---------------------------------------------------------------- */

/* Build a 64 byte command frame from its complete data part
 * (@data already includes the instruction byte).  Returns FALSE if the
 * data does not fit into the fixed 64 byte frame. */
gboolean oms_proto_build_command (const guint8 *data,
                                  guint8        data_len,
                                  guint8        out[OMS_CMD_FRAME_LEN]);

/* Validate a raw response and fill in @frame.  @buf/@buf_len is the data
 * returned by one logical read.  On success the function guarantees that
 * frame->type == OMS_FRAME_TYPE_RESP. */
gboolean oms_proto_parse_frame (const guint8  *buf,
                                gsize          buf_len,
                                OmsProtoFrame *frame);

/* OmsIndexTable helpers (module fingerprint ID == bit position). */
gboolean oms_index_table_get (const OmsIndexTable *table,
                              guint16              id);
void     oms_index_table_set (OmsIndexTable *table,
                              guint16        id,
                              gboolean       enrolled);
guint    oms_index_table_count (const OmsIndexTable *table);
/* Look up the lowest free ID in the table.  Returns FALSE when full. */
gboolean oms_index_table_find_free (const OmsIndexTable *table,
                                    guint16             *free_id);
