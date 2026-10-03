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


#define FP_COMPONENT "cv2"

#include <string.h>

#include "drivers_api.h"
#include "fpi-byte-utils.h"

#include "cv2-proto.h"

/* Constants the stock driver sends; their meaning is unknown */
#define CV2_OPEN_ARG 0x44
#define CV2_CAPTURE_ARG 2
#define CV2_CAPTURE_MODE_SAMPLE 0x23
#define CV2_MATCH_ARG1 0x48
#define CV2_MATCH_ARG2 0x53e2

/* Padding after a parameter's data */
#define CV2_PAD(len) ((4 - (len) % 4) % 4)

/* Header flags, as the stock driver sends them; their meaning is unknown */
static guint16
cmd_flags (Cv2Command cmd)
{
  switch (cmd)
    {
    case CV2_CMD_CAPTURE_START:
    case CV2_CMD_MATCH:
    case CV2_CMD_DELETE:
      return 0x0442;

    case CV2_CMD_OPEN_SESSION:
    case CV2_CMD_CLOSE_SESSION:
    case CV2_CMD_GET_VERSION:
    case CV2_CMD_CAPTURE_CANCEL:
    case CV2_CMD_ENROLL_SAMPLE:
    case CV2_CMD_ENROLL_DISCARD:
    case CV2_CMD_ENROLL_COMMIT:
    case CV2_CMD_ENROLL_BEGIN:
      return 0x0440;
    }

  g_assert_not_reached ();
}

static GByteArray *
msg_new (Cv2Command cmd, guint32 session)
{
  GByteArray *msg = g_byte_array_sized_new (128);

  g_byte_array_set_size (msg, CV2_HEADER_LEN);
  memset (msg->data, 0, CV2_HEADER_LEN);
  FP_WRITE_UINT32_LE (msg->data + CV2_HDR_MAGIC, CV2_MAGIC);
  FP_WRITE_UINT16_LE (msg->data + CV2_HDR_CMD, cmd);
  FP_WRITE_UINT16_LE (msg->data + CV2_HDR_FLAGS, cmd_flags (cmd));
  FP_WRITE_UINT32_LE (msg->data + CV2_HDR_UNKNOWN, 2);
  FP_WRITE_UINT32_LE (msg->data + CV2_HDR_SESSION, session);

  return msg;
}

/* The declared length can differ from the bytes sent: the open request
 * declares 7 for "myAppID" but sends the NUL too. */
static void
msg_add_param (GByteArray *msg,
               guint32     kind,
               guint32     declared_len,
               const void *data,
               gsize       data_len)
{
  static const guint8 zeros[4] = { 0 };
  guint8 hdr[CV2_PARAM_HEADER_LEN];

  FP_WRITE_UINT32_LE (hdr, kind);
  FP_WRITE_UINT32_LE (hdr + 4, declared_len);
  g_byte_array_append (msg, hdr, sizeof (hdr));

  if (data_len > 0)
    {
      g_byte_array_append (msg, data, data_len);
      g_byte_array_append (msg, zeros, CV2_PAD (data_len));
    }
}

static void
msg_add_u32 (GByteArray *msg, guint32 value)
{
  guint8 buf[4];

  FP_WRITE_UINT32_LE (buf, value);
  msg_add_param (msg, CV2_PARAM_VALUE, sizeof (buf), buf, sizeof (buf));
}

static GByteArray *
msg_finish (GByteArray *msg)
{
  FP_WRITE_UINT32_LE (msg->data + CV2_HDR_LEN, msg->len);
  FP_WRITE_UINT32_LE (msg->data + CV2_HDR_PARAMS_LEN, msg->len - CV2_HEADER_LEN);
  return msg;
}

/* A command whose only parameter is the value 0: get version, enroll begin
 * and discard, capture cancel. */
GByteArray *
cv2_build_simple (Cv2Command cmd)
{
  GByteArray *msg = msg_new (cmd, 0);

  msg_add_u32 (msg, 0);
  return msg_finish (msg);
}

GByteArray *
cv2_build_open_session (void)
{
  GByteArray *msg = msg_new (CV2_CMD_OPEN_SESSION, 0);

  msg_add_u32 (msg, CV2_OPEN_ARG);
  msg_add_param (msg, CV2_PARAM_STRING, 7, "myAppID", 8);
  msg_add_param (msg, CV2_PARAM_STRING, 8, "myUserID", 8);
  msg_add_param (msg, CV2_PARAM_STRING, 0, NULL, 0);
  return msg_finish (msg);
}

GByteArray *
cv2_build_close_session (guint32 session)
{
  GByteArray *msg = msg_new (CV2_CMD_CLOSE_SESSION, 0);

  msg_add_u32 (msg, session);
  return msg_finish (msg);
}

GByteArray *
cv2_build_capture_start (guint32 session)
{
  GByteArray *msg = msg_new (CV2_CMD_CAPTURE_START, session);

  msg_add_u32 (msg, session);
  msg_add_u32 (msg, CV2_CAPTURE_ARG);
  msg_add_u32 (msg, CV2_CAPTURE_MODE_SAMPLE);
  return msg_finish (msg);
}

GByteArray *
cv2_build_enroll_sample (guint32 session, const guint8 *capture_id)
{
  GByteArray *msg = msg_new (CV2_CMD_ENROLL_SAMPLE, session);

  msg_add_u32 (msg, session);
  msg_add_param (msg, CV2_PARAM_VALUE, CV2_CAPTURE_ID_LEN, capture_id, CV2_CAPTURE_ID_LEN);
  msg_add_param (msg, CV2_PARAM_BLOCK, 0, NULL, 0);
  return msg_finish (msg);
}

GByteArray *
cv2_build_enroll_commit (guint32 session, const guint8 *result_id)
{
  GByteArray *msg = msg_new (CV2_CMD_ENROLL_COMMIT, session);

  msg_add_u32 (msg, session);
  msg_add_param (msg, CV2_PARAM_VALUE, CV2_CAPTURE_ID_LEN, result_id, CV2_CAPTURE_ID_LEN);
  msg_add_param (msg, CV2_PARAM_BLOCK, 0, NULL, 0);
  msg_add_param (msg, CV2_PARAM_BLOCK, 0, NULL, 0);
  msg_add_param (msg, CV2_PARAM_BUFFER, 0, NULL, 0);
  return msg_finish (msg);
}

GByteArray *
cv2_build_match (guint32 session, const guint32 *templates, guint n_templates)
{
  GByteArray *msg = msg_new (CV2_CMD_MATCH, session);
  g_autofree guint8 *list = g_malloc0 (MAX (n_templates, 1) * 4);

  for (guint i = 0; i < n_templates; i++)
    FP_WRITE_UINT32_LE (list + 4 * i, templates[i]);

  msg_add_u32 (msg, session);
  msg_add_u32 (msg, CV2_MATCH_ARG1);
  msg_add_u32 (msg, CV2_MATCH_ARG2);
  msg_add_u32 (msg, 0);
  msg_add_param (msg, CV2_PARAM_BLOCK, n_templates * 4, list, n_templates * 4);
  return msg_finish (msg);
}

GByteArray *
cv2_build_delete (guint32 session, guint32 template_id)
{
  GByteArray *msg = msg_new (CV2_CMD_DELETE, session);

  msg_add_u32 (msg, session);
  msg_add_u32 (msg, template_id);
  msg_add_param (msg, CV2_PARAM_BLOCK, 0, NULL, 0);
  return msg_finish (msg);
}
