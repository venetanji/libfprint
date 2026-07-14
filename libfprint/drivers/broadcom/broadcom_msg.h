/*
 * Broadcom ControlVault protocol definitions for libfprint
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

#pragma once

/* The commands */
#define BCM_CMD_OPEN ((BcmCommand){ "OPEN",           0x02 })
#define BCM_CMD_CLOSE ((BcmCommand){ "CLOSE",          0x04 })
#define BCM_CMD_DELETE_OBJECT ((BcmCommand){ "DELETE_OBJECT",  0x0a })
#define BCM_CMD_GET_VERSION ((BcmCommand){ "GET_VERSION",    0x39 })
#define BCM_CMD_CAPTURE ((BcmCommand){ "CAPTURE",        0x2f })
#define BCM_CMD_GET_CHALLENGE ((BcmCommand){ "GET_CHALLENGE",  0x66 })
#define BCM_CMD_CANCEL_CAPTURE ((BcmCommand){ "CANCEL_CAPTURE", 0x68 })
#define BCM_CMD_ENROLL_UPDATE ((BcmCommand){ "ENROLL_UPDATE",  0x6c })
#define BCM_CMD_ENROLL_COMMIT ((BcmCommand){ "ENROLL_COMMIT",  0x6e })
#define BCM_CMD_START_ENROLL ((BcmCommand){ "START_ENROLL",   0x8a })

/* Header constants */
#define BCM_MSG_VERSION 1
#define BCM_MSG_PARAM 2

/* Credential type constant */
#define BCM_CREDENTIAL_TYPE 0x44

/* Message types (u16 LE at offset 10) */
#define BCM_MSG_SIMPLE_REQ 0x0040
#define BCM_MSG_EXT_REQ 0x0042
/* Response types are not used right now. They are included in responses and
 * could be validated against, but length + status + tlv validation is enough
 * for now. */
#define BCM_MSG_SIMPLE_RSP 0x0140
#define BCM_MSG_EXT_RSP 0x0142

/* Capture parameters */
#define BCM_CAPTURE_MODE 0x48
#define BCM_CAPTURE_QUALITY 0x53e2

/* Protocol constants */
#define BCM_MESSAGE_HEADER_SIZE 36
#define BCM_MESSAGE_DATA_OFFSET 44
#define BCM_HASH_SIZE 20             /* SHA-1 */
#define BCM_INTERRUPT_SIZE 8

/**
 * Outgoing message builders - each returns a g_malloc'd buffer.
 * Caller must g_free() the returned buffer.
 */
/* Command struct. Bundles the wire byte and human-readable name together for
 * logging purposes. */
typedef struct
{
  const gchar *name;
  guint8       id;
} BcmCommand;

guint8 *bcm_build_get_version (gsize *out_length);
guint8 *bcm_build_open (gsize      *out_length,
                        BcmCommand *out_command);
guint8 *bcm_build_close (guint32     session_id,
                         gsize      *out_length,
                         BcmCommand *out_command);
guint8 *bcm_build_delete_object (guint32     session_id,
                                 guint32     template_id,
                                 gsize      *out_length,
                                 BcmCommand *out_command);
guint8 *bcm_build_start_enroll (gsize      *out_length,
                                BcmCommand *out_command);
guint8 *bcm_build_get_challenge (guint32     session_id,
                                 gsize      *out_length,
                                 BcmCommand *out_command);
guint8 *bcm_build_enroll_update (guint32       session_id,
                                 const guint8 *hash,
                                 gsize        *out_length,
                                 BcmCommand   *out_command);
guint8 *bcm_build_enroll_commit (guint32       session_id,
                                 const guint8 *hash,
                                 gsize        *out_length,
                                 BcmCommand   *out_command);
guint8 *bcm_build_capture (guint32        session_id,
                           const guint32 *template_ids,
                           guint          n_templates,
                           gsize         *out_length,
                           BcmCommand    *out_command);
guint8 *bcm_build_cancel_capture (gsize      *out_length,
                                  BcmCommand *out_command);

/**
 * Incoming message parsers. Returns true on success
 */

/* Parsed message header */
typedef struct
{
  guint32 version;
  guint32 total_length;
  guint8  command_id;
  guint16 message_type;
  guint32 param;
  guint32 session_id;
  guint32 status;
} BcmMessageHeader;

/* Parsed interrupt notification */
typedef struct
{
  guint32 status;
  guint32 response_length;
} BcmInterrupt;

gboolean bcm_parse_header (const guint8     *data,
                           gsize             length,
                           BcmMessageHeader *out);
gboolean bcm_parse_interrupt (const guint8 *data,
                              gsize         length,
                              BcmInterrupt *out);
gboolean bcm_parse_version_response (const guint8 *data,
                                     gsize         length,
                                     guint32      *fw_version,
                                     guint32      *sbi_version);
gboolean bcm_validate_open_response (const guint8           *data,
                                     gsize                   length,
                                     const BcmMessageHeader *header);
gboolean bcm_parse_challenge_response (const guint8 *data,
                                       gsize         length,
                                       guint8       *hash_out);
gboolean bcm_parse_enroll_update_response (const guint8 *data,
                                           gsize         length,
                                           guint32      *progress_state,
                                           guint8       *hash_out);
gboolean bcm_parse_enroll_commit_response (const guint8 *data,
                                           gsize         length,
                                           guint32      *template_id);
gboolean bcm_parse_capture_response (const guint8 *data,
                                     gsize         length,
                                     guint32      *match_result,
                                     guint32      *template_id);
