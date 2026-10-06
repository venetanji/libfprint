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
#include "fpi-byte-reader.h"
#include "fpi-byte-utils.h"

#include "cv2-proto.h"

/* Constants the stock driver sends; their meaning is unknown */
#define CV2_OPEN_ARG 0x44
#define CV2_CAPTURE_ARG 2
#define CV2_CAPTURE_MODE_SAMPLE 0x23
#define CV2_MATCH_ARG1 0x48
#define CV2_MATCH_ARG2 0x53e2

/* The session's app and user IDs, as Dell's Linux drivers send them */
#define CV2_APP_ID "myAppID"
#define CV2_USER_ID "myUserID"

/* Enumerate: templates record the SHA-1 of the enrolling session's app ID
 * and user ID, as declared at open (without the app ID's NUL) */
#define CV2_OWNER_HASH_LEN 20
#define CV2_OBJECT_FINGERPRINT 7
/* Room for 256 handles */
#define CV2_ENUMERATE_BUF_LEN 1024

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

    case CV2_CMD_SENSOR_RESET:
      /* As the Windows driver sends it; the only form tested */
      return 0x0040;

    case CV2_CMD_OPEN_SESSION:
    case CV2_CMD_CLOSE_SESSION:
    case CV2_CMD_GET_VERSION:
    case CV2_CMD_CAPTURE_CANCEL:
    case CV2_CMD_ENROLL_SAMPLE:
    case CV2_CMD_ENROLL_DISCARD:
    case CV2_CMD_ENROLL_COMMIT:
    case CV2_CMD_ENROLL_BEGIN:
    case CV2_CMD_ENUMERATE:
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

/* A command whose only parameter is the value 0: get version, sensor reset,
 * enroll begin and discard, capture cancel. */
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
  msg_add_param (msg, CV2_PARAM_STRING, strlen (CV2_APP_ID), CV2_APP_ID, sizeof (CV2_APP_ID));
  msg_add_param (msg, CV2_PARAM_STRING, strlen (CV2_USER_ID), CV2_USER_ID, strlen (CV2_USER_ID));
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

/*
 * The commit's two blocks, and the delete's block, captured on USB from
 * Dell's Linux driver as patched to send the values Dell's Windows driver
 * uses (Windows sends them inside its protected session).
 *
 * Firmware 4.12.015 also accepts empty blocks, and the delete with this
 * authorization also removes templates committed with empty blocks. Older
 * firmware rejected a commit with empty blocks with status 0x24.
 */
static const guint8 commit_attributes[] = {
  0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
};

/* Contains the fixed text "BroadcomWBF" */
static const guint8 authorization[] = {
  0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x0c, 0x42, 0x72, 0x6f,
  0x61, 0x64, 0x63, 0x6f, 0x6d, 0x57, 0x42, 0x46, 0x00,
};

GByteArray *
cv2_build_enroll_commit (guint32 session, const guint8 *result_id)
{
  GByteArray *msg = msg_new (CV2_CMD_ENROLL_COMMIT, session);

  msg_add_u32 (msg, session);
  msg_add_param (msg, CV2_PARAM_VALUE, CV2_CAPTURE_ID_LEN, result_id, CV2_CAPTURE_ID_LEN);
  msg_add_param (msg, CV2_PARAM_BLOCK, sizeof (commit_attributes),
                 commit_attributes, sizeof (commit_attributes));
  msg_add_param (msg, CV2_PARAM_BLOCK, sizeof (authorization),
                 authorization, sizeof (authorization));
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
  msg_add_param (msg, CV2_PARAM_BLOCK, sizeof (authorization),
                 authorization, sizeof (authorization));
  return msg_finish (msg);
}

/*
 * Lists the fingerprint templates this driver's sessions own: those enrolled
 * here or by Dell's Linux driver (same app and user IDs), not those of other
 * systems, e.g. Windows Hello on the same laptop. The arguments are the byte
 * length of a list of owner hashes, the list (our one hash), the object type
 * and the size offered for the reply. The session is only in the header.
 *
 * The first argument must be that length: the chip walks its storage once
 * per hash, so a large value with nothing to list keeps it busy until it is
 * powered off.
 */
