/*
 * Capture program infrastructure for Validity/Synaptics VCSFW sensors
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
#include "validity_capture.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

/* OpenSSL for SHA256 (clean slate) */
#include <openssl/evp.h>

/* ================================================================
 * Chunk parsing
 * ================================================================ */

ValidityCaptureChunk *
validity_capture_split_chunks (const guint8 *data,
                               gsize         data_len,
                               gsize        *n_chunks)
{
  GArray *arr;
  FpiByteReader reader;

  g_return_val_if_fail (data != NULL || data_len == 0, NULL);
  g_return_val_if_fail (n_chunks != NULL, NULL);

  arr = g_array_new (FALSE, TRUE, sizeof (ValidityCaptureChunk));

  fpi_byte_reader_init (&reader, data, data_len);

  while (fpi_byte_reader_get_remaining (&reader) >= 4)
    {
      ValidityCaptureChunk chunk;
      const guint8 *chunk_data_ptr;

      if (!fpi_byte_reader_get_uint16_le (&reader, &chunk.type) ||
          !fpi_byte_reader_get_uint16_le (&reader, &chunk.size))
        break;

      if (!fpi_byte_reader_get_data (&reader, chunk.size, &chunk_data_ptr))
        {
          /* Truncated chunk — free what we have and fail */
          for (gsize i = 0; i < arr->len; i++)
            g_free (g_array_index (arr, ValidityCaptureChunk, i).data);
          g_array_free (arr, TRUE);
          *n_chunks = 0;
          return NULL;
        }

      chunk.data = g_memdup2 (chunk_data_ptr, chunk.size);

      g_array_append_val (arr, chunk);
    }

  *n_chunks = arr->len;
  return (ValidityCaptureChunk *) g_array_free (arr, FALSE);
}

guint8 *
validity_capture_merge_chunks (const ValidityCaptureChunk *chunks,
                               gsize                       n_chunks,
                               gsize                      *out_len)
{
  gsize total = 0;
  FpiByteWriter writer;

  g_return_val_if_fail (out_len != NULL, NULL);

  /* Calculate total size */
  for (gsize i = 0; i < n_chunks; i++)
    total += 4 + chunks[i].size;

  fpi_byte_writer_init_with_size (&writer, total, FALSE);

  for (gsize i = 0; i < n_chunks; i++)
    {
      fpi_byte_writer_put_uint16_le (&writer, chunks[i].type);
      fpi_byte_writer_put_uint16_le (&writer, chunks[i].size);
      if (chunks[i].size > 0 && chunks[i].data)
        fpi_byte_writer_put_data (&writer, chunks[i].data, chunks[i].size);
    }

  *out_len = fpi_byte_writer_get_pos (&writer);
  return fpi_byte_writer_reset_and_get_data (&writer);
}

void
validity_capture_chunks_free (ValidityCaptureChunk *chunks,
                              gsize                 n_chunks)
{
  if (chunks == NULL)
    return;

  for (gsize i = 0; i < n_chunks; i++)
    g_free (chunks[i].data);

  g_free (chunks);
}

/* ================================================================
 * Timeslot instruction decoder
 *
 * Reference: python-validity timeslot.py decode_insn()
 * ================================================================ */

gboolean
validity_capture_decode_insn (const guint8 *data,
                              gsize         data_len,
                              guint8       *opcode,
                              guint8       *insn_len,
                              guint32       operands[3],
                              guint8       *n_operands)
{
  g_return_val_if_fail (data != NULL && data_len > 0, FALSE);
  g_return_val_if_fail (opcode != NULL && insn_len != NULL, FALSE);
  g_return_val_if_fail (n_operands != NULL, FALSE);

  *n_operands = 0;
  guint8 b0 = data[0];

  /* Single-byte instructions: 0x00-0x04 */
  if (b0 <= 4)
    {
      *opcode = b0;
      *insn_len = 1;
      return TRUE;
    }

  /* Two-byte instructions with one operand */
  if (b0 == 5)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_MACRO;
      *insn_len = 2;
      operands[0] = data[1];
      *n_operands = 1;
      return TRUE;
    }

  if (b0 == 6)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_ENABLE_RX;
      *insn_len = 2;
      operands[0] = data[1];
      *n_operands = 1;
      return TRUE;
    }

  if (b0 == 7)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_IDLE_RX;
      *insn_len = 2;
      operands[0] = (data[1] == 0) ? 0x100 : data[1];
      *n_operands = 1;
      return TRUE;
    }

  /* Enable SO: 0x08-0x09 */
  if ((b0 & 0xfe) == 0x08)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_ENABLE_SO;
      *insn_len = 2;
      operands[0] = ((guint32) (b0 & 1) << 8) | data[1];
      *n_operands = 1;
      return TRUE;
    }

  /* Disable SO: 0x0a-0x0b */
  if ((b0 & 0xfe) == 0x0a)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_DISABLE_SO;
      *insn_len = 2;
      operands[0] = ((guint32) (b0 & 1) << 8) | data[1];
      *n_operands = 1;
      return TRUE;
    }

  /* Interrupt: 0x0c-0x0f */
  if ((b0 & 0xfc) == 0x0c)
    {
      *opcode = TST_OP_INTERRUPT;
      *insn_len = 1;
      operands[0] = b0 & 3;
      *n_operands = 1;
      return TRUE;
    }

  /* Call: 0x10-0x17 (3 bytes) */
  if ((b0 & 0xf8) == 0x10)
    {
      if (data_len < 3)
        return FALSE;
      *opcode = TST_OP_CALL;
      *insn_len = 3;
      operands[0] = b0 & 7;                               /* rx_inc */
      operands[1] = (guint32) data[1] << 2;               /* address */
      operands[2] = (data[2] == 0) ? 0x100 : data[2];     /* repeat */
      *n_operands = 3;
      return TRUE;
    }

  /* Features: 0x20-0x3f (1 byte) */
  if ((b0 & 0xe0) == 0x20)
    {
      *opcode = TST_OP_FEATURES;
      *insn_len = 1;
      operands[0] = b0 & 0x1f;
      *n_operands = 1;
      return TRUE;
    }

  /* Register Write: 0x40-0x7f (3 bytes) */
  if ((b0 & 0xc0) == 0x40)
    {
      if (data_len < 3)
        return FALSE;
      *opcode = TST_OP_REG_WRITE;
      *insn_len = 3;
      operands[0] = (guint32) (b0 & 0x3f) * 4 + 0x80002000;  /* register address */
      operands[1] = (guint32) data[1] | ((guint32) data[2] << 8);  /* value */
      *n_operands = 2;
      return TRUE;
    }

  /* Sample: 0x80-0xbf (1 byte) */
  if ((b0 & 0xc0) == 0x80)
    {
      *opcode = TST_OP_SAMPLE;
      *insn_len = 1;
      operands[0] = (b0 & 0x38) >> 3;
      operands[1] = b0 & 7;
      *n_operands = 2;
      return TRUE;
    }

  /* Sample Repeat: 0xc0-0xff (2 bytes) */
  if ((b0 & 0xc0) == 0xc0)
    {
      if (data_len < 2)
        return FALSE;
      *opcode = TST_OP_SAMPLE_REPEAT;
      *insn_len = 2;
      operands[0] = (b0 & 0x38) >> 3;
      operands[1] = b0 & 7;
      operands[2] = (data[1] == 0) ? 0x100 : data[1];
      *n_operands = 3;
      return TRUE;
    }

  return FALSE;
}

gssize
validity_capture_find_nth_insn (const guint8 *data,
                                gsize         data_len,
                                guint8        target_opcode,
                                guint         n)
{
  gsize pc = 0;

  while (pc < data_len)
    {
      guint8 opcode, len, n_ops;
      guint32 operands[3];

      if (!validity_capture_decode_insn (data + pc, data_len - pc,
                                         &opcode, &len, operands, &n_ops))
        break;

      if (opcode == target_opcode)
        {
          n--;
          if (n == 0)
            return (gssize) pc;
        }

      pc += len;
    }

  return -1;
}

