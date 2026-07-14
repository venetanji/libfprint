/*
 * Broadcom ControlVault message builders and parsers for libfprint
 *
 * Copyright (C) 2026 Erik Håkansson <erikhakan@gmail.com>
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

#include "fpi-byte-writer.h"
#include "fpi-log.h"
#include "broadcom_msg.h"

/*
 * TLV appender helpers
 */
static void append_tlv_header (FpiByteWriter *w,
                               guint32        total_len,
                               guint8         cmd_id,
                               guint16        msg_type,
                               guint32        session_id,
                               guint32        data_len);
static void append_tlv_u32 (FpiByteWriter *w,
                            guint32        tag,
                            guint32        val);
static void append_tlv_u32_array (FpiByteWriter *w,
                                  guint32        tag,
                                  const guint32 *values,
                                  guint32        n_values);
static void append_tlv_byte_blob (FpiByteWriter *w,
                                  guint32        tag,
                                  const guint8  *data,
                                  guint32        len);

/*
 * Message parser helpers
 */
static guint32 extract_u32_le (const guint8 *buf,
                               gsize         offset);
static gboolean verify_tlv_and_get_u32 (const guint8 *buf,
                                        gsize         offset,
                                        guint32       expected_tag,
                                        guint32       expected_length,
                                        guint32      *out,
                                        gsize        *next_offset);
static gboolean verify_tlv_and_copy_blob (const guint8 *buf,
                                          gsize         offset,
                                          guint32       expected_tag,
                                          guint32       expected_length,
                                          guint8       *out,
                                          gsize        *next_offset);
static guint32 parse_version_field (const guint8 *data,
                                    gsize         length,
                                    const gchar  *field_key);

/**
 * Message builders
 **/

/*
 * GET_VERSION
 * 56 bytes
 *
 * Probes the device for current version information.
 * No session required.
 * TLV: [tag=0, len=4, val=0].
 */
guint8 *
bcm_build_get_version (gsize *out_length)
{
  g_auto(FpiByteWriter) w = {0};

  fpi_byte_writer_init_with_size (&w, 56, TRUE);
  append_tlv_header (&w, 56, BCM_CMD_GET_VERSION.id, BCM_MSG_SIMPLE_REQ, 0, 12);
  append_tlv_u32 (&w, 0, 0);

  *out_length = 56;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * OPEN
 * 96 bytes
 *
 * Opens session with the device
 *
 * TLV: [tag=0, len=4, credential_type]
 *      [tag=1, len=7, username]
 *      [tag=1, len=8, password]
 *      [tag=1, len=0, terminator]
 */
guint8 *
bcm_build_open (gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_OPEN;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 96;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_OPEN.id, BCM_MSG_SIMPLE_REQ, 0, 52);
  append_tlv_u32 (&w, 0, BCM_CREDENTIAL_TYPE);
  //myAppId and myUserId are hardcoded
  append_tlv_byte_blob (&w, 1, (const guint8 *) "myAppID", 7);
  append_tlv_byte_blob (&w, 1, (const guint8 *) "myUserID", 8);
  append_tlv_u32_array (&w, 1, NULL, 0); //Empty array => len==0

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * CLOSE
 * 56 bytes
 *
 * Closes device session
 * TLV: [tag=0, len=4, session_id]
 */
guint8 *
bcm_build_close (const guint32 session_id, gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_CLOSE;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 56;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_CLOSE.id, BCM_MSG_SIMPLE_REQ, 0, 12);
  append_tlv_u32 (&w, 0, session_id);

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * DELETE_OBJECT
 * 76 bytes
 *
 * Deletes a fingerprint template from device NVRAM by template_id.
 * Status 0x1b (NOT_FOUND) is returned when template_id does not exist.
 *
 * TLV: [tag=0, len=4, session_id]
 *      [tag=0, len=4, template_id]
 *      [tag=2, len=0, scan_type]
 *
 * Sending an empty or 0x0 template_id is a no-op. Cannot be used to clear all templates.
 */
guint8 *
bcm_build_delete_object (const guint32 session_id, const guint32 template_id, gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_DELETE_OBJECT;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 76;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_DELETE_OBJECT.id, BCM_MSG_EXT_REQ, session_id, 32);
  append_tlv_u32 (&w, 0, session_id);
  append_tlv_u32 (&w, 0, template_id);
  append_tlv_u32_array (&w, 2, NULL, 0); //Empty array => len==0

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}


