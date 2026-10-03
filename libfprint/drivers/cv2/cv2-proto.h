/*
 * Broadcom ControlVault2 (BCM5880) fingerprint protocol
 *
 * Copyright (C) 2026 Matt Trent <matt@thetrents.org>
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

/* Message header. All fields are little-endian. */
#define CV2_HEADER_LEN 0x2c
#define CV2_HDR_MAGIC 0x00       /* u32, always 1 */
#define CV2_HDR_LEN 0x04         /* u32, the whole message */
#define CV2_HDR_CMD 0x08         /* u16 */
#define CV2_HDR_FLAGS 0x0a       /* u16 */
#define CV2_HDR_UNKNOWN 0x0c     /* u32, 2 in every message seen */
#define CV2_HDR_SESSION 0x10     /* u32 */
#define CV2_HDR_STATUS 0x14      /* u32, replies only */
#define CV2_HDR_PARAMS_LEN 0x28  /* u32, the message after the header */
#define CV2_MAGIC 1

/* Each parameter is a u32 kind and a u32 length, then the data padded to
 * 4 bytes */
#define CV2_PARAM_HEADER_LEN 8

#define CV2_CAPTURE_ID_LEN 20
/* Bulk IN buffer size; the stock driver uses the same */
#define CV2_REPLY_BUF_LEN 4196

/* Interrupt messages: a u32 type, then a u32 value */
#define CV2_INT_BUF_LEN 32
#define CV2_INT_EVENT_LEN 8

/* First firmware (USH_REL_UPGRADE_VER) whose enrollment commit works */
#define CV2_FW_MIN_ENROLL 0x00412015

typedef enum {
  CV2_PARAM_VALUE  = 0,
  CV2_PARAM_STRING = 1,
  CV2_PARAM_BLOCK  = 2,
  CV2_PARAM_BUFFER = 3,
} Cv2ParamKind;

typedef enum {
  CV2_CMD_OPEN_SESSION   = 0x02,
  CV2_CMD_CLOSE_SESSION  = 0x04,
  CV2_CMD_DELETE         = 0x0a,
  CV2_CMD_MATCH          = 0x2f,
  CV2_CMD_GET_VERSION    = 0x39,
  CV2_CMD_CAPTURE_START  = 0x66,
  CV2_CMD_CAPTURE_CANCEL = 0x68,
  CV2_CMD_ENROLL_SAMPLE  = 0x6c,
  CV2_CMD_ENROLL_DISCARD = 0x6d,
  CV2_CMD_ENROLL_COMMIT  = 0x6e,
  CV2_CMD_SENSOR_RESET   = 0x82,
  CV2_CMD_ENROLL_BEGIN   = 0x8a,
} Cv2Command;

typedef enum {
  CV2_STATUS_OK              = 0x00,
  CV2_STATUS_NO_TEMPLATE     = 0x1b,
  CV2_STATUS_FW_TOO_OLD      = 0x24,
  CV2_STATUS_NO_TEMPLATES    = 0x47,
  CV2_STATUS_SAMPLE_REJECTED = 0x59,
  CV2_STATUS_CAPTURE_PENDING = 0x85,
  CV2_STATUS_NO_CAPTURE      = 0x89,
} Cv2Status;

typedef enum {
  CV2_OUTCOME_OK,              /* status 0 */
  CV2_OUTCOME_NO_MATCH,        /* match with no templates: treat as no match */
  CV2_OUTCOME_RETRY,           /* no usable sample; error is a retry error */
  CV2_OUTCOME_CAPTURE_PENDING, /* cancel the capture and retry once; error is BUSY */
  CV2_OUTCOME_ERROR,           /* error is set */
} Cv2Outcome;

typedef enum {
  CV2_INT_REPLY  = 0,  /* value: length of the reply waiting on bulk IN */
  CV2_INT_FINGER = 3,  /* value: nonzero if the chip got no usable capture */
} Cv2IntType;

typedef struct
{
  guint32 type;
  guint32 value;
} Cv2IntEvent;

typedef enum {
  CV2_FW_UNKNOWN,
  CV2_FW_TOO_OLD,
  CV2_FW_OK,
} Cv2FwSupport;

typedef struct
{
  guint32       kind;
  guint32       len;
  const guint8 *data;  /* points into Cv2Reply.raw */
} Cv2Param;

typedef struct
{
  guint16 cmd;
  guint16 flags;
  guint32 session;
  guint32 status;
  GBytes *raw;      /* the message, header total length bytes */
  GArray *params;   /* of Cv2Param */
} Cv2Reply;

/* Requests. Each returns a complete message owned by the caller. */
GByteArray *cv2_build_simple (Cv2Command cmd);
GByteArray *cv2_build_open_session (void);
GByteArray *cv2_build_close_session (guint32 session);
GByteArray *cv2_build_capture_start (guint32 session);
GByteArray *cv2_build_enroll_sample (guint32       session,
                                     const guint8 *capture_id);
GByteArray *cv2_build_enroll_commit (guint32       session,
                                     const guint8 *result_id);
GByteArray *cv2_build_match (guint32        session,
                             const guint32 *templates,
                             guint          n_templates);
GByteArray *cv2_build_delete (guint32 session,
                              guint32 template_id);

/* Replies and interrupt messages */
gboolean cv2_int_event_parse (const guint8 *buf,
                              gsize         len,
                              Cv2IntEvent  *event,
                              GError      **error);
gboolean cv2_message_peek_cmd (const guint8 *buf,
                               gsize         len,
                               guint16      *cmd);
Cv2Reply *cv2_reply_parse (const guint8 *buf,
                           gsize         len,
                           guint16       expected_cmd,
                           GError      **error);
void cv2_reply_free (Cv2Reply *reply);
gboolean cv2_reply_get_session (const Cv2Reply *reply,
                                guint32        *session,
                                GError        **error);
gboolean cv2_reply_get_capture_id (const Cv2Reply *reply,
                                   guint8         *capture_id,
                                   GError        **error);
gboolean cv2_reply_get_sample (const Cv2Reply *reply,
                               gboolean       *done,
                               guint8         *result_id,
                               GError        **error);
gboolean cv2_reply_get_template (const Cv2Reply *reply,
                                 guint32        *template_id,
                                 GError        **error);
gboolean cv2_reply_get_match (const Cv2Reply *reply,
                              gboolean       *matched,
                              guint32        *template_id,
                              GError        **error);
gchar *cv2_reply_get_version_text (const Cv2Reply *reply,
                                   GError        **error);

/* Status codes and firmware */
Cv2Outcome cv2_status_classify (guint16  cmd,
                                guint32  status,
                                GError **error);
gchar *cv2_version_lookup (const gchar *text,
                           const gchar *key);
Cv2FwSupport cv2_firmware_enroll_support (const gchar *upgrade_ver);
GError *cv2_firmware_error (const gchar *upgrade_ver);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Cv2Reply, cv2_reply_free)