gssize
validity_capture_find_nth_regwrite (const guint8 *data,
                                    gsize         data_len,
                                    guint32       reg_addr,
                                    guint         n)
{
  gsize pc = 0;

  while (pc < data_len)
    {
      guint8 opcode, len, n_ops;
      guint32 operands[3];

      if (!validity_capture_decode_insn (data + pc, data_len - pc,
                                         &opcode, &len, operands, &n_ops))
        break;

      if (opcode == TST_OP_REG_WRITE && n_ops >= 2 && operands[0] == reg_addr)
        {
          n--;
          if (n == 0)
            return (gssize) pc;
        }

      pc += len;
    }

  return -1;
}

/* ================================================================
 * Timeslot table patching
 * ================================================================ */

gboolean
validity_capture_patch_timeslot_table (guint8  *data,
                                       gsize    data_len,
                                       gboolean inc_address,
                                       guint8   mult)
{
  gsize i = 0;

  g_return_val_if_fail (data != NULL, FALSE);

  while (i + 3 <= data_len)
    {
      /* Call instruction: 0x10-0x17 */
      if ((data[i] & 0xf8) == 0x10)
        {
          if (data[i + 2] > 1)
            {
              data[i + 2] *= mult;
              if (inc_address)
                data[i + 1] += 1;
            }
          i += 3;
          continue;
        }

      /* NOOP: single byte 0x00 */
      if (data[i] == 0x00)
        {
          i += 1;
          continue;
        }

      /* Idle Rx: two bytes, opcode 0x07 */
      if (data[i] == 0x07)
        {
          i += 2;
          continue;
        }

      /* Unknown instruction — stop patching */
      break;
    }

  return TRUE;
}

gboolean
validity_capture_patch_timeslot_again (guint8       *data,
                                       gsize         data_len,
                                       const guint8 *factory_calibration_values,
                                       gsize         factory_cal_len,
                                       guint16       key_calibration_line)
{
  gssize call_target = -1;
  gsize pc = 0;
  gssize match = -1;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (factory_calibration_values != NULL, FALSE);

  /* First pass: find the last Call instruction's destination address */
  while (pc < data_len)
    {
      guint8 opcode, len, n_ops;
      guint32 operands[3];

      if (!validity_capture_decode_insn (data + pc, data_len - pc,
                                         &opcode, &len, operands, &n_ops))
        break;

      /* End of Table, Return, End of Data — stop scanning */
      if (opcode == TST_OP_END_OF_TABLE ||
          opcode == TST_OP_RETURN ||
          opcode == TST_OP_END_OF_DATA)
        break;

      /* Call — record its destination address */
      if (opcode == TST_OP_CALL && n_ops >= 2)
        call_target = (gssize) operands[1];

      pc += len;
    }

  if (call_target < 0 || (gsize) call_target >= data_len)
    return FALSE;

  /* Second pass: from the Call target, find the last Register Write to 0x8000203C */
  pc = (gsize) call_target;
  while (pc < data_len)
    {
      guint8 opcode, len, n_ops;
      guint32 operands[3];

      if (!validity_capture_decode_insn (data + pc, data_len - pc,
                                         &opcode, &len, operands, &n_ops))
        break;

      if (opcode == TST_OP_END_OF_TABLE ||
          opcode == TST_OP_RETURN ||
          opcode == TST_OP_END_OF_DATA)
        break;

      /* Register Write to 0x8000203C */
      if (opcode == TST_OP_REG_WRITE && n_ops >= 2 && operands[0] == 0x8000203c)
        match = (gssize) pc;

      pc += len;
    }

  if (match < 0)
    return FALSE;

  /* Patch the value byte with factory calibration value at key_calibration_line.
   * The instruction is 3 bytes: [opcode_byte] [value_lo] [value_hi]
   * We patch value_lo (byte at match+1). */
  if (key_calibration_line < factory_cal_len)
    data[match + 1] = factory_calibration_values[key_calibration_line];

  return TRUE;
}

/* ================================================================
 * Helper functions for line update
 * ================================================================ */

/* Clip a signed value to [-128, 127] and return as unsigned byte */
static guint8
clip_signed (gint x)
{
  if (x < -128)
    x = -128;
  if (x > 127)
    x = 127;
  return (guint8) (x & 0xff);
}

/* Scale a byte value for calibration processing */
static guint8
scale_byte (guint8 x)
{
  gint val = (gint) x - 0x80;

  val = val * 10 / 0x22;
  return clip_signed (val);
}

/* Add two unsigned bytes as signed, clip result */
static guint8
add_signed_bytes (guint8 l, guint8 r)
{
  gint8 sl = (gint8) l;
  gint8 sr = (gint8) r;

  return clip_signed ((gint) sl + (gint) sr);
}

/* ================================================================
 * Bitpack — compress calibration values
 *
 * Reference: python-validity sensor.py bitpack()
 * ================================================================ */

guint8 *
validity_capture_bitpack (const guint8 *values,
                          gsize         values_len,
                          guint8       *out_v0,
                          guint8       *out_v1,
                          gsize        *out_len)
{
  guint8 min_val = 0xff, max_val = 0;
  guint8 useful_bits;
  guint max_delta;
  gsize total_bits;
  gsize total_bytes;
  guint8 *packed;

  g_return_val_if_fail (values != NULL && values_len > 0, NULL);
  g_return_val_if_fail (out_v0 != NULL && out_v1 != NULL && out_len != NULL, NULL);

  /* Find min and max */
  for (gsize i = 0; i < values_len; i++)
    {
      if (values[i] < min_val)
        min_val = values[i];
      if (values[i] > max_val)
        max_val = values[i];
    }

  max_delta = max_val - min_val;

  /* Count useful bits */
  useful_bits = 0;
  {
    guint tmp = max_delta;
    while (tmp > 0)
      {
        tmp >>= 1;
        useful_bits++;
      }
  }

  /* Handle edge case: all values identical */
  if (useful_bits == 0)
    {
      *out_v0 = 0;
      *out_v1 = min_val;
      *out_len = 0;
      return g_malloc0 (1);
    }

  /* Pack values in reverse order into a bit stream.
   * Each value is (value - min), stored in useful_bits bits.
   * Values are packed starting from the last value (reverse). */
  total_bits = (gsize) useful_bits * values_len;
  total_bytes = (total_bits + 7) / 8;
  packed = g_malloc0 (total_bytes);

  /* Build by accumulating shifted deltas from the last value to the first */
  {
    /* Use a big-endian bit accumulation approach:
     * Process values from last to first. For each value, shift the
     * accumulator left by useful_bits and OR in the delta.
     * This matches python: int(''.join(bin(v - min)[-u:] for v reversed), 2) */
    guint bit_pos = 0;
    for (gsize i = 0; i < values_len; i++)
      {
        guint delta = values[i] - min_val;
        /* Write 'useful_bits' bits at bit_pos (little-endian byte order) */
        for (guint b = 0; b < useful_bits; b++)
          {
            if (delta & (1u << b))
              packed[bit_pos / 8] |= (1u << (bit_pos % 8));
            bit_pos++;
          }
      }
  }

  *out_v0 = useful_bits;
  *out_v1 = min_val;
  *out_len = total_bytes;
  return packed;
}

/* ================================================================
 * get_key_line — extract the key calibration line from calib_data
 *
 * Reference: python-validity sensor.py get_key_line()
 * ================================================================ */