/*
 * START_ENROLLMENT
 * 56 bytes
 *
 * Starts a fingerprint enrollment
 * TLV: [tag=0, len=4, value=0]
 */
guint8 *
bcm_build_start_enroll (gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_START_ENROLL;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 56;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_START_ENROLL.id, BCM_MSG_SIMPLE_REQ, 0, 12);
  append_tlv_u32 (&w, 0, 0);

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}


/*
 * GET_CHALLENGE
 * 80 bytes
 *
 * Gets a challenge nonce to be used in next step
 *
 * TLV: [tag=0, len=4, session_id]
 *      [tag=0, len=4, purpose]
 *      [tag=0, len=4, hash_algorithm]
 */
guint8 *
bcm_build_get_challenge (const guint32 session_id, gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_GET_CHALLENGE;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 80;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_GET_CHALLENGE.id, BCM_MSG_EXT_REQ, session_id, 36);
  append_tlv_u32 (&w, 0, session_id);
  append_tlv_u32 (&w, 0, 2); // purpose
  append_tlv_u32 (&w, 0, 0x23); // hash_algo = SHA-1

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * ENROLL_UPDATE
 * 92 bytes
 *
 * Send an update to the enrollment process
 *
 * TLV: [tag=0, len=4, session_id]
 *      [tag=0, len=BCM_HASH_SIZE, hash]
 *      [tag=2, len=0, scan_type]
 */
guint8 *
bcm_build_enroll_update (const guint32 session_id, const guint8 *hash, gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_ENROLL_UPDATE;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 92;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_ENROLL_UPDATE.id, BCM_MSG_SIMPLE_REQ, session_id, 48);
  append_tlv_u32 (&w, 0, session_id);
  append_tlv_byte_blob (&w, 0, hash, BCM_HASH_SIZE);
  append_tlv_u32_array (&w, 2, NULL, 0); //scan_type. Empty array => len==0

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * ENROLL_COMMIT
 * 108 bytes
 *
 * Commits enrollment
 *
 * TLV: [tag=0, len=4, session_id]
 *      [tag=0, len=BCM_HASH_SIZE, hash]
 *      [tag=2, len=0, scan_type]
 *      [tag=2, len=0, (unknown)]
 *      [tag=3, len=0, commit_flag]
 */
guint8 *
bcm_build_enroll_commit (const guint32 session_id, const guint8 *hash, gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_ENROLL_COMMIT;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 108;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_ENROLL_COMMIT.id, BCM_MSG_SIMPLE_REQ, session_id, 64);
  append_tlv_u32 (&w, 0, session_id);
  append_tlv_byte_blob (&w, 0, hash, BCM_HASH_SIZE);
  append_tlv_u32_array (&w, 2, NULL, 0); //scan_type. Empty array => len==0
  append_tlv_u32_array (&w, 2, NULL, 0);
  append_tlv_u32_array (&w, 3, NULL, 0); //commit_flag

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * CAPTURE
 * Used to verify a single template id or identify multiple ids.
 * variable length
 * Packet size: 100 + N*4 bytes
 * data_len:     56 + N*4 bytes
 *
 * Same as single-template CAPTURE/verify but TLV[4] carries all N template IDs:
 * [tag=2, len=N*4, template_id_0 ... template_id_N-1]
 *
 * The device performs on-device 1:N matching and returns the matched
 * template_id (or 0) along with match_result.  Parsed by the same
 * bcm_parse_capture_response() as single-template verify.
 *
 * TLV: [tag=0, len=4, session_id]
 *      [tag=0, len=4, BCM_CAPTURE_MODE]
 *      [tag=0, len=4, BCM_CAPTURE_QUALITY]
 *      [tag=0, len=4, 0] unknown
 *      [tag=2, len=N*4, template_ids]
 */
