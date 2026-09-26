/*
 * Capture program infrastructure for Validity/Synaptics VCSFW sensors
 *
 * Implements the capture command builder (cmd 0x02), timeslot table
 * patching, factory calibration parsing, frame averaging, and
 * calibration data processing.
 *
 * Reference: python-validity sensor.py, timeslot.py, generated_tables.py
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

#pragma once

#include <glib.h>
#include "validity_sensor.h"

/* ================================================================
 * Capture mode — passed to build_cmd_02 to select capture behavior
 * Values match python-validity CaptureMode enum.
 * ================================================================ */
typedef enum {
  VALIDITY_CAPTURE_CALIBRATE       = 1,
  VALIDITY_CAPTURE_IDENTIFY        = 2,
  VALIDITY_CAPTURE_ENROLL          = 3,
  VALIDITY_CAPTURE_ENROLL_IDENTIFY = 4,  /* IDENTIFY chunk (0x4e) + ENROLL recon (0x23) */
} ValidityCaptureMode;

/* ================================================================
 * Capture program chunk — TLV entry from the CaptureProg table.
 * Format: type(2LE) | size(2LE) | data(size bytes)
 * ================================================================ */
typedef struct
{
  guint16 type;
  guint16 size;
  guint8 *data;      /* owned; g_free() when done */
} ValidityCaptureChunk;

/* ================================================================
 * Capture state — kept in the device struct, initialized during open
 * ================================================================ */
typedef struct
{
  /* Capture program binary (from CaptureProg table lookup) */
  const guint8 *capture_prog;
  gsize         capture_prog_len;

  /* Geometry derived from SensorTypeInfo + CaptureProg */
  guint16 lines_per_frame;
  guint16 bytes_per_line;

  /* Calibration parameters (derived from sensor geometry) */
  guint16 key_calibration_line;
  guint8  calibration_frames;
  guint8  calibration_iterations;

  /* Factory calibration values (from factory_bits subtag 3) */
  guint8 *factory_calibration_values;
  gsize   factory_calibration_values_len;

  /* Optional factory calibration data (from factory_bits subtag 7) */
  guint8 *factory_calib_data;
  gsize   factory_calib_data_len;

  /* Accumulated calibration data (built during calibration loop) */
  guint8 *calib_data;
  gsize   calib_data_len;

  /* Whether this is a line_update_type_1 device */
  gboolean is_type1_device;
} ValidityCaptureState;

/* ================================================================
 * Chunk parsing — split/merge TLV-encoded capture program
 * ================================================================ */

/*
 * Split a capture program binary into an array of chunks.
 * Returns an array of ValidityCaptureChunk (caller must free with
 * validity_capture_chunks_free). Sets n_chunks to the count.
 * Returns NULL on parse error.
 */
ValidityCaptureChunk *validity_capture_split_chunks (const guint8 *data,
                                                     gsize         data_len,
                                                     gsize        *n_chunks);

/*
 * Merge an array of chunks back into a binary blob.
 * Caller must g_free() the returned buffer.
 * Returns NULL on error and sets out_len to 0.
 */
guint8 *validity_capture_merge_chunks (const ValidityCaptureChunk *chunks,
                                       gsize                       n_chunks,
                                       gsize                      *out_len);

/*
 * Free an array of chunks (including each chunk's data).
 */
void validity_capture_chunks_free (ValidityCaptureChunk *chunks,
                                   gsize                 n_chunks);

/* ================================================================
 * Timeslot instruction decoder
 *
 * The timeslot table is a bytecode program for the sensor DSP.
 * Each instruction is 1-3 bytes. We need to decode them to find
 * specific instructions for patching.
 *
 * Returns: opcode in *opcode, instruction length in *insn_len,
 *          operands in operands[] (up to 3), count in *n_operands.
 *          Returns FALSE if the instruction cannot be decoded.
 * ================================================================ */
gboolean validity_capture_decode_insn (const guint8 *data,
                                       gsize         data_len,
                                       guint8       *opcode,
                                       guint8       *insn_len,
                                       guint32       operands[3],
                                       guint8       *n_operands);

/* Timeslot instruction opcodes */
#define TST_OP_NOOP 0
#define TST_OP_END_OF_TABLE 1
#define TST_OP_RETURN 2
#define TST_OP_CLEAR_SO 3
#define TST_OP_END_OF_DATA 4
#define TST_OP_MACRO 5
#define TST_OP_ENABLE_RX 6
#define TST_OP_IDLE_RX 7
#define TST_OP_ENABLE_SO 8
#define TST_OP_DISABLE_SO 9
#define TST_OP_INTERRUPT 10
#define TST_OP_CALL 11
#define TST_OP_FEATURES 12
#define TST_OP_REG_WRITE 13
#define TST_OP_SAMPLE 14
#define TST_OP_SAMPLE_REPEAT 15