static guint8 *
get_key_line (const guint8 *calib_data,
              gsize         calib_data_len,
              guint16       lines_per_calibration_data,
              guint16       key_calibration_line,
              guint16       line_width)
{
  guint8 *key_line = g_malloc0 (line_width);

  if (calib_data != NULL && calib_data_len > 0 && lines_per_calibration_data > 0)
    {
      gsize bytes_per_cal_line = calib_data_len / lines_per_calibration_data;
      gsize key_offset = 8 + bytes_per_cal_line * key_calibration_line;

      if (key_offset + line_width <= calib_data_len)
        {
          memcpy (key_line, calib_data + key_offset, line_width);
          /* Replace value 5 with 4 (python: [i-1 if i == 5 else i for i in key_line]) */
          for (guint16 i = 0; i < line_width; i++)
            if (key_line[i] == 5)
              key_line[i] = 4;
        }
    }

  return key_line;
}

/* ================================================================
 * Line Update Type 1
 *
 * Modifies the chunk list for type-1 devices (includes 0xb5).
 * Adds Reply Config, Finger Detect/Image Reconstruction,
 * Interleave, Line Update, and Line Update Transform chunks.
 *
 * Reference: python-validity sensor.py line_update_type_1()
 * ================================================================ */

/* Internal line entry for building Line Update chunks */
typedef struct
{
  guint32 mask;
  guint32 flags;
  guint8 *data;
  gsize   data_len;
  guint8  v0;       /* bitpack: useful bits */
  guint8  v1;       /* bitpack: minimum */
  guint16 v2;       /* unused in type 1, always 0 */
} LineEntry;

/*
 * Build the line update chunk list for type 1 devices.
 * This is a complex function that:
 * 1. Patches the timeslot table in existing chunks
 * 2. Adds mode-specific chunks (Reply Config, Finger Detect, Image Recon)
 * 3. Adds Interleave chunk
 * 4. Builds Line Update and Line Update Transform from calibration data
 *
 * Returns a new array of chunks (caller frees with validity_capture_chunks_free).
 * n_chunks is updated with the new count.
 */
