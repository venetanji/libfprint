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
 * Protocol layer implementation (see oms_proto.h for details).
 */

#include "glib.h"
#include "oms_proto.h"

/*
 * The checksum is the big endian, 16 bit sum of all bytes between the
 * fixed prefix and the checksum itself, i.e. of:
 *   type(1) + len(2) + data
 */
static guint16
oms_proto_checksum (guint8        type,
                    const guint8 *data,
                    guint16       data_len)
{
  guint16 sum = type + ((data_len + 2) >> 8) + ((data_len + 2) & 0xFF);

  for (guint i = 0; i < data_len; i++)
    sum += data[i];

  return sum;
}

gboolean
oms_proto_build_command (const guint8 *data,
                         guint8        data_len,
                         guint8        out[OMS_CMD_FRAME_LEN])
{
  static const guint8 prefix[OMS_FRAME_PREFIX_LEN] = OMS_FRAME_PREFIX;
  guint16 checksum;

  g_return_val_if_fail (data || data_len == 0, FALSE);
  g_return_val_if_fail (out, FALSE);

  /* data_len (command byte + params) must leave room for the two
   * checksum bytes inside the fixed frame. */
  if (data_len + 2u > OMS_CMD_FRAME_LEN - OMS_FRAME_PREFIX_LEN - 3u)
    return FALSE;

  memset (out, 0, OMS_CMD_FRAME_LEN);

  memcpy (out, prefix, OMS_FRAME_PREFIX_LEN);

  out[OMS_FRAME_PREFIX_LEN] = OMS_FRAME_TYPE_CMD;
  out[OMS_FRAME_PREFIX_LEN + 1] = (data_len + 2) >> 8;
  out[OMS_FRAME_PREFIX_LEN + 2] = (data_len + 2) & 0xFF;

  if (data_len)
    memcpy (out + OMS_FRAME_PREFIX_LEN + 3, data, data_len);

  checksum = oms_proto_checksum (OMS_FRAME_TYPE_CMD, data, data_len);
  out[OMS_FRAME_PREFIX_LEN + 3 + data_len] = checksum >> 8;
  out[OMS_FRAME_PREFIX_LEN + 3 + data_len + 1] = checksum & 0xFF;

  return TRUE;
}

static gboolean
oms_proto_checksum_valid (OmsProtoFrame *frame)
{
  guint16 sum = frame->type +
                (frame->data_len + 2) / 0x100 +
                (frame->data_len + 2) % 0x100;
  guint16 checksum = 0;

  for (guint i = 0; i < frame->data_len; i++)
    sum += frame->data[i];

  /* The checksum is stored right after the data. */
  checksum = (frame->data[frame->data_len] << 8) |
             frame->data[frame->data_len + 1];

  return sum == checksum;
}

gboolean
oms_proto_parse_frame (const guint8  *buf,
                       gsize          buf_len,
                       OmsProtoFrame *frame)
{
  guint16 data_len;

  g_return_val_if_fail (buf, FALSE);
  g_return_val_if_fail (frame, FALSE);

  /* Need prefix + type + len + at least 2 checksum bytes. */
  if (buf_len < OMS_FRAME_PREFIX_LEN + 5u)
    return FALSE;

  /* Fixed prefix. */
  if (memcmp (buf, (guint8[]) OMS_FRAME_PREFIX, OMS_FRAME_PREFIX_LEN) != 0)
    return FALSE;

  if (buf[OMS_FRAME_PREFIX_LEN] != OMS_FRAME_TYPE_RESP)
    return FALSE;

  data_len = (buf[OMS_FRAME_PREFIX_LEN + 1] << 8) |
             buf[OMS_FRAME_PREFIX_LEN + 2];

  /* data_len includes the two checksum bytes; we must have enough room. */
  if (data_len < 2u)
    return FALSE;
  if (OMS_FRAME_PREFIX_LEN + 3u + data_len > buf_len)
    return FALSE;

  frame->type = OMS_FRAME_TYPE_RESP;
  frame->data = buf + OMS_FRAME_PREFIX_LEN + 3;
  frame->data_len = data_len - 2u;

  /* Validate the checksum over type/len/data. */
  return oms_proto_checksum_valid (frame);
}

gboolean
oms_index_table_get (const OmsIndexTable *table,
                     guint16              id)
{
  g_return_val_if_fail (table, FALSE);

  if (id >= OMS_INDEX_TABLE_BITS)
    return FALSE;

  return (table->bytes[id / 8] >> (id % 8)) & 0x01;
}

void
oms_index_table_set (OmsIndexTable *table,
                     guint16        id,
                     gboolean       enrolled)
{
  g_return_if_fail (table);
  g_return_if_fail (id < OMS_INDEX_TABLE_BITS);

  if (enrolled)
    table->bytes[id / 8] |=  (0x01 << (id % 8));
  else
    table->bytes[id / 8] &= ~(0x01 << (id % 8));
}

guint
oms_index_table_count (const OmsIndexTable *table)
{
  guint count = 0;

  g_return_val_if_fail (table, 0);

  for (guint id = 0; id < OMS_INDEX_TABLE_BITS; id++)
    count += oms_index_table_get (table, id) ? 1 : 0;

  return count;
}

gboolean
oms_index_table_find_free (const OmsIndexTable *table,
                           guint16             *free_id)
{
  g_return_val_if_fail (table, FALSE);
  g_return_val_if_fail (free_id, FALSE);

  for (guint id = 0; id < OMS_INDEX_TABLE_BITS; id++)
    {
      if (!oms_index_table_get (table, id))
        {
          *free_id = id;
          return TRUE;
        }
    }

  return FALSE;
}