GByteArray *
cv2_build_enumerate (guint32 session)
{
  g_autoptr(GChecksum) sha1 = g_checksum_new (G_CHECKSUM_SHA1);
  guint8 owner_hash[CV2_OWNER_HASH_LEN];
  gsize hash_len = sizeof (owner_hash);
  GByteArray *msg = msg_new (CV2_CMD_ENUMERATE, session);

  g_checksum_update (sha1, (const guchar *) CV2_APP_ID, strlen (CV2_APP_ID));
  g_checksum_update (sha1, (const guchar *) CV2_USER_ID, strlen (CV2_USER_ID));
  g_checksum_get_digest (sha1, owner_hash, &hash_len);

  msg_add_u32 (msg, sizeof (owner_hash));
  msg_add_param (msg, CV2_PARAM_VALUE, sizeof (owner_hash), owner_hash, sizeof (owner_hash));
  msg_add_u32 (msg, CV2_OBJECT_FINGERPRINT);
  msg_add_param (msg, CV2_PARAM_BUFFER, CV2_ENUMERATE_BUF_LEN, NULL, 0);
  return msg_finish (msg);
}

#define set_proto_error(error, ...) \
  g_propagate_error ((error), fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, __VA_ARGS__))

gboolean
cv2_int_event_parse (const guint8 *buf,
                     gsize         len,
                     Cv2IntEvent  *event,
                     GError      **error)
{
  if (len < CV2_INT_EVENT_LEN)
    {
      set_proto_error (error, "Short interrupt message (%" G_GSIZE_FORMAT " bytes)", len);
      return FALSE;
    }

  event->type = FP_READ_UINT32_LE (buf);
  event->value = FP_READ_UINT32_LE (buf + 4);

  if (event->type == CV2_INT_REPLY &&
      (event->value < CV2_HEADER_LEN || event->value > CV2_REPLY_BUF_LEN))
    {
      set_proto_error (error, "Reply-ready message with bad reply length %u", event->value);
      return FALSE;
    }

  return TRUE;
}

gboolean
cv2_message_peek_cmd (const guint8 *buf, gsize len, guint16 *cmd)
{
  if (len < CV2_HEADER_LEN)
    return FALSE;

  *cmd = FP_READ_UINT16_LE (buf + CV2_HDR_CMD);
  return TRUE;
}

void
cv2_reply_free (Cv2Reply *reply)
{
  if (!reply)
    return;

  g_clear_pointer (&reply->raw, g_bytes_unref);
  g_clear_pointer (&reply->params, g_array_unref);
  g_free (reply);
}

/* Bytes after the header's total length are ignored: the reader sometimes
 * sends one more byte than the message holds. */
Cv2Reply *
cv2_reply_parse (const guint8 *buf,
                 gsize         len,
                 guint16       expected_cmd,
                 GError      **error)
{
  g_autoptr(Cv2Reply) reply = NULL;
  FpiByteReader reader;
  const guint8 *data;
  guint32 total;
  guint32 params_len;

  if (len < CV2_HEADER_LEN)
    {
      set_proto_error (error, "Reply too short (%" G_GSIZE_FORMAT " bytes)", len);
      return NULL;
    }

  total = FP_READ_UINT32_LE (buf + CV2_HDR_LEN);
  if (FP_READ_UINT32_LE (buf + CV2_HDR_MAGIC) != CV2_MAGIC ||
      total < CV2_HEADER_LEN || total > len)
    {
      set_proto_error (error, "Bad reply header (length %u, %" G_GSIZE_FORMAT " bytes read)",
                       total, len);
      return NULL;
    }

  reply = g_new0 (Cv2Reply, 1);
  reply->raw = g_bytes_new (buf, total);
  reply->params = g_array_new (FALSE, FALSE, sizeof (Cv2Param));
  data = g_bytes_get_data (reply->raw, NULL);
  reply->cmd = FP_READ_UINT16_LE (data + CV2_HDR_CMD);
  reply->flags = FP_READ_UINT16_LE (data + CV2_HDR_FLAGS);
  reply->session = FP_READ_UINT32_LE (data + CV2_HDR_SESSION);
  reply->status = FP_READ_UINT32_LE (data + CV2_HDR_STATUS);

  if (reply->cmd != expected_cmd)
    {
      set_proto_error (error, "Reply is for command 0x%02x, expected 0x%02x",
                       reply->cmd, expected_cmd);
      return NULL;
    }

  params_len = FP_READ_UINT32_LE (data + CV2_HDR_PARAMS_LEN);
  if (params_len != total - CV2_HEADER_LEN)
    {
      set_proto_error (error, "Parameter area length %u does not match message length %u",
                       params_len, total);
      return NULL;
    }

  fpi_byte_reader_init (&reader, data + CV2_HEADER_LEN, params_len);
  while (fpi_byte_reader_get_remaining (&reader) > 0)
    {
      Cv2Param param;

      if (!fpi_byte_reader_get_uint32_le (&reader, &param.kind) ||
          !fpi_byte_reader_get_uint32_le (&reader, &param.len))
        {
          set_proto_error (error, "Truncated parameter header in reply to 0x%02x", reply->cmd);
          return NULL;
        }

      if (!fpi_byte_reader_get_data (&reader, param.len, &param.data) ||
          !fpi_byte_reader_skip (&reader, CV2_PAD (param.len)))
        {
          set_proto_error (error, "Parameter %u overruns the reply to 0x%02x",
                           reply->params->len, reply->cmd);
          return NULL;
        }

      g_array_append_val (reply->params, param);
    }

  return g_steal_pointer (&reply);
}