static ValidityCaptureChunk *
build_line_update_type1 (const ValidityCaptureState   *capture,
                         const ValiditySensorTypeInfo *type_info,
                         ValidityCaptureMode           mode,
                         ValidityCaptureChunk         *in_chunks,
                         gsize                         in_n_chunks,
                         gsize                        *out_n_chunks)
{
  GArray *chunks_arr;
  GArray *lines_arr;
  gsize cnt = 2;    /* line counter starts at 2 per python-validity */

  /* Save the patched TST (before key_line replacement) for instruction
   * searches later.  PY searches the original patched tst variable, not
   * the chunk data that has the key_line prepended. */
  g_autofree guint8 *patched_tst = NULL;
  gsize patched_tst_len = 0;

  /* Copy input chunks, patching timeslot table in-place */
  chunks_arr = g_array_new (FALSE, TRUE, sizeof (ValidityCaptureChunk));

  for (gsize i = 0; i < in_n_chunks; i++)
    {
      ValidityCaptureChunk c;
      c.type = in_chunks[i].type;
      c.size = in_chunks[i].size;
      c.data = g_memdup2 (in_chunks[i].data, in_chunks[i].size);

      /* Patch Timeslot Table 2D */
      if (c.type == CAPT_CHUNK_TIMESLOT_2D && c.data && c.size > 0)
        {
          validity_capture_patch_timeslot_table (c.data, c.size, TRUE,
                                                 type_info->repeat_multiplier);
          if (mode != VALIDITY_CAPTURE_CALIBRATE)
            {
              validity_capture_patch_timeslot_again (c.data, c.size,
                                                     capture->factory_calibration_values,
                                                     capture->factory_calibration_values_len,
                                                     capture->key_calibration_line);
            }

          /* Save the patched TST before key_line replacement.
           * Instruction searches must use this, not the key_line-modified data. */
          patched_tst = g_memdup2 (c.data, c.size);
          patched_tst_len = c.size;

          /* Prepend key line to the timeslot table.
           * In type 1: c[1] = get_key_line() + tst[line_width:] */
          {
            g_autofree guint8 *key_line = get_key_line (
              capture->calib_data, capture->calib_data_len,
              type_info->lines_per_calibration_data,
              capture->key_calibration_line,
              type_info->line_width);

            if (c.size > type_info->line_width)
              {
                gsize new_size = type_info->line_width + (c.size - type_info->line_width);
                guint8 *new_data = g_malloc (new_size);
                memcpy (new_data, key_line, type_info->line_width);
                memcpy (new_data + type_info->line_width,
                        c.data + type_info->line_width,
                        c.size - type_info->line_width);
                g_free (c.data);
                c.data = new_data;
                c.size = new_size;
              }
          }
        }

      g_array_append_val (chunks_arr, c);
    }

  /* --- Reply Configuration --- */
  {
    ValidityCaptureChunk rc = { .type = CAPT_CHUNK_REPLY_CONFIG, .size = 0, .data = NULL };
    g_array_append_val (chunks_arr, rc);
  }

  /* --- Mode-specific chunks (Finger Detect / WTF / Image Reconstruction) --- */
  if (mode == VALIDITY_CAPTURE_IDENTIFY)
    {
      /* Finger Detect (chunk 0x4e) — hardcoded for type 1 devices */
      static const guint8 wtf_data[] = {
        0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00,
        0x30, 0x00, 0x00, 0x00, 0x87, 0x00, 0x02, 0x00,
        0x67, 0x00, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00,
        0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
        0x88, 0x13, 0xb8, 0x0b, 0x01, 0x09, 0x10, 0x00,
      };
      ValidityCaptureChunk fd = {
        .type = CAPT_CHUNK_WTF,
        .size = sizeof (wtf_data),
        .data = g_memdup2 (wtf_data, sizeof (wtf_data)),
      };
      g_array_append_val (chunks_arr, fd);

      /* Image Reconstruction (for identify mode) */
      static const guint8 recon_identify[] = {
        0x02, 0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x70, 0x00, 0x70, 0x00, 0x4d, 0x01, 0x00, 0x00,
        0xa0, 0x00, 0x8c, 0x00, 0x3c, 0x32, 0x32, 0x1e,
        0x3c, 0x0a, 0x02, 0x02,
      };
      ValidityCaptureChunk ir = {
        .type = CAPT_CHUNK_IMAGE_RECON,
        .size = sizeof (recon_identify),
        .data = g_memdup2 (recon_identify, sizeof (recon_identify)),
      };
      g_array_append_val (chunks_arr, ir);
    }
  else if (mode == VALIDITY_CAPTURE_ENROLL)
    {
      /* Finger Detect for enroll — uses chunk 0x26 */
      static const guint8 fd_enroll[] = {
        0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00,
        0x30, 0x00, 0x00, 0x00, 0x87, 0x00, 0x02, 0x00,
        0x67, 0x00, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00,
        0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
        0x50, 0xc3, 0x60, 0xea, 0x01, 0x09, 0x10, 0x00,
      };
      ValidityCaptureChunk fd = {
        .type = CAPT_CHUNK_FINGER_DETECT,
        .size = sizeof (fd_enroll),
        .data = g_memdup2 (fd_enroll, sizeof (fd_enroll)),
      };
      g_array_append_val (chunks_arr, fd);

      /* Image Reconstruction (for enroll mode — one byte different) */
      static const guint8 recon_enroll[] = {
        0x02, 0x00, 0x18, 0x00, 0x23, 0x00, 0x00, 0x00,
        0x70, 0x00, 0x70, 0x00, 0x4d, 0x01, 0x00, 0x00,
        0xa0, 0x00, 0x8c, 0x00, 0x3c, 0x32, 0x32, 0x1e,
        0x3c, 0x0a, 0x02, 0x02,
      };
      ValidityCaptureChunk ir = {
        .type = CAPT_CHUNK_IMAGE_RECON,
        .size = sizeof (recon_enroll),
        .data = g_memdup2 (recon_enroll, sizeof (recon_enroll)),
      };
      g_array_append_val (chunks_arr, ir);
    }
  else if (mode == VALIDITY_CAPTURE_ENROLL_IDENTIFY)
    {
      /* Hybrid: IDENTIFY chunk (0x4e) for reliable completion + ENROLL
       * image reconstruction for proper enrollment data processing.
       * Works around sensors where chunk 0x26 triggers false finger
       * detection from ambient capacitance. */
      static const guint8 wtf_data[] = {
        0xfb, 0xb2, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00,
        0x30, 0x00, 0x00, 0x00, 0x87, 0x00, 0x02, 0x00,
        0x67, 0x00, 0x0a, 0x00, 0x01, 0x80, 0x00, 0x00,
        0x0a, 0x02, 0x00, 0x00, 0x0b, 0x19, 0x00, 0x00,
        0x88, 0x13, 0xb8, 0x0b, 0x01, 0x09, 0x10, 0x00,
      };
      ValidityCaptureChunk fd = {
        .type = CAPT_CHUNK_WTF,
        .size = sizeof (wtf_data),
        .data = g_memdup2 (wtf_data, sizeof (wtf_data)),
      };
      g_array_append_val (chunks_arr, fd);

      /* Image Reconstruction — ENROLL mode (byte 4 = 0x23) */
      static const guint8 recon_enroll[] = {
        0x02, 0x00, 0x18, 0x00, 0x23, 0x00, 0x00, 0x00,
        0x70, 0x00, 0x70, 0x00, 0x4d, 0x01, 0x00, 0x00,
        0xa0, 0x00, 0x8c, 0x00, 0x3c, 0x32, 0x32, 0x1e,
        0x3c, 0x0a, 0x02, 0x02,
      };
      ValidityCaptureChunk ir = {
        .type = CAPT_CHUNK_IMAGE_RECON,
        .size = sizeof (recon_enroll),
        .data = g_memdup2 (recon_enroll, sizeof (recon_enroll)),
      };
      g_array_append_val (chunks_arr, ir);
    }
  /* CALIBRATE mode: no Finger Detect or Image Reconstruction */

  /* --- Interleave --- */
  {
    guint8 interleave_data[4];
    validity_pack (interleave_data, sizeof (interleave_data), "w", (guint32) 1);
    ValidityCaptureChunk il = {
      .type = CAPT_CHUNK_INTERLEAVE,
      .size = 4,
      .data = g_memdup2 (interleave_data, 4),
    };
    g_array_append_val (chunks_arr, il);
  }

  /* --- Line Update and Line Update Transform ---
   * Build line entries from calibration data for the timeslot table. */
  lines_arr = g_array_new (FALSE, TRUE, sizeof (LineEntry));

  /* We need the patched timeslot table (before key_line replacement)
  * for instruction searches — see PY's line_update_type_1 which uses
  * the 'tst' variable, not 'c[1]' (which has key_line prepended). */
  if (patched_tst && patched_tst_len > 0)
    {
      const guint8 *tst_data = patched_tst;
      gsize tst_len = patched_tst_len;
      /* Line 0: calibration blob at Enable Rx position */
      {
        gssize pc = validity_capture_find_nth_insn (tst_data, tst_len,
                                                    TST_OP_ENABLE_RX, 2);
        if (pc >= 0 && type_info->calibration_blob)
          {
            LineEntry le = { 0 };
            le.mask = 0xff;
            le.flags = ((guint32) (pc + 1)) | ((guint32) cnt << 0x14) | 0x7000000;
            le.data = g_memdup2 (type_info->calibration_blob,
                                 type_info->calibration_blob_len);
            le.data_len = type_info->calibration_blob_len;
            le.v0 = 0x0f;
            g_array_append_val (lines_arr, le);
            cnt++;
          }
      }

      /* Line 1: factory calibration values at Register Write position */
      {
        gssize pc = validity_capture_find_nth_regwrite (tst_data, tst_len,
                                                        0x8000203C, 1);
        if (pc >= 0 && capture->factory_calibration_values)
          {
            LineEntry le = { 0 };
            le.mask = 0xff;
            le.flags = ((guint32) (pc + 1)) | ((guint32) cnt << 0x14) | 0x7000000;

            /* Bitpack the factory calibration values */
            le.data = validity_capture_bitpack (
              capture->factory_calibration_values,
              capture->factory_calibration_values_len,
              &le.v0, &le.v1, &le.data_len);
            le.v0 = (le.v0 - 1) | 8;
            cnt++;

            g_array_append_val (lines_arr, le);
          }
      }

      /* Calibration data lines (if we have calib_data) */
      if (capture->calib_data && capture->calib_data_len > 0)
        {
          gsize bytes_per_cal_line = capture->calib_data_len /
                                     type_info->lines_per_calibration_data;

          for (guint i = 0; i < 112; i += 4)
            {
              LineEntry le = { 0 };
              le.mask = 0xffffffff;
              le.flags = i | (0x85u << 24);

              /* Collect data from each calibration line at offset i */
              gsize row_data_len = 0;
              GByteArray *row = g_byte_array_new ();

              for (guint j = 0; j < 112; j++)
                {
                  gsize p = 8 + (gsize) j * bytes_per_cal_line + i;
                  if (p + 4 <= capture->calib_data_len)
                    {
                      g_byte_array_append (row, capture->calib_data + p, 4);
                    }
                  else
                    {
                      guint8 zeros[4] = { 0 };
                      g_byte_array_append (row, zeros, 4);
                    }
                }

              le.data_len = row->len;
              le.data = g_byte_array_free (row, FALSE);
              row_data_len = le.data_len;

              (void) row_data_len;
              g_array_append_val (lines_arr, le);
            }
        }
    }

  /* Align all line data to 4-byte boundary */
  for (gsize i = 0; i < lines_arr->len; i++)
    {
      LineEntry *le = &g_array_index (lines_arr, LineEntry, i);
      gsize pad = le->data_len % 4;
      if (pad > 0)
        {
          gsize new_len = le->data_len + (4 - pad);
          le->data = g_realloc (le->data, new_len);
          memset (le->data + le->data_len, 0, 4 - pad);
          le->data_len = new_len;
        }
    }

  /* Build Line Update chunk (entries with (flags & 0x00f00000) >> 0x14 <= 1) */
  {
    GByteArray *lu = g_byte_array_new ();
    guint32 n_lines = lines_arr->len;
    guint8 hdr[4];
    validity_pack (hdr, sizeof (hdr), "w", n_lines);
    g_byte_array_append (lu, hdr, 4);

    /* Mask + flags headers */
    for (gsize i = 0; i < lines_arr->len; i++)
      {
        LineEntry *le = &g_array_index (lines_arr, LineEntry, i);
        guint8 entry[8];
        validity_pack (entry, sizeof (entry), "ww", le->mask, le->flags);
        g_byte_array_append (lu, entry, 8);
      }

    /* Data for entries where (flags >> 20) & 0xf <= 1 */
    for (gsize i = 0; i < lines_arr->len; i++)
      {
        LineEntry *le = &g_array_index (lines_arr, LineEntry, i);
        guint32 slot = (le->flags & 0x00f00000) >> 0x14;
        if (slot <= 1 && le->data && le->data_len > 0)
          g_byte_array_append (lu, le->data, le->data_len);
      }

    ValidityCaptureChunk lu_chunk = {
      .type = CAPT_CHUNK_LINE_UPDATE,
      .size = lu->len,
      .data = g_byte_array_free (lu, FALSE),
    };
    g_array_append_val (chunks_arr, lu_chunk);
  }

  /* Build Line Update Transform chunk (entries where (flags >> 20) & 0xf > 1) */
  {
    GByteArray *lut = g_byte_array_new ();

    for (gsize i = 0; i < lines_arr->len; i++)
      {
        LineEntry *le = &g_array_index (lines_arr, LineEntry, i);
        guint32 slot = (le->flags & 0x00f00000) >> 0x14;
        if (slot > 1 && le->data && le->data_len > 0)
          {
            guint8 hdr[4];
            validity_pack (hdr, sizeof (hdr), "bbh", le->v0, le->v1, le->v2);
            g_byte_array_append (lut, hdr, 4);
            g_byte_array_append (lut, le->data, le->data_len);
          }
      }

    if (lut->len > 0)
      {
        ValidityCaptureChunk lut_chunk = {
          .type = CAPT_CHUNK_LINE_UPDATE_XFORM,
          .size = lut->len,
          .data = g_byte_array_free (lut, FALSE),
        };
        g_array_append_val (chunks_arr, lut_chunk);
      }
    else
      {
        g_byte_array_free (lut, TRUE);
      }
  }

  /* Free line entries */
  for (gsize i = 0; i < lines_arr->len; i++)
    g_free (g_array_index (lines_arr, LineEntry, i).data);
  g_array_free (lines_arr, TRUE);

  *out_n_chunks = chunks_arr->len;
  return (ValidityCaptureChunk *) g_array_free (chunks_arr, FALSE);
}