/*
 * Find the Nth instruction with the given opcode.
 * Returns the byte offset (pc) of the instruction, or -1 if not found.
 */
gssize validity_capture_find_nth_insn (const guint8 *data,
                                       gsize         data_len,
                                       guint8        target_opcode,
                                       guint         n);

/*
 * Find the Nth Register Write instruction to a specific register address.
 * Returns the byte offset (pc), or -1 if not found.
 */
gssize validity_capture_find_nth_regwrite (const guint8 *data,
                                           gsize         data_len,
                                           guint32       reg_addr,
                                           guint         n);

/* ================================================================
 * Timeslot table patching
 * ================================================================ */

/*
 * First pass: patch the timeslot table multiplier.
 * For each "Call" instruction (opcode 0x10-0x17), if repeat > 1,
 * multiply it by mult and optionally increment the address.
 * Modifies data in-place. Returns TRUE on success.
 */
gboolean validity_capture_patch_timeslot_table (guint8  *data,
                                                gsize    data_len,
                                                gboolean inc_address,
                                                guint8   mult);

/*
 * Second pass: find the register write to 0x8000203C in the Call
 * target subroutine and patch its value with the factory calibration
 * value at key_calibration_line.
 * Modifies data in-place. Returns TRUE on success; FALSE means
 * no matching instruction was found (non-fatal).
 */
gboolean validity_capture_patch_timeslot_again (guint8       *data,
                                                gsize         data_len,
                                                const guint8 *factory_calibration_values,
                                                gsize         factory_cal_len,
                                                guint16       key_calibration_line);

/* ================================================================
 * build_cmd_02 — main capture command builder
 * ================================================================ */

/*
 * Build a capture command (cmd 0x02).
 * Format: 0x02 | bytes_per_line(2LE) | req_lines(2LE) | merged_chunks
 *
 * The capture program is loaded from capture_state->capture_prog,
 * split into chunks, patched (timeslot, line_update), and reassembled.
 *
 * Returns a newly-allocated buffer (caller must g_free) or NULL on error.
 * Sets out_len to the buffer size.
 */
guint8 *validity_capture_build_cmd_02 (const ValidityCaptureState   *capture,
                                       const ValiditySensorTypeInfo *type_info,
                                       ValidityCaptureMode           mode,
                                       gsize                        *out_len);

/* ================================================================
 * Factory bits parsing
 * ================================================================ */

/*
 * Parse the raw factory bits response (from cmd 0x6f).
 * Extracts subtag 3 (factory_calibration_values) and optionally
 * subtag 7 (factory_calib_data).
 *
 * Response format (after 2-byte status, already stripped):
 *   wtf(4LE) | entries(4LE) | entry[]:
 *     ptr(4LE) | length(2LE) | tag(2LE) | subtag(2LE) | flags(2LE) | data(length)
 *
 * Returns TRUE if at least subtag 3 was found.
 */
gboolean validity_capture_parse_factory_bits (const guint8 *data,
                                              gsize         data_len,
                                              guint8      **cal_values,
                                              gsize        *cal_values_len,
                                              guint8      **cal_data,
                                              gsize        *cal_data_len);

/* ================================================================
 * Frame averaging
 * ================================================================ */

/*
 * Average raw multi-frame capture data from EP 0x82.
 * Deinterlaces frames and averages interleaved lines.
 *
 * Returns a newly-allocated buffer (one frame), or NULL on error.
 * The caller must g_free() the buffer.
 */
guint8 *validity_capture_average_frames (const guint8 *raw_data,
                                         gsize         raw_len,
                                         guint16       lines_per_frame,
                                         guint16       bytes_per_line,
                                         guint16       lines_per_calibration_data,
                                         guint8        calibration_frames,
                                         gsize        *out_len);

/* ================================================================
 * Calibration data processing
 * ================================================================ */

/*
 * Process averaged calibration frame into calibration data.
 * Applies scaling and accumulates with existing calib_data.
 * Updates calib_data/calib_data_len in place.
 *
 * If calib_data is NULL, initializes from the frame.
 * If non-NULL, combines (adds signed values, clips).
 */