guint8 *
bcm_build_capture (const guint32 session_id, const guint32 *template_ids, const guint n_templates,
                   gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_CAPTURE;
  const guint32 data_length = 56 + (n_templates * 4);
  const gsize packet_length = 100 + (n_templates * 4);
  g_auto(FpiByteWriter) w = {0};

  fpi_byte_writer_init_with_size (&w, packet_length, TRUE);
  append_tlv_header (&w, (guint32) packet_length, BCM_CMD_CAPTURE.id,
                     BCM_MSG_EXT_REQ, session_id, data_length);
  append_tlv_u32 (&w, 0, session_id);
  append_tlv_u32 (&w, 0, BCM_CAPTURE_MODE);
  append_tlv_u32 (&w, 0, BCM_CAPTURE_QUALITY);
  append_tlv_u32 (&w, 0, 0); // reserved

  /* TLV[4]: tag=2, len=N*4, followed by N template IDs as u32 LE */
  append_tlv_u32_array (&w, 2, template_ids, n_templates);

  *out_length = packet_length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/*
 * CANCEL_CAPTURE
 * 56 bytes
 *
 * TLV: [tag=0, len=4, value=0]
 */
guint8 *
bcm_build_cancel_capture (gsize *out_length, BcmCommand *out_command)
{
  *out_command = BCM_CMD_CANCEL_CAPTURE;
  g_auto(FpiByteWriter) w = {0};

  const gint length = 56;
  fpi_byte_writer_init_with_size (&w, length, TRUE);
  append_tlv_header (&w, length, BCM_CMD_CANCEL_CAPTURE.id, BCM_MSG_SIMPLE_REQ, 0, 12);
  append_tlv_u32 (&w, 0, 0);

  *out_length = length;
  return fpi_byte_writer_reset_and_get_data (&w);
}

/** Incoming message parsers **/

/*
 * Parse 8-byte interrupt.
 *
 * byte 0-3: u32 status
 * byte 4-7: u32 response_length
 *
 * status=0x00: response ready on bulk IN, read response_len bytes
 * status=0x03: finger event, response_len should be 0
 */
gboolean
bcm_parse_interrupt (const guint8 *data, const gsize length, BcmInterrupt *out)
{
  if (length < BCM_INTERRUPT_SIZE)
    {
      fp_warn ("Interrupt packet too short: got %zu bytes, expected at least %u", length, BCM_INTERRUPT_SIZE);
      return FALSE;
    }

  out->status = extract_u32_le (data, 0);
  out->response_length = extract_u32_le (data, 4);
  return TRUE;
}

/*
 * Parse the 36-byte message header.
 *
 * Header format:
 *   0x00 (0)  u32 version
 *   0x04 (4)  u32 total_length
 *   0x08 (8)  u8  command_id
 *   0x09 (9)  u8  reserved
 *   0x0a (10) u16 message_type
 *   0x0c (12) u32 param
 *   0x10 (16) u32 session_id
 *   0x14 (20) u32 status
 *   0x18 (24) u8  reserved[12]
 */
gboolean
bcm_parse_header (const guint8 *data, const gsize length, BcmMessageHeader *out)
{
  if (length < BCM_MESSAGE_HEADER_SIZE)
    {
      fp_warn ("Message header too short: got %zu bytes, expected at least %u", length, BCM_MESSAGE_HEADER_SIZE);
      return FALSE;
    }

  out->version = extract_u32_le (data, 0);
  out->total_length = extract_u32_le (data, 4);
  out->command_id = data[8];
  guint16 message_type;
  memcpy (&message_type, data + 10, 2);
  out->message_type = GUINT16_FROM_LE (message_type);
  out->param = extract_u32_le (data, 12);
  out->session_id = extract_u32_le (data, 16);
  out->status = extract_u32_le (data, 20);

  return TRUE;
}

/*
 * Parse GET_VERSION response
 * variable size
 *
 * Note: The length of the TLV might be variable in different versions of the firmware, or it might be fixed length at 876. We treat it as variable to be safe.
 *
 * The response data area contains "USH_REL_UPGRADE_VER:" (firmware version) and "USH_SBI_MINOR_VER:" (secondary boot image version) and parse the 8-digit decimal string that follows
 * (e.g. "00515010" which represents the first three components of version 5.15.010.0)
 *
 * The zero-padded format means higher versions always produce larger
 * integers, so a simple numeric comparison works for version ordering.
 *
 * TLV: [tag=1, len=variable, version_data]
 */
gboolean
bcm_parse_version_response (const guint8 *data, const gsize length, guint32 *fw_version, guint32 *sbi_version)
{
  if (length < BCM_MESSAGE_DATA_OFFSET + 8)
    {
      fp_dbg ("GET_VERSION response too short: %zu bytes", length);
      return FALSE;
    }

  const guint32 version_blob_length = extract_u32_le (data, BCM_MESSAGE_DATA_OFFSET + 4);
  const gsize version_blob_total_length = (gsize) version_blob_length + BCM_MESSAGE_DATA_OFFSET + 8;
  if (version_blob_total_length > length)
    {
      fp_dbg ("GET_VERSION response truncated: blob length %u exceeds packet size %zu", version_blob_length, length);
      return FALSE;
    }

  g_autofree guint8 *version_blob = NULL;
  version_blob = g_malloc (version_blob_length + 1);
  if (!verify_tlv_and_copy_blob (data, BCM_MESSAGE_DATA_OFFSET, 1, version_blob_length, version_blob, NULL))
    {
      fp_dbg ("GET_VERSION: malformed version blob TLV");
      return FALSE;
    }
  version_blob[version_blob_length] = '\0';
  fp_dbg ("GET_VERSION response:\n%s", version_blob);

  //Application image firmware version
  *fw_version = parse_version_field (version_blob, version_blob_length, "USH_REL_UPGRADE_VER:");

  if (*fw_version == 0)
    {
      fp_dbg ("GET_VERSION: could not parse AAI version");
      return FALSE;
    }
  fp_dbg ("GET_VERSION: FW version %u", *fw_version);

  //Secondary boot image version
  *sbi_version = parse_version_field (version_blob, version_blob_length, "USH_SBI_MINOR_VER:");
  fp_dbg ("GET_VERSION: SBI version %u%s", *sbi_version, *sbi_version == 0 ? " (unknown)" : "");

  return TRUE;
}

/*
 * Validate OPEN response
 * 56 bytes
 *
 * The response carries session_id twice:
 * - in the header at offset 16
 * - in TLV[0] as tag=0, len=4, value=session_id
 *
 * Both are required to match.
 * TLV: [tag=0, len=4, session_id]
 *
 */
gboolean
bcm_validate_open_response (const guint8 *data, const gsize length, const BcmMessageHeader *header)
{
  guint32 echoed_session_id;

  if (length != 56)
    {
      fp_warn ("Malformed OPEN response: got %zu bytes, expected 56", length);
      return FALSE;
    }
  if (!verify_tlv_and_get_u32 (data, BCM_MESSAGE_DATA_OFFSET, 0, 4, &echoed_session_id, NULL))
    {
      fp_warn ("%s", "Malformed OPEN response: missing session-id TLV");
      return FALSE;
    }
  if (header->session_id != echoed_session_id)
    {
      fp_warn ("Malformed OPEN response: header session_id 0x%08x does not match TLV session_id 0x%08x",
               header->session_id, echoed_session_id);
      return FALSE;
    }
  return TRUE;
}

/*
 * Parse GET_CHALLENGE response
 * 72 bytes
 *
 * extract 20-byte SHA-1 hash.
 * TLV: [tag=0, len=20, hash]
 */
gboolean
bcm_parse_challenge_response (const guint8 *data, const gsize length, guint8 *hash_out)
{
  if (length != 72)
    {
      fp_warn ("Malformed GET_CHALLENGE response: got %zu bytes, expected 72", length);
      return FALSE;
    }

  if (!verify_tlv_and_copy_blob (data, BCM_MESSAGE_DATA_OFFSET, 0, BCM_HASH_SIZE, hash_out, NULL))
    {
      fp_warn ("Malformed GET_CHALLENGE response: missing hash TLV");
      return FALSE;
    }
  return TRUE;
}

/*
 * Parse ENROLL_UPDATE response.
 *
 * Success (96 bytes):
 * TLV: [tag=0][len=4 ][progress_state]
 *      [tag=0][len=20][hash:20]
 *      [tag=0][len=4 ][0]
 *
 * Note: Retry flow (44 bytes, header status 0xa4, 0x80) should never enter here
 */
gboolean
bcm_parse_enroll_update_response (const guint8 *data, const gsize length, guint32 *progress_state, guint8 *hash_out)
{
  if (length != 96)
    {
      fp_warn ("Malformed ENROLL_UPDATE response: got %zu bytes, expected 96", length);
      return FALSE;
    }

  gsize next_offset;
  if (!verify_tlv_and_get_u32 (data, BCM_MESSAGE_DATA_OFFSET, 0, 4, progress_state, &next_offset))
    {
      fp_warn ("Malformed ENROLL_UPDATE response: missing progress-state TLV");
      return FALSE;
    }
  if (!verify_tlv_and_copy_blob (data, next_offset, 0, BCM_HASH_SIZE, hash_out, NULL))
    {
      fp_warn ("Malformed ENROLL_UPDATE response: missing hash TLV");
      return FALSE;
    }
  return TRUE;
}

/*
 * Parse ENROLL_COMMIT response
 * extract the new template_id.
 *
 * Response: 64 bytes.
 * TLV: [tag=3][len=0]
 *      [tag=0][len=4][template_id]
 */
gboolean
bcm_parse_enroll_commit_response (const guint8 *data, const gsize length, guint32 *template_id)
{
  if (length != 64)
    {
      fp_warn ("Malformed ENROLL_COMMIT response: got %zu bytes, expected 64", length);
      return FALSE;
    }
  gsize next_offset;
  if (!verify_tlv_and_get_u32 (data, BCM_MESSAGE_DATA_OFFSET, 3, 0, NULL, &next_offset))
    {
      fp_warn ("Malformed ENROLL_COMMIT response: missing status TLV");
      return FALSE;
    }
  if (!verify_tlv_and_get_u32 (data, next_offset, 0, 4, template_id, NULL))
    {
      fp_warn ("Malformed ENROLL_COMMIT response: missing template-id TLV");
      return FALSE;
    }
  return TRUE;
}

/*
 * Parse CAPTURE response
 * extract match result and template_id.
 *
 * Response: 68 bytes.

 * TLV: [tag=0][len=4][match_result]
 *      [tag=0][len=4][template_id]
 */
gboolean
bcm_parse_capture_response (const guint8 *data, const gsize length, guint32 *match_result, guint32 *template_id)
{
  if (length != 68)
    {
      fp_warn ("Malformed CAPTURE response: got %zu bytes, expected 68", length);
      return FALSE;
    }

  gsize next_offset;
  if (!verify_tlv_and_get_u32 (data, BCM_MESSAGE_DATA_OFFSET, 0, 4, match_result, &next_offset))
    {
      fp_warn ("Malformed CAPTURE response: missing match-result TLV");
      return FALSE;
    }
  if (!verify_tlv_and_get_u32 (data, next_offset, 0, 4, template_id, NULL))
    {
      fp_warn ("Malformed CAPTURE response: missing template-id TLV");
      return FALSE;
    }
  return TRUE;
}

/**
 * Helpers
 **/

/*
 * Write the 36-byte message header into w, then the data-area prefix [pad=0][data_len]. This leaves the writer positioned at offset 44 (first TLV), ready for TLV entries.
 *
 * Header format:
 *   0x00 (0)  u32 version
 *   0x04 (4)  u32 total_length
 *   0x08 (8)  u8  command_id
 *   0x09 (9)  u8  reserved
 *   0x0a (10) u16 message_type
 *   0x0c (12) u32 param
 *   0x10 (16) u32 session_id
 *   0x14 (20) u32 status
 *   0x18 (24) u8  reserved[12]
 *   0x24 (36) u32 data pad
 *   0x28 (40) u32 data_length
 *
 * The buffer must already be initialized (zero-filled) before calling.
 */
static void
append_tlv_header (FpiByteWriter *w, const guint32 total_len, const guint8 cmd_id, const guint16 msg_type,
                   const guint32 session_id, const guint32 data_len)
{
  gboolean ok = TRUE;

  ok &= fpi_byte_writer_put_uint32_le (w, BCM_MSG_VERSION); // 0  version
  ok &= fpi_byte_writer_put_uint32_le (w, total_len);       // 4  total_len
  ok &= fpi_byte_writer_put_uint8 (w, cmd_id);              // 8  cmd_id
  ok &= fpi_byte_writer_put_uint8 (w, 0);                   // 9  reserved
  ok &= fpi_byte_writer_put_uint16_le (w, msg_type);        // 10 msg_type
  ok &= fpi_byte_writer_put_uint32_le (w, BCM_MSG_PARAM);   // 12 param
  ok &= fpi_byte_writer_put_uint32_le (w, session_id);      // 16 session
  ok &= fpi_byte_writer_fill (w, 0, 16);                    // 20 status + reserved[12]
  ok &= fpi_byte_writer_put_uint32_le (w, 0);               // 36 data pad
  ok &= fpi_byte_writer_put_uint32_le (w, data_len);        // 40 data_len
  // The writer is now at offset 44, the first TLV.

  g_assert (ok);
}

static void
append_tlv_u32 (FpiByteWriter *w, const guint32 tag, const guint32 val)
{
  append_tlv_u32_array (w, tag, &val, 1);
}

static void
append_tlv_u32_array (FpiByteWriter *w, const guint32 tag, const guint32 *values, const guint32 n_values)
{
  gboolean ok = TRUE;

  const guint32 len = n_values * 4;

  ok &= fpi_byte_writer_put_uint32_le (w, tag);
  ok &= fpi_byte_writer_put_uint32_le (w, len);

  for (guint i = 0; i < n_values; i++)
    ok &= fpi_byte_writer_put_uint32_le (w, values[i]);
  g_assert (ok);
}

static void
append_tlv_byte_blob (FpiByteWriter *w, const guint32 tag, const guint8 *data, const guint32 len)
{
  const guint32 pad = (4 - (len & 3)) & 3;
  gboolean ok = TRUE;

  ok &= fpi_byte_writer_put_uint32_le (w, tag);
  ok &= fpi_byte_writer_put_uint32_le (w, len);
  ok &= fpi_byte_writer_put_data (w, data, len);
  if (pad)
    ok &= fpi_byte_writer_fill (w, 0, pad);
  g_assert (ok);
}

/*
 * Read a u32 LE from an offset in a received buffer.
 */
static guint32
extract_u32_le (const guint8 *buf, const gsize offset)
{
  guint32 le;

  memcpy (&le, buf + offset, 4);
  return GUINT32_FROM_LE (le);
}

/*
 * Verify a tlv format and return its u32 value
 */
static gboolean
verify_tlv_and_get_u32 (const guint8 *buf, const gsize offset, const guint32 expected_tag, const guint32 expected_length, guint32 *out, gsize *next_offset)
{
  const guint32 tag = extract_u32_le (buf, offset);
  const guint32 length = extract_u32_le (buf, offset + 4);

  if (tag != expected_tag || length != expected_length)
    {
      fp_dbg ("TLV mismatch at offset %zu: got tag=%u len=%u, expected tag=%u len=%u", offset, tag, length, expected_tag, expected_length);
      return FALSE;
    }
  if (out)
    {
      if (length != 4)
        {
          fp_dbg ("TLV at offset %zu has length %u, cannot decode as u32", offset, length);
          return FALSE;
        }
      guint32 le;
      memcpy (&le, buf + offset + 8, 4);
      *out = GUINT32_FROM_LE (le);
    }
  if (next_offset)
    *next_offset = offset + 8 + length;
  return TRUE;
}

/*
 * Verify a TLV format and copy its value as a blob
 */
static gboolean
verify_tlv_and_copy_blob (const guint8 *buf, const gsize offset, const guint32 expected_tag, const guint32 expected_length, guint8 *out, gsize *next_offset)
{
  const guint32 tag = extract_u32_le (buf, offset);
  const guint32 length = extract_u32_le (buf, offset + 4);

  if (tag != expected_tag || length != expected_length)
    {
      fp_dbg ("TLV mismatch at offset %zu: got tag=%u len=%u, expected tag=%u len=%u", offset, tag, length, expected_tag, expected_length);
      return FALSE;
    }
  memcpy (out, buf + offset + 8, length);
  if (next_offset)
    *next_offset = offset + 8 + length;
  return TRUE;
}

/*
 * Parse a decimal "key:value" field from the newline-delimited GET_VERSION blob.
 * Matches field_key only at line start and reads up to 8 following digits.
 * Returns 0 if the key is absent or malformed.
 */
static guint32
parse_version_field (const guint8 *data, const gsize length, const gchar *field_key)
{
  const gsize field_key_length = strlen (field_key);
  const guint8 *line_start = data;
  const guint8 *buffer_end = data + length;

  while (line_start < buffer_end)
    {
      const guint8 *newline = memchr (line_start, '\n', buffer_end - line_start);
      const guint8 *line_end = newline ? newline : buffer_end;

      if ((gsize) (line_end - line_start) >= field_key_length && memcmp (line_start, field_key, field_key_length) == 0)
        {
          gchar decimal_digits[9];
          gsize digit_count = 0;
          gchar *parse_end = NULL;
          guint64 parsed_value = 0;

          while (digit_count < 8 && line_start + field_key_length + digit_count < line_end && g_ascii_isdigit (line_start[field_key_length + digit_count]))
            {
              decimal_digits[digit_count] = (gchar) line_start[field_key_length + digit_count];
              digit_count++;
            }
          decimal_digits[digit_count] = '\0';

          if (digit_count == 0)
            {
              fp_dbg ("GET_VERSION: value for '%s' is not decimal", field_key);
              return 0;
            }

          parsed_value = g_ascii_strtoull (decimal_digits, &parse_end, 10);
          if (parse_end == decimal_digits)
            {
              fp_dbg ("GET_VERSION: value for '%s' is not decimal: '%s'",
                      field_key, decimal_digits);
              return 0;
            }

          return (guint32) parsed_value;
        }

      line_start = newline ? newline + 1 : buffer_end;
    }

  fp_dbg ("GET_VERSION: key '%s' not found", field_key);
  return 0;
}