/* ================================================================
 * build_cmd_02 — assemble the final capture command
 *
 * Reference: python-validity sensor.py build_cmd_02()
 * ================================================================ */

guint8 *
validity_capture_build_cmd_02 (const ValidityCaptureState   *capture,
                               const ValiditySensorTypeInfo *type_info,
                               ValidityCaptureMode           mode,
                               gsize                        *out_len)
{
  ValidityCaptureChunk *chunks = NULL;
  ValidityCaptureChunk *patched = NULL;
  gsize n_chunks = 0;
  gsize n_patched = 0;
  guint8 *merged = NULL;
  gsize merged_len = 0;
  guint8 *cmd = NULL;
  guint16 req_lines;

  g_return_val_if_fail (capture != NULL, NULL);
  g_return_val_if_fail (type_info != NULL, NULL);
  g_return_val_if_fail (out_len != NULL, NULL);
  g_return_val_if_fail (capture->capture_prog != NULL, NULL);

  *out_len = 0;

  /* Split the capture program into chunks */
  chunks = validity_capture_split_chunks (capture->capture_prog,
                                          capture->capture_prog_len,
                                          &n_chunks);
  if (!chunks)
    return NULL;

  /* Apply line update patching (type 1 for our target devices) */
  if (capture->is_type1_device)
    {
      patched = build_line_update_type1 (capture, type_info, mode,
                                         chunks, n_chunks, &n_patched);
    }
  else
    {
      /* Type 2 devices — not yet implemented (0x9d, 0x97, etc.) */
      fp_warn ("Line update type 2 not yet implemented");
      validity_capture_chunks_free (chunks, n_chunks);
      return NULL;
    }

  validity_capture_chunks_free (chunks, n_chunks);

  if (!patched)
    return NULL;

  /* Debug: log chunk types and sizes */
  for (gsize i = 0; i < n_patched; i++)
    fp_dbg ("cmd_02 chunk[%zu]: type=0x%02x size=%u",
            i, patched[i].type, patched[i].size);

  /* Merge chunks back to binary */
  merged = validity_capture_merge_chunks (patched, n_patched, &merged_len);
  validity_capture_chunks_free (patched, n_patched);

  if (!merged)
    return NULL;

  /* Calculate requested lines */
  if (mode == VALIDITY_CAPTURE_CALIBRATE)
    req_lines = (guint16) (capture->calibration_frames * capture->lines_per_frame + 1);
  else
    req_lines = 0;

  /* Build final command: cmd(1) | bytes_per_line(2LE) | req_lines(2LE) | chunks */
  {
    cmd = validity_pack_new (out_len, "bhhd",
                             0x02,
                             (int) capture->bytes_per_line,
                             (int) req_lines,
                             merged, (gsize) merged_len);
    g_free (merged);
  }

  /* Debug: dump first 200 bytes of capture command for comparison with PY */
  {
    GString *hex = g_string_new ("");
    gsize dump_len = MIN (*out_len, 200);
    for (gsize i = 0; i < dump_len; i++)
      g_string_append_printf (hex, "%02x", cmd[i]);
    fp_dbg ("cmd_02 mode=%d len=%zu first %zu bytes: %s",
            mode, *out_len, dump_len, hex->str);
    g_string_free (hex, TRUE);
  }

  return cmd;
}

/* ================================================================
 * Factory bits parsing
 *
 * Reference: python-validity sensor.py get_factory_bits()
 * ================================================================ */

gboolean
validity_capture_parse_factory_bits (const guint8 *data,
                                     gsize         data_len,
                                     guint8      **cal_values,
                                     gsize        *cal_values_len,
                                     guint8      **cal_data,
                                     gsize        *cal_data_len)
{
  FpiByteReader reader;
  guint32 wtf, entries;
  gboolean found_subtag3 = FALSE;

  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (cal_values != NULL && cal_values_len != NULL, FALSE);

  *cal_values = NULL;
  *cal_values_len = 0;
  if (cal_data)
    {
      *cal_data = NULL;
      *cal_data_len = 0;
    }

  if (data_len < 8)
    return FALSE;

  if (!validity_unpack (data, data_len, "ww", &wtf, &entries))
    return FALSE;

  (void) wtf;

  fpi_byte_reader_init (&reader, data + 8, data_len - 8);

  for (guint32 i = 0; i < entries; i++)
    {
      guint32 ptr;
      guint16 length, tag, subtag, flags;
      const guint8 *entry_data;

      if (!fpi_byte_reader_get_uint32_le (&reader, &ptr) ||
          !fpi_byte_reader_get_uint16_le (&reader, &length) ||
          !fpi_byte_reader_get_uint16_le (&reader, &tag) ||
          !fpi_byte_reader_get_uint16_le (&reader, &subtag) ||
          !fpi_byte_reader_get_uint16_le (&reader, &flags))
        break;

      (void) ptr;
      (void) tag;
      (void) flags;

      if (!fpi_byte_reader_get_data (&reader, length, &entry_data))
        break;

      /* Subtag 3: factory calibration values.
       * First 4 bytes are a header; actual values start at +4.
       * Free any earlier allocation first: a device may (validly or
       * maliciously) send more than one subtag-3 entry. */
      if (subtag == 3 && length > 4)
        {
          *cal_values_len = length - 4;
          g_clear_pointer (cal_values, g_free);
          *cal_values = g_memdup2 (entry_data + 4, *cal_values_len);
          found_subtag3 = TRUE;
        }

      /* Subtag 7: optional factory calibration data.
       * Also has a 4-byte header. */
      if (subtag == 7 && length > 4 && cal_data && cal_data_len)
        {
          *cal_data_len = length - 4;
          g_clear_pointer (cal_data, g_free);
          *cal_data = g_memdup2 (entry_data + 4, *cal_data_len);
        }
    }

  return found_subtag3;
}

/* ================================================================
 * Frame averaging
 *
 * Reference: python-validity sensor.py average()
 * ================================================================ */