void validity_capture_process_calibration (guint8      **calib_data,
                                           gsize        *calib_data_len,
                                           const guint8 *averaged_frame,
                                           gsize         frame_len,
                                           guint16       bytes_per_line);

/*
 * Build the clean slate format for flash persistence.
 * Format: magic(2LE=0x5002) | inner_len(2LE) | sha256(32) | zeroes(32) |
 *         data_len(2LE) | data | trailing_zero(2LE=0)
 *
 * Returns a newly-allocated buffer or NULL on error.
 */
guint8 *validity_capture_build_clean_slate (const guint8 *averaged_frame,
                                            gsize         frame_len,
                                            gsize        *out_len);

/*
 * Verify clean slate format (check magic and SHA256 hash).
 * Returns TRUE if the format is valid.
 */
gboolean validity_capture_verify_clean_slate (const guint8 *data,
                                              gsize         data_len);

/* ================================================================
 * Bitpack — compress factory calibration values for Line Update
 * ================================================================ */

/*
 * Pack calibration values using minimum-bit encoding.
 * Returns packed data, minimum value (v1), bit count (v0),
 * and packed length.
 *
 * Python-validity bitpack(): find min, max delta, encode each value
 * as (value - min) in minimum bits, pack as little-endian.
 *
 * Returns a newly-allocated buffer or NULL on error.
 */
guint8 *validity_capture_bitpack (const guint8 *values,
                                  gsize         values_len,
                                  guint8       *out_v0,
                                  guint8       *out_v1,
                                  gsize        *out_len);

/* ================================================================
 * Finger ID mapping
 * ================================================================ */

/*
 * Map FpFinger enum value to VCSFW finger subtype (1-10).
 * Returns 0 if the finger is not recognized.
 */
guint16 validity_finger_to_subtype (guint finger);

/*
 * Map VCSFW finger subtype (1-10) to FpFinger enum value.
 * Returns -1 if the subtype is not recognized.
 */
gint validity_subtype_to_finger (guint16 subtype);

/* ================================================================
 * LED control commands — for user feedback during capture
 * ================================================================ */

/*
 * Build the LED "glow start scan" command (sent via TLS app).
 * Returns a static buffer and sets out_len.
 */
const guint8 *validity_capture_glow_start_cmd (gsize *out_len);

/*
 * Build the LED "glow end scan" command (sent via TLS app).
 * Returns a static buffer and sets out_len.
 */
const guint8 *validity_capture_glow_end_cmd (gsize *out_len);

/* ================================================================
 * CaptureProg table lookup
 * ================================================================ */

/*
 * Look up the capture program for a given ROM version and sensor type.
 * Returns a pointer to the static blob data and sets out_len.
 * Returns NULL if no matching entry is found.
 */
const guint8 *validity_capture_prog_lookup (guint8  rom_major,
                                            guint8  rom_minor,
                                            guint16 dev_type,
                                            gsize  *out_len);

/* ================================================================
 * Capture state lifecycle
 * ================================================================ */

void validity_capture_state_init (ValidityCaptureState *state);
void validity_capture_state_clear (ValidityCaptureState *state);

/*
 * Initialize capture state from sensor info and factory bits.
 * Loads the CaptureProg, computes geometry, sets calibration params.
 * Returns TRUE on success.
 */
gboolean validity_capture_state_setup (ValidityCaptureState         *state,
                                       const ValiditySensorTypeInfo *type_info,
                                       guint16                       dev_type,
                                       guint8                        rom_major,
                                       guint8                        rom_minor,
                                       const guint8                 *factory_bits,
                                       gsize                         factory_bits_len);

/* Chunk type IDs used in capture programs */
#define CAPT_CHUNK_REPLY_CONFIG 0x0017
#define CAPT_CHUNK_FINGER_DETECT 0x0026
#define CAPT_CHUNK_IMAGE_RECON 0x002e
#define CAPT_CHUNK_2D_PARAMS 0x002f
#define CAPT_CHUNK_LINE_UPDATE 0x0030
#define CAPT_CHUNK_TIMESLOT_2D 0x0034
#define CAPT_CHUNK_TS_OFFSET 0x0029
#define CAPT_CHUNK_TS_FD_OFFSET 0x0035
#define CAPT_CHUNK_LINE_UPDATE_XFORM 0x0043
#define CAPT_CHUNK_INTERLEAVE 0x0044
#define CAPT_CHUNK_WTF 0x004e

/* Capture program cookie for the ACM Config chunk */
#define CAPT_CHUNK_ACM_CONFIG 0x002a
#define CAPT_CHUNK_CEM_CONFIG 0x002c