/* Accepts a parameter of any length */
#define ANY_LEN G_MAXUINT32

static const Cv2Param *
get_param (const Cv2Reply *reply,
           guint           index,
           guint32         kind,
           guint32         len,
           GError        **error)
{
  const Cv2Param *param;

  if (index >= reply->params->len)
    {
      set_proto_error (error, "Reply to 0x%02x has no parameter %u", reply->cmd, index);
      return NULL;
    }

  param = &g_array_index (reply->params, Cv2Param, index);
  if (param->kind != kind || (len != ANY_LEN && param->len != len))
    {
      set_proto_error (error, "Reply to 0x%02x: parameter %u is %u:%u, expected kind %u",
                       reply->cmd, index, param->kind, param->len, kind);
      return NULL;
    }

  return param;
}

gboolean
cv2_reply_get_session (const Cv2Reply *reply, guint32 *session, GError **error)
{
  const Cv2Param *param = get_param (reply, 0, CV2_PARAM_VALUE, 4, error);

  if (!param)
    return FALSE;

  *session = FP_READ_UINT32_LE (param->data);
  return TRUE;
}

gboolean
cv2_reply_get_capture_id (const Cv2Reply *reply, guint8 *capture_id, GError **error)
{
  const Cv2Param *param = get_param (reply, 0, CV2_PARAM_VALUE, CV2_CAPTURE_ID_LEN, error);

  if (!param)
    return FALSE;

  memcpy (capture_id, param->data, CV2_CAPTURE_ID_LEN);
  return TRUE;
}

/* The done flag is the low byte; the upper bytes carry unrelated data. */
gboolean
cv2_reply_get_sample (const Cv2Reply *reply,
                      gboolean       *done,
                      guint8         *result_id,
                      GError        **error)
{
  const Cv2Param *flag = get_param (reply, 0, CV2_PARAM_VALUE, 4, error);
  const Cv2Param *id;

  if (!flag)
    return FALSE;

  id = get_param (reply, 1, CV2_PARAM_VALUE, CV2_CAPTURE_ID_LEN, error);
  if (!id)
    return FALSE;

  *done = (FP_READ_UINT32_LE (flag->data) & 0xff) == 1;
  memcpy (result_id, id->data, CV2_CAPTURE_ID_LEN);
  return TRUE;
}

/* Parameter 0 is an empty buffer (3:0); the handle is parameter 1. */
gboolean
cv2_reply_get_template (const Cv2Reply *reply, guint32 *template_id, GError **error)
{
  const Cv2Param *param = get_param (reply, 1, CV2_PARAM_VALUE, 4, error);

  if (!param)
    return FALSE;

  *template_id = FP_READ_UINT32_LE (param->data);
  return TRUE;
}

gboolean
cv2_reply_get_match (const Cv2Reply *reply,
                     gboolean       *matched,
                     guint32        *template_id,
                     GError        **error)
{
  const Cv2Param *flag = get_param (reply, 0, CV2_PARAM_VALUE, 4, error);
  const Cv2Param *handle;
  guint32 value;

  if (!flag)
    return FALSE;

  handle = get_param (reply, 1, CV2_PARAM_VALUE, 4, error);
  if (!handle)
    return FALSE;

  value = FP_READ_UINT32_LE (flag->data);
  if (value > 1)
    {
      set_proto_error (error, "Unexpected match result %u", value);
      return FALSE;
    }

  *matched = value == 1;
  *template_id = FP_READ_UINT32_LE (handle->data);
  return TRUE;
}

/* The handles fill the buffer parameter, 4 bytes each; it is empty (3:0)
 * when the chip holds none. Returns an array of guint32. */