guint8 *
validity_capture_average_frames (const guint8 *raw_data,
                                 gsize         raw_len,
                                 guint16       lines_per_frame,
                                 guint16       bytes_per_line,
                                 guint16       lines_per_calibration_data,
                                 guint8        calibration_frames,
                                 gsize        *out_len)
{
  gsize frame_size;
  guint16 interleave_lines;
  guint8 input_frames;
  gsize base_address = 0;
  guint8 *result;
  gsize result_len;

  g_return_val_if_fail (raw_data != NULL, NULL);
  g_return_val_if_fail (out_len != NULL, NULL);
  g_return_val_if_fail (lines_per_calibration_data > 0, NULL);

  frame_size = (gsize) lines_per_frame * bytes_per_line;
  interleave_lines = lines_per_frame / lines_per_calibration_data;
  input_frames = calibration_frames;

  if (interleave_lines <= 0)
    {
      *out_len = 0;
      return NULL;
    }

  if (interleave_lines > 1)
    {
      const guint8 *frame;

      if (input_frames > 1)
        {
          /* Skip the first frame */
          input_frames--;
          base_address = frame_size;
        }

      if (base_address + frame_size > raw_len)
        {
          *out_len = 0;
          return NULL;
        }

      frame = raw_data + base_address;

      /* Result: one line per group of interleaved lines */
      result_len = (gsize) lines_per_calibration_data * bytes_per_line;
      result = g_malloc0 (result_len);

      for (guint16 line = 0; line < lines_per_calibration_data; line++)
        {
          gsize group_offset = (gsize) line * interleave_lines * bytes_per_line;

          for (guint16 col = 0; col < bytes_per_line; col++)
            {
              guint sum = 0;
              for (guint16 il = 0; il < interleave_lines; il++)
                {
                  gsize idx = group_offset + (gsize) il * bytes_per_line + col;
                  if (idx < frame_size)
                    sum += frame[idx];
                }
              result[(gsize) line * bytes_per_line + col] =
                (guint8) (sum / interleave_lines);
            }
        }

      *out_len = result_len;
      return result;
    }
  else
    {
      /* interleave_lines == 1: average multiple frames */
      if (input_frames > 1)
        {
          /* Average frames 1..N (skip frame 0) */
          result_len = frame_size;
          result = g_malloc0 (result_len);

          for (gsize i = 0; i < frame_size; i++)
            {
              guint sum = 0;
              for (guint8 f = 1; f <= input_frames; f++)
                {
                  gsize idx = (gsize) f * frame_size + i;
                  if (idx < raw_len)
                    sum += raw_data[idx];
                }
              result[i] = (guint8) (sum / input_frames);
            }

          *out_len = result_len;
          return result;
        }
      else
        {
          /* Single frame — just copy */
          if (frame_size > raw_len)
            {
              *out_len = 0;
              return NULL;
            }
          result = g_memdup2 (raw_data, frame_size);
          *out_len = frame_size;
          return result;
        }
    }
}

/* ================================================================
 * Calibration data processing
 *
 * Reference: python-validity sensor.py process_calibration_results()
 * ================================================================ */

void
validity_capture_process_calibration (guint8      **calib_data,
                                      gsize        *calib_data_len,
                                      const guint8 *averaged_frame,
                                      gsize         frame_len,
                                      guint16       bytes_per_line)
{
  guint8 *frame_scaled;

  g_return_if_fail (calib_data != NULL && calib_data_len != NULL);
  g_return_if_fail (averaged_frame != NULL);

  /* Apply scaling: leave first 8 bytes of each line, scale the rest */
  frame_scaled = g_memdup2 (averaged_frame, frame_len);

  {
    gsize n_lines = frame_len / bytes_per_line;
    for (gsize line = 0; line < n_lines; line++)
      {
        gsize line_start = line * bytes_per_line;
        /* First 8 bytes: untouched */
        /* Bytes 8+: scale */
        for (gsize col = 8; col < bytes_per_line && line_start + col < frame_len; col++)
          frame_scaled[line_start + col] = scale_byte (frame_scaled[line_start + col]);
      }
  }

  if (*calib_data != NULL && *calib_data_len > 0)
    {
      /* Combine with existing calibration data */
      gsize len = MIN (*calib_data_len, frame_len);
      gsize n_lines = len / bytes_per_line;

      for (gsize line = 0; line < n_lines; line++)
        {
          gsize off = line * bytes_per_line;
          /* First 8 bytes: keep as-is from previous */
          for (gsize col = 8; col < bytes_per_line && off + col < len; col++)
            (*calib_data)[off + col] = add_signed_bytes ((*calib_data)[off + col],
                                                         frame_scaled[off + col]);
        }
    }
  else
    {
      /* First calibration — use this frame */
      g_free (*calib_data);
      *calib_data = g_memdup2 (frame_scaled, frame_len);
      *calib_data_len = frame_len;
    }

  g_free (frame_scaled);
}

/* ================================================================
 * Clean slate
 *
 * Reference: python-validity sensor.py calibrate() (clean slate format)
 * ================================================================ */

guint8 *
validity_capture_build_clean_slate (const guint8 *averaged_frame,
                                    gsize         frame_len,
                                    gsize        *out_len)
{
  /* Inner payload: data_len(2LE) | data | trailing_zero(2LE=0) */
  gsize inner_payload_len = 2 + frame_len + 2;
  gsize total_len;
  guint8 *buf;
  guint8 hash[32];
  EVP_MD_CTX *ctx;
  guint hash_len = 32;

  g_return_val_if_fail (averaged_frame != NULL, NULL);
  g_return_val_if_fail (out_len != NULL, NULL);

  /* Build inner payload */
  guint8 *inner_payload;
  inner_payload = validity_pack_new (&inner_payload_len, "hdh",
                                     (int) frame_len,
                                     averaged_frame, frame_len,
                                     0);  /* trailing zero */

  /* SHA256 of inner_payload */
  ctx = EVP_MD_CTX_new ();
  EVP_DigestInit_ex (ctx, EVP_sha256 (), NULL);
  EVP_DigestUpdate (ctx, inner_payload, inner_payload_len);
  EVP_DigestFinal_ex (ctx, hash, &hash_len);
  EVP_MD_CTX_free (ctx);

  /* Build final buffer: magic(2) | inner_len(2) | sha256(32) | zeroes(32) | payload */
  {
    static const guint8 zero_pad[32] = { 0 };
    buf = validity_pack_new (&total_len, "hhddd",
                             (int) 0x5002,
                             (int) inner_payload_len,
                             hash, (gsize) 32,
                             zero_pad, (gsize) 32,
                             inner_payload, (gsize) inner_payload_len);
  }

  g_free (inner_payload);
  *out_len = total_len;
  return buf;
}

gboolean
validity_capture_verify_clean_slate (const guint8 *data,
                                     gsize         data_len)
{
  guint16 magic, inner_len;
  const guint8 *hash_stored;
  const guint8 *zeroes;
  const guint8 *payload;
  guint8 hash_computed[32];
  EVP_MD_CTX *ctx;
  guint hash_len = 32;

  if (data_len < 68)  /* 2+2+32+32 minimum */
    return FALSE;

  /* Unpack fixed header: magic(2) | inner_len(2) | hash(32) | zeroes(32) */
  if (!validity_unpack (data, data_len, "hhdd",
                        &magic, &inner_len,
                        &hash_stored, (gsize) 32,
                        &zeroes, (gsize) 32))
    return FALSE;
  if (magic != 0x5002)
    return FALSE;

  /* Check zeroes block */
  for (int i = 0; i < 32; i++)
    if (zeroes[i] != 0)
      return FALSE;

  /* Verify hash — payload starts at offset 68 (2+2+32+32) */
  if (68 + (gsize) inner_len > data_len)
    return FALSE;
  payload = data + 68;

  ctx = EVP_MD_CTX_new ();
  EVP_DigestInit_ex (ctx, EVP_sha256 (), NULL);
  EVP_DigestUpdate (ctx, payload, inner_len);
  EVP_DigestFinal_ex (ctx, hash_computed, &hash_len);
  EVP_MD_CTX_free (ctx);

  return memcmp (hash_stored, hash_computed, 32) == 0;
}

/* ================================================================
 * Finger ID mapping
 *
 * Maps FpFinger enum to VCSFW finger subtype (1-10).
 * FpFinger: LEFT_THUMB=1, LEFT_INDEX=2, ..., RIGHT_LITTLE=10
 * VCSFW subtype: same 1-10 mapping (matches WINBIO_ANSI_381_POS)
 * ================================================================ */

guint16
validity_finger_to_subtype (guint finger)
{
  /* FpFinger values: FP_FINGER_LEFT_THUMB=1 through FP_FINGER_RIGHT_LITTLE=10 */
  if (finger >= 1 && finger <= 10)
    return (guint16) finger;
  return 0;
}

gint
validity_subtype_to_finger (guint16 subtype)
{
  if (subtype >= 1 && subtype <= 10)
    return (gint) subtype;
  return -1;
}

/* ================================================================
 * LED control commands
 *
 * Reference: python-validity sensor.py glow_start_scan(), glow_end_scan()
 * These are sent via tls.app() (TLS application data).
 * ================================================================ */

static const guint8 glow_start_data[] = {
  0x39, 0x20, 0xbf, 0x02, 0x00, 0xff, 0xff, 0x00,
  0x00, 0x01, 0x99, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0x99, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00,
  0x00, 0x00, 0x99, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

static const guint8 glow_end_data[] = {
  0x39, 0xf4, 0x01, 0x00, 0x00, 0xf4, 0x01, 0x00,
  0x00, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x01, 0x00,
  0x00, 0x00, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
};

const guint8 *
validity_capture_glow_start_cmd (gsize *out_len)
{
  *out_len = sizeof (glow_start_data);
  return glow_start_data;
}

const guint8 *
validity_capture_glow_end_cmd (gsize *out_len)
{
  *out_len = sizeof (glow_end_data);
  return glow_end_data;
}

/* ================================================================
 * CaptureProg table
 *
 * Hardcoded capture program for the target firmware/device combination.
 * This is the generic capture program for firmware 6.x with line_update_type1
 * geometry (bytes_per_line=0x78, line_width=112).
 *
 * The blob is split into TLV chunks:
 *   0x002a: ACM Config (8 bytes)
 *   0x002c: CEM Config (40 bytes)
 *   0x0034: Timeslot Table 2D (64 bytes)
 *   0x002f: 2D Params (4 bytes) — lines per calibration data
 *   0x0029: Timeslot Table Offset (4 bytes)
 *   0x0035: Timeslot Table Offset for Finger Detect (4 bytes)
 *
 * Reference: python-validity generated_tables.py SensorCaptureProg entries
 * ================================================================ */

/* Generic capture program for firmware 6.x, type1 devices (incl. 0xb5).
 * The Timeslot Table 2D and 2D Params chunks are the key data. */
static const guint8 capture_prog_type1_b5[] = {
  /* ACM Config (0x2a, 8 bytes) */
  0x2a, 0x00, 0x08, 0x00,
  0x20, 0x01, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00,

  /* CEM Config (0x2c, 40 bytes) */
  0x2c, 0x00, 0x28, 0x00,
  0x80, 0x20, 0x80, 0x20, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x3f, 0x40, 0x00, 0x00, 0x00, 0x00,
  0x08, 0x0f, 0x08, 0x0f, 0x00, 0x00, 0x00, 0x00,
  0x27, 0x9c, 0x10, 0x00, 0x27, 0x9c, 0x10, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

  /* Timeslot Table 2D (0x34, 64 bytes) */
  0x34, 0x00, 0x40, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x07, 0x16, 0x00, 0x00,
  0x24, 0x0a, 0x59, 0x08, 0x5a, 0x07, 0x01, 0xc9,
  0x50, 0x0a, 0xaa, 0x07, 0x01, 0x0a, 0xda, 0x08,
  0xdb, 0x07, 0x01, 0xc9, 0x46, 0x0b, 0x21, 0x07,
  0x01, 0x08, 0x00, 0x80, 0x0a, 0x00, 0x88, 0xc9,
  0x59, 0x0a, 0x5a, 0x07, 0x01, 0x0a, 0xa9, 0x08,
  0xaa, 0x07, 0x01, 0xc9, 0x1f, 0x0a, 0xc9, 0x00,
  0x00, 0x0c, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00,

  /* 2D Params (0x2f, 4 bytes) — lines_per_calibration_data = 112 = 0x70 */
  0x2f, 0x00, 0x04, 0x00,
  0x70, 0x00, 0x00, 0x00,

  /* Timeslot Table Offset (0x29, 4 bytes) */
  0x29, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00,

  /* Timeslot Table Offset for Finger Detect (0x35, 4 bytes) */
  0x35, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

/* Device-specific capture program for sensor type 0x0199 (57K0 family).
 * From python-validity SensorCaptureProg entry: major=6, dev_type=0x199,
 * a0=0x18, a1=0x19, 2 blobs totalling 648 bytes. */
static const guint8 capture_prog_type1_0199[] = {
  /* Blob 0: 228 bytes */
  0x23, 0x00, 0x00, 0x00, 0x20, 0x00, 0x08, 0x00,
  0x00, 0x20, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00,
  0x32, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x80,
  0x20, 0x20, 0x04, 0x00, 0x24, 0x20, 0x00, 0x00,
  0x50, 0x20, 0x77, 0x36, 0x28, 0x20, 0x01, 0x00,
  0x30, 0x20, 0x01, 0x00, 0x3c, 0x20, 0x80, 0x00,
  0x08, 0x21, 0x38, 0x00, 0x0c, 0x21, 0x00, 0x00,
  0x48, 0x21, 0x07, 0x00, 0x4c, 0x21, 0x00, 0x00,
  0x58, 0x20, 0x00, 0x00, 0x5c, 0x20, 0x00, 0x00,
  0x60, 0x20, 0x00, 0x00, 0x68, 0x20, 0x05, 0x00,
  0x6c, 0x20, 0x01, 0x49, 0x70, 0x20, 0x01, 0x41,
  0x74, 0x20, 0x01, 0x88, 0x78, 0x20, 0x01, 0x80,
  0x84, 0x20, 0x20, 0x00, 0x94, 0x20, 0x01, 0x80,
  0x9c, 0x20, 0x09, 0x02, 0xa0, 0x20, 0x0b, 0x19,
  0xb4, 0x20, 0x03, 0x00, 0xb8, 0x20, 0x3b, 0x04,
  0xbc, 0x20, 0x14, 0x00, 0xc0, 0x20, 0x02, 0x00,
  0xc4, 0x20, 0x01, 0x00, 0xc8, 0x20, 0x02, 0x00,
  0x33, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x80,
  0xcc, 0x20, 0x00, 0x00, 0xf5, 0x03, 0xd0, 0x20,
  0x00, 0x00, 0xa1, 0x01, 0x32, 0x00, 0x44, 0x00,
  0x00, 0x00, 0x00, 0x80, 0xdc, 0x20, 0xe8, 0x03,
  0xe0, 0x20, 0x64, 0x01, 0xe4, 0x20, 0xd0, 0x02,
  0xe8, 0x20, 0x00, 0x01, 0xf0, 0x20, 0x05, 0x00,
  0xf8, 0x20, 0x05, 0x00, 0xfc, 0x20, 0x00, 0x00,
  0xb8, 0x20, 0x3a, 0x00, 0x00, 0x08, 0x04, 0x00,
  0x14, 0x08, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00,
  0x08, 0x08, 0x00, 0x00, 0x14, 0x08, 0x30, 0x00,
  0x08, 0x08, 0x00, 0x00, 0x14, 0x08, 0x31, 0x00,
  0x1c, 0x08, 0x1a, 0x00,
  /* Blob 1: 420 bytes */
  0x32, 0x00, 0x0c, 0x00,
  0x00, 0x00, 0x00, 0x80, 0x50, 0x11, 0x01, 0x00,
  0x4c, 0x11, 0x1e, 0x00, 0x34, 0x00, 0x78, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x22, 0x17, 0x10, 0x22, 0x17, 0x10, 0x22,
  0x16, 0x10, 0x22, 0x16, 0x10, 0x22, 0x16, 0x01,
  0x06, 0x50, 0x10, 0x25, 0x01, 0x01, 0x00, 0x00,
  0x07, 0xc8, 0x07, 0x8c, 0x06, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x4f, 0x80, 0x00, 0x6d,
  0x03, 0x00, 0x28, 0x03, 0x07, 0x03, 0x09, 0x90,
  0x09, 0x8d, 0xb0, 0x0b, 0x90, 0x88, 0x09, 0x91,
  0x85, 0x8e, 0x08, 0xc1, 0x81, 0x0b, 0x91, 0x90,
  0x91, 0x0a, 0xc1, 0xb8, 0x92, 0x8a, 0x09, 0x93,
  0x87, 0x8a, 0x89, 0x0b, 0x93, 0x88, 0x89, 0x89,
  0x08, 0xc8, 0x81, 0x91, 0x89, 0x0a, 0xc8, 0x88,
  0x92, 0x89, 0x09, 0x9a, 0x81, 0x8a, 0x89, 0x0b,
  0x9a, 0x88, 0x89, 0x89, 0x08, 0xd0, 0x81, 0x91,
  0x89, 0x0a, 0xd0, 0x88, 0x92, 0x89, 0x08, 0x02,
  0x81, 0x8a, 0x09, 0x5a, 0x81, 0x0a, 0x02, 0x88,
  0x89, 0x0b, 0x5a, 0x88, 0x08, 0xd9, 0x81, 0x89,
  0x89, 0x0a, 0xd9, 0x90, 0x89, 0x89, 0x09, 0x5e,
  0x82, 0x89, 0x89, 0x0b, 0x5e, 0x88, 0x89, 0x89,
  0x08, 0xe1, 0x81, 0x89, 0x89, 0x0a, 0xe1, 0x90,
  0x89, 0x89, 0x09, 0x64, 0x82, 0x89, 0x89, 0x0b,
  0x64, 0x88, 0x89, 0x09, 0x6e, 0x81, 0x08, 0xe9,
  0x81, 0x89, 0x0b, 0x6e, 0x88, 0x0a, 0xe9, 0x90,
  0x91, 0xb9, 0x09, 0x6f, 0x82, 0x8a, 0x8f, 0x0b,
  0x6f, 0x88, 0x91, 0x89, 0x08, 0xf0, 0x81, 0x8a,
  0x89, 0x0a, 0xf0, 0x90, 0x89, 0x89, 0x09, 0x76,
  0x82, 0x89, 0x91, 0x0b, 0x76, 0xb8, 0x91, 0x8a,
  0x08, 0xf8, 0x87, 0x92, 0x91, 0x0a, 0xf8, 0x88,
  0x8a, 0x92, 0x09, 0x7c, 0x81, 0x89, 0x8a, 0x0b,
  0x7c, 0x09, 0x01, 0x80, 0x89, 0x89, 0x0b, 0x01,
  0x88, 0x89, 0x91, 0x09, 0x7f, 0x81, 0x89, 0x92,
  0x0b, 0x7f, 0x09, 0x08, 0x80, 0x89, 0x92, 0x0b,
  0x08, 0x88, 0x89, 0x92, 0x0c, 0x07, 0x03, 0x03,
  0x07, 0x20, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00,
  0x2f, 0x00, 0x04, 0x00, 0x70, 0x00, 0x00, 0x00,
  0x29, 0x00, 0x04, 0x00, 0x70, 0x00, 0x00, 0x00,
  0x35, 0x00, 0x04, 0x00, 0x80, 0x00, 0x00, 0x00,
};

/* Device types that use line_update_type_1 */
static const guint16 line_update_type1_devices[] = {
  0x00B5, 0x0885, 0x00B3, 0x143B, 0x1055,
  0x00E1, 0x08B1, 0x00EA, 0x00E4, 0x00ED,
  0x1825, 0x1FF5, 0x0199, 0x0969, 0x0D51,
};

const guint8 *
validity_capture_prog_lookup (guint8  rom_major,
                              guint8  rom_minor,
                              guint16 dev_type,
                              gsize  *out_len)
{
  g_return_val_if_fail (out_len != NULL, NULL);

  /* Currently we only have capture program data for firmware 6.x,
   * type-1 devices with 0x78 bytes/line geometry. */
  if (rom_major == 6)
    {
      /* Device-specific programs take priority. 0x969 and 0xd51 silicon alias
       * to the 0x199 profile (identical 57K0 geometry) and use its capture
       * program; mirrors python-validity aliasing device_info.type -> 0x199. */
      if (dev_type == 0x0199 || dev_type == 0x0969 || dev_type == 0x0d51)
        {
          *out_len = sizeof (capture_prog_type1_0199);
          return capture_prog_type1_0199;
        }

      for (gsize i = 0; i < G_N_ELEMENTS (line_update_type1_devices); i++)
        {
          if (line_update_type1_devices[i] == dev_type)
            {
              *out_len = sizeof (capture_prog_type1_b5);
              return capture_prog_type1_b5;
            }
        }
    }

  *out_len = 0;
  return NULL;

  (void) rom_minor;
}

/* ================================================================
 * Capture state lifecycle
 * ================================================================ */

void
validity_capture_state_init (ValidityCaptureState *state)
{
  memset (state, 0, sizeof (*state));
}

void
validity_capture_state_clear (ValidityCaptureState *state)
{
  g_clear_pointer (&state->factory_calibration_values, g_free);
  g_clear_pointer (&state->factory_calib_data, g_free);
  g_clear_pointer (&state->calib_data, g_free);
  memset (state, 0, sizeof (*state));
}

gboolean
validity_capture_state_setup (ValidityCaptureState         *state,
                              const ValiditySensorTypeInfo *type_info,
                              guint16                       dev_type,
                              guint8                        rom_major,
                              guint8                        rom_minor,
                              const guint8                 *factory_bits,
                              gsize                         factory_bits_len)
{
  const guint8 *prog;
  gsize prog_len;

  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (type_info != NULL, FALSE);

  /* Look up capture program */
  prog = validity_capture_prog_lookup (rom_major, rom_minor, dev_type, &prog_len);
  if (!prog)
    {
      fp_warn ("No capture program for rom %d.%d, dev_type 0x%04x",
               rom_major, rom_minor, dev_type);
      return FALSE;
    }

  state->capture_prog = prog;
  state->capture_prog_len = prog_len;

  /* Check if this is a type-1 device */
  state->is_type1_device = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS (line_update_type1_devices); i++)
    {
      if (line_update_type1_devices[i] == dev_type)
        {
          state->is_type1_device = TRUE;
          break;
        }
    }

  /* Extract lines_per_frame from the 2D params chunk */
  {
    gsize n_chunks = 0;
    ValidityCaptureChunk *chunks = validity_capture_split_chunks (prog, prog_len, &n_chunks);
    if (chunks)
      {
        for (gsize i = 0; i < n_chunks; i++)
          {
            if (chunks[i].type == CAPT_CHUNK_2D_PARAMS && chunks[i].size >= 4)
              {
                guint32 lines_2d;
                validity_unpack (chunks[i].data, chunks[i].size, "w", &lines_2d);
                state->lines_per_frame = (guint16) (lines_2d * type_info->repeat_multiplier);
                break;
              }
          }
        validity_capture_chunks_free (chunks, n_chunks);
      }
  }

  state->bytes_per_line = type_info->bytes_per_line;

  /* Set calibration parameters.
   * For sensor_type 0xb5 (and all type-1 devices with the same geometry
   * as 0x199): key_calibration_line = lines_per_calibration_data / 2,
   * calibration_frames = 3, calibration_iterations = 3.
   * Derived from python-validity (0x199 has identical SensorTypeInfo). */
  state->key_calibration_line = type_info->lines_per_calibration_data / 2;
  state->calibration_frames = 3;
  state->calibration_iterations = 3;

  /* Parse factory bits if available */
  if (factory_bits && factory_bits_len > 0)
    {
      validity_capture_parse_factory_bits (factory_bits, factory_bits_len,
                                           &state->factory_calibration_values,
                                           &state->factory_calibration_values_len,
                                           &state->factory_calib_data,
                                           &state->factory_calib_data_len);
    }

  return TRUE;
}