GArray *
cv2_reply_get_templates (const Cv2Reply *reply, GError **error)
{
  const Cv2Param *param = get_param (reply, 0, CV2_PARAM_BUFFER, ANY_LEN, error);
  GArray *templates;

  if (!param)
    return NULL;

  if (param->len % 4 != 0)
    {
      set_proto_error (error, "Template list of %u bytes is not a list of handles", param->len);
      return NULL;
    }

  /* A full buffer may have dropped handles, and fprintd deletes stored
   * prints missing from the list */
  if (param->len >= CV2_ENUMERATE_BUF_LEN)
    {
      set_proto_error (error, "Template list fills the %u bytes offered", param->len);
      return NULL;
    }

  templates = g_array_sized_new (FALSE, FALSE, sizeof (guint32), param->len / 4);
  for (guint32 i = 0; i < param->len; i += 4)
    {
      guint32 template_id = FP_READ_UINT32_LE (param->data + i);

      g_array_append_val (templates, template_id);
    }

  return templates;
}

gchar *
cv2_reply_get_version_text (const Cv2Reply *reply, GError **error)
{
  const Cv2Param *param = get_param (reply, 0, CV2_PARAM_STRING, ANY_LEN, error);

  if (!param)
    return NULL;

  return g_strndup ((const gchar *) param->data, param->len);
}

Cv2Outcome
cv2_status_classify (guint16 cmd, guint32 status, GError **error)
{
  if (status == CV2_STATUS_OK)
    return CV2_OUTCOME_OK;

  if (cmd == CV2_CMD_ENROLL_SAMPLE && status == CV2_STATUS_SAMPLE_REJECTED)
    {
      g_propagate_error (error, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return CV2_OUTCOME_RETRY;
    }

  /* No usable capture, e.g. the finger was lifted too soon */
  if ((cmd == CV2_CMD_ENROLL_SAMPLE || cmd == CV2_CMD_MATCH) &&
      status == CV2_STATUS_NO_CAPTURE)
    {
      g_propagate_error (error, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return CV2_OUTCOME_RETRY;
    }

  if (cmd == CV2_CMD_CAPTURE_START && status == CV2_STATUS_CAPTURE_PENDING)
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_BUSY,
                                                   "A capture is still pending on the reader"));
      return CV2_OUTCOME_CAPTURE_PENDING;
    }

  if (cmd == CV2_CMD_MATCH && status == CV2_STATUS_NO_TEMPLATES)
    return CV2_OUTCOME_NO_MATCH;

  if (cmd == CV2_CMD_ENROLL_COMMIT && status == CV2_STATUS_BAD_ATTRIBUTES)
    {
      g_propagate_error (error, cv2_firmware_error (NULL));
      return CV2_OUTCOME_ERROR;
    }

  /* The chip keeps up to 200 objects of every kind (keys, certificates,
   * templates) in its flash; per Broadcom's Citadel SDK, a commit that finds
   * no free entry or no space fails with one of these. Not yet seen on a
   * reader. */
  if (cmd == CV2_CMD_ENROLL_COMMIT &&
      (status == CV2_STATUS_NO_OBJECT_ENTRY || status == CV2_STATUS_NO_FLASH))
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                   "The reader's storage is full (status 0x%02x)",
                                                   status));
      return CV2_OUTCOME_ERROR;
    }

  /* A handle that is not (or no longer) on the chip, e.g. deleted */
  if ((cmd == CV2_CMD_DELETE || cmd == CV2_CMD_MATCH) && status == CV2_STATUS_NO_TEMPLATE)
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                                   "The template is not on the reader"));
      return CV2_OUTCOME_ERROR;
    }

  if (cmd == CV2_CMD_DELETE)
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                   "Deleting the template failed (status 0x%02x)",
                                                   status));
      return CV2_OUTCOME_ERROR;
    }

  set_proto_error (error, "Command 0x%02x failed with status 0x%02x", cmd, status);
  return CV2_OUTCOME_ERROR;
}

gchar *
cv2_version_lookup (const gchar *text, const gchar *key)
{
  g_auto(GStrv) lines = g_strsplit (text, "\n", -1);
  gsize key_len = strlen (key);

  for (guint i = 0; lines[i]; i++)
    if (strncmp (lines[i], key, key_len) == 0 && lines[i][key_len] == ':')
      return g_strstrip (g_strdup (lines[i] + key_len + 1));

  return NULL;
}

GError *
cv2_firmware_error (const gchar *upgrade_ver)
{
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                   "The reader rejected the new template's storage attributes "
                                   "(status 0x24, ControlVault2 firmware %s). Updating the "
                                   "firmware with "
                                   "Dell's ControlVault2 Driver and Firmware package version "
                                   "%s or newer, installed from Windows, may fix this.",
                                   upgrade_ver ? upgrade_ver : "version unknown",
                                   CV2_FW_PACKAGE_NAME);
}
