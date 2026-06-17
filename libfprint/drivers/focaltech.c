/*
 * Copyright (C) 2026 FocalTech Electronics Inc
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

#define FP_COMPONENT "focaltech"

#include <math.h>

#include "focaltech.h"
#include "drivers_api.h"

/* ── USB packet layout ────────────────────────────────────────────────────────
 *
 * All packets share a common framing:
 *
 *   [0x02][len_h][len_l][code][payload...][bcc]
 *
 *   len  = number of bytes from code through the last payload byte (BCC excluded)
 *   bcc  = XOR of bytes at indices 1 .. N-2 (i.e. len_h through last payload byte)
 *
 * Request : code = command byte (FT_CMD_*)
 * Response: code = status byte  (FT_STATUS_OK = 0x04 on success)
 */

/* Magic start byte present in every packet */
#define FT_MAGIC 0x02

/* Command codes */
#define FT_CMD_QUERY 0x80
#define FT_CMD_CAPTURE 0x81
#define FT_CMD_CTRL 0x82

/* Sub-commands used with FT_CMD_QUERY / FT_CMD_CTRL */
#define FT_SUB_FINGER 0x02      /* query finger presence */
#define FT_SUB_COLS 0x03        /* get sensor width */
#define FT_SUB_ROWS 0x04        /* get sensor height */
#define FT_SUB_SENSOR_ID 0x05   /* get chip ID */
#define FT_SUB_CLEAR 0x78       /* clear sensor data (FT_CMD_CTRL) */

/* Response status value indicating success */
#define FT_STATUS_OK 0x04

/* Sensor reports 0x01 in the finger-status byte when a finger is present */
#define FT_FINGER_PRESENT 0x01

/* Timing */
#define FT_CMD_TIMEOUT_MS 2000
#define FT_CAPTURE_TIMEOUT_MS 5000
#define FT_FINGER_POLL_MS 20

/* Receive buffer for normal command responses */
#define FT_CMD_RESPONSE_BUF 512

/*
 * Maximum receive buffer for an image capture response.
 * The known sensor is 64×80 at 16-bit: header(4) + 64×80×2 + bcc(1) = 10245.
 * 0x5806 (22534) gives roughly 2× margin for that.
 */
#define FT_MAX_CAPTURE_BUF 0x5806

/* Number of captures stored as individual templates for enroll */
#define FT_NUM_ENROLL_STAGES 10

/*
 * NCC threshold for verify. Max NCC across all stored templates is used so
 * that any single well-aligned capture can confirm identity, even if the other
 * captures were taken from a different angle or offset.
 *
 * 0.50 was chosen to maintain a clear gap between genuine matches (0.47–0.74)
 * and adjacent-finger false positives. Lower values (e.g. 0.35) can pass
 * neighbouring fingers due to overlapping ridge texture in the search window.
 */
#define FT_NCC_THRESHOLD 0.50
#define FT_NCC_SEARCH_RADIUS 10

/* ── Packet helpers ──────────────────────────────────────────────────────────*/

typedef struct
{
  guint8 magic;
  guint8 len_h;
  guint8 len_l;
  guint8 code;
  guint8 payload[0];
} FtPacket;

static guint8
ft_bcc (const guint8 *data, gsize len)
{
  guint8 bcc = 0;

  for (gsize i = 0; i < len; i++)
    bcc ^= data[i];
  return bcc;
}

/* Build a request packet on the heap (caller takes ownership via g_free). */
static guint8 *
ft_compose_cmd (guint8 cmd, const guint8 *data, guint16 data_len)
{
  /* total payload length field = cmd byte + data bytes */
  guint16 len_field = (guint16) (data_len + 1);
  /* total packet size = magic + len_h + len_l + cmd + data + bcc */
  gsize pkt_size = 3 + len_field + 1;

  guint8 *buf = g_malloc0 (pkt_size);
  FtPacket *pkt = (FtPacket *) buf;

  pkt->magic = FT_MAGIC;
  pkt->len_h = (guint8) (len_field >> 8);
  pkt->len_l = (guint8) (len_field & 0xff);
  pkt->code  = cmd;

  if (data && data_len > 0)
    memcpy (pkt->payload, data, data_len);

  /* BCC covers bytes from len_h to the last payload byte */
  buf[pkt_size - 1] = ft_bcc (buf + 1, pkt_size - 2);

  return buf;
}

/* Validate a received response packet. Returns TRUE on success. */
static gboolean
ft_check_response (const guint8 *buf, gsize buf_len)
{
  if (buf_len < 5)           /* minimum: magic + len_h + len_l + code + bcc */
    return FALSE;

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->magic != FT_MAGIC)
    return FALSE;

  guint16 len_field = (guint16) ((pkt->len_h << 8) | pkt->len_l);
  if (len_field == 0)
    return FALSE;

  gsize pkt_size = 3 + len_field + 1;  /* magic + len_h + len_l + payload + bcc */
  if (pkt_size > buf_len)
    return FALSE;

  /* BCC covers bytes [1 .. pkt_size-2] */
  if (ft_bcc (buf + 1, pkt_size - 2) != buf[pkt_size - 1])
    return FALSE;

  return TRUE;
}

/* ── Device struct ───────────────────────────────────────────────────────────*/

struct _FpiDeviceFocaltech
{
  FpDevice parent;

  guint8   bulk_in_ep;
  guint8   bulk_out_ep;

  guint16  chip_id;
  guint    sensor_width;
  guint    sensor_height;

  /* Normalized 8-bit image buffer (sensor_width × sensor_height bytes) */
  guint8 *img_buf;

  /* Enrollment storage: FT_NUM_ENROLL_STAGES × w × h bytes */
  guint8 *enroll_images;
  guint   enroll_stage;      /* stages completed so far */

  /*
   * Pointer to the currently running outer SSM (open, enroll, or verify).
   * All USB-callback helpers read/write this to advance the outer SSM.
   * The inner cmd SSM is tracked separately via cmd_ssm.
   */
  FpiSsm *task_ssm;

  /* cmd SSM plumbing */
  FpiSsm         *cmd_ssm;
  FpiUsbTransfer *cmd_transfer;
  gboolean        cmd_cancelable;
  gsize           cmd_len_in;
};

G_DEFINE_TYPE (FpiDeviceFocaltech, fpi_device_focaltech, FP_TYPE_DEVICE)

/* ── Command callback type ───────────────────────────────────────────────────*/

typedef void (*FtCmdCallback) (FpiDeviceFocaltech *self,
                               const guint8       *buf,
                               gsize               buf_len,
                               GError             *error);

typedef struct
{
  FtCmdCallback callback;
} FtCmdData;

/* ── Command SSM (2 states: send → receive) ──────────────────────────────────*/

enum {
  FT_CMD_SEND = 0,
  FT_CMD_RECV,
  FT_CMD_NUM_STATES,
};

static void
ft_cmd_receive_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer userdata, GError *error)
{
  FtCmdData *data = userdata;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&error));
      return;
    }

  /* zero-length packet: retry this state */
  if (transfer->actual_length == 0)
    {
      fpi_ssm_jump_to_state (transfer->ssm, fpi_ssm_get_cur_state (transfer->ssm));
      return;
    }

  if (!ft_check_response (transfer->buffer, transfer->actual_length))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "invalid response packet"));
      return;
    }

  if (data && data->callback)
    data->callback (FPI_DEVICE_FOCALTECH (device),
                    transfer->buffer, transfer->actual_length, NULL);

  fpi_ssm_mark_completed (transfer->ssm);
}

static void
ft_cmd_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FT_CMD_SEND:
      if (self->cmd_transfer)
        {
          self->cmd_transfer->ssm = ssm;
          fpi_usb_transfer_submit (g_steal_pointer (&self->cmd_transfer),
                                   FT_CMD_TIMEOUT_MS,
                                   NULL,
                                   fpi_ssm_usb_transfer_cb,
                                   NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case FT_CMD_RECV:
      if (self->cmd_len_in == 0)
        {
          FtCmdData *d = fpi_ssm_get_data (ssm);
          if (d && d->callback)
            d->callback (self, NULL, 0, NULL);
          fpi_ssm_mark_completed (ssm);
          return;
        }

      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, self->bulk_in_ep, self->cmd_len_in);
      fpi_usb_transfer_submit (transfer,
                               self->cmd_cancelable ?
                               FT_CAPTURE_TIMEOUT_MS : FT_CMD_TIMEOUT_MS,
                               self->cmd_cancelable ?
                               fpi_device_get_cancellable (device) : NULL,
                               ft_cmd_receive_cb,
                               fpi_ssm_get_data (ssm));
      break;
    }
}

static void
ft_cmd_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);
  FtCmdData *data = fpi_ssm_get_data (ssm);

  self->cmd_ssm = NULL;

  if (error && data && data->callback)
    data->callback (self, NULL, 0, g_steal_pointer (&error));
  else
    g_clear_error (&error);
}

static void
ft_cmd_data_free (FtCmdData *data)
{
  g_free (data);
}

/* Send a request and receive the response asynchronously.
 * buffer_out is transferred to the USB layer which calls g_free on it. */
static void
ft_get_cmd (FpDevice *device, guint8 *buffer_out, gsize length_out, gsize length_in, gboolean cancelable, FtCmdCallback callback)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);
  FtCmdData *data = g_new0 (FtCmdData, 1);

  g_autoptr(FpiUsbTransfer) transfer = NULL;

  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_bulk_full (transfer, self->bulk_out_ep,
                                   buffer_out, length_out, g_free);

  data->callback = callback;
  self->cmd_transfer   = g_steal_pointer (&transfer);
  self->cmd_len_in     = length_in;
  self->cmd_cancelable = cancelable;

  self->cmd_ssm = fpi_ssm_new (device, ft_cmd_run_state, FT_CMD_NUM_STATES);
  fpi_ssm_set_data (self->cmd_ssm, data, (GDestroyNotify) ft_cmd_data_free);
  fpi_ssm_start (self->cmd_ssm, ft_cmd_ssm_done);
}

/* ── Image normalization ─────────────────────────────────────────────────────
 *
 * The FT9362 sensor transmits 16-bit pixel values (little-endian) where
 * ridges have HIGH raw values. We negate them so that ridges map to the
 * low end of the 8-bit output range (dark), then apply a linear stretch
 * with a 50% clip to keep contrast consistent across captures.
 */
static void
ft_normalize_image_16 (const guint16 *src, guint width, guint height, guint8 *dst)
{
  guint n = width * height;
  g_autofree gint16 *neg = g_new (gint16, n);

  /* Step 1: negate so ridges (high raw values) become low (dark) values */
  for (guint i = 0; i < n; i++)
    neg[i] = -(gint16) GUINT16_FROM_LE (src[i]);

  /* Step 2: shift so minimum = 0 */
  gint16 vmin_s = neg[0];
  for (guint i = 1; i < n; i++)
    if (neg[i] < vmin_s)
      vmin_s = neg[i];
  for (guint i = 0; i < n; i++)
    neg[i] -= vmin_s;

  /* Step 3: find max after shift */
  gint16 vmax_s = neg[0];
  for (guint i = 1; i < n; i++)
    if (neg[i] > vmax_s)
      vmax_s = neg[i];

  if (vmax_s == 0)
    {
      memset (dst, 128, n);
      return;
    }

  /* Clip at 50% of the shifted range for consistent contrast */
  guint clip_val = (guint) vmax_s / 2;
  if (clip_val == 0)
    clip_val = 1;

  for (guint i = 0; i < n; i++)
    {
      guint v = (guint) neg[i];
      dst[i] = v >= clip_val ? 255 : (guint8) (v * 255u / clip_val);
    }
}

/* ── NCC matching ────────────────────────────────────────────────────────────
 *
 * Normalized Cross-Correlation with a ±FT_NCC_SEARCH_RADIUS pixel search window.
 * ft_ncc_pair returns the best NCC score over all shifts for one pair of images.
 * ft_ncc_match returns TRUE if the max score over all enrolled templates exceeds
 * FT_NCC_THRESHOLD.
 */
static gdouble
ft_ncc_pair (const guint8 *img_a, const guint8 *img_b, guint w, guint h)
{
  gint r  = FT_NCC_SEARCH_RADIUS;
  gint iw = (gint) w;
  gint ih = (gint) h;
  gdouble best = 0.0;

  for (gint dy = -r; dy <= r; dy++)
    {
      for (gint dx = -r; dx <= r; dx++)
        {
          /* Overlapping rectangle when img_b is shifted by (dx, dy) relative to img_a:
           *   img_a(x, y) ↔ img_b(x + dx, y + dy)
           * Both coordinates must be in [0, w) × [0, h).
           */
          gdouble sum_a = 0, sum_b = 0;
          guint n = 0;

          for (gint y = 0; y < ih; y++)
            {
              gint by = y + dy;
              if (by < 0 || by >= ih)
                continue;
              for (gint x = 0; x < iw; x++)
                {
                  gint bx = x + dx;
                  if (bx < 0 || bx >= iw)
                    continue;
                  sum_a += img_a[y * iw + x];
                  sum_b += img_b[by * iw + bx];
                  n++;
                }
            }

          if (n == 0)
            continue;
          gdouble mean_a = sum_a / n;
          gdouble mean_b = sum_b / n;

          gdouble num = 0, den_a = 0, den_b = 0;
          for (gint y = 0; y < ih; y++)
            {
              gint by = y + dy;
              if (by < 0 || by >= ih)
                continue;
              for (gint x = 0; x < iw; x++)
                {
                  gint bx = x + dx;
                  if (bx < 0 || bx >= iw)
                    continue;
                  gdouble da = img_a[y * iw + x] - mean_a;
                  gdouble db = img_b[by * iw + bx] - mean_b;
                  num   += da * db;
                  den_a += da * da;
                  den_b += db * db;
                }
            }

          gdouble denom = sqrt (den_a * den_b);
          if (denom < 1e-10)
            continue;
          gdouble ncc = num / denom;
          if (ncc > best)
            best = ncc;
        }
    }

  return best;
}

static gdouble
ft_ncc_match (const guint8 *verify_img, const guint8 *enroll_imgs, guint n_templates, guint w, guint h)
{
  gdouble best = 0.0;

  for (guint i = 0; i < n_templates; i++)
    {
      gdouble s = ft_ncc_pair (verify_img, enroll_imgs + (gsize) i * w * h, w, h);
      if (s > best)
        best = s;
    }

  fp_dbg ("max NCC over %u templates: %.4f (threshold %.2f) — %s",
          n_templates, best, (gdouble) FT_NCC_THRESHOLD,
          best >= FT_NCC_THRESHOLD ? "MATCH" : "NO MATCH");
  return best;
}

/* ── Open SSM ────────────────────────────────────────────────────────────────*/

enum open_states {
  OPEN_GET_SENSOR_ID = 0,
  OPEN_GET_WIDTH,
  OPEN_GET_HEIGHT,
  OPEN_CLEAR_SENSOR,
  OPEN_NUM_STATES,
};

static void
open_sensor_id_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "get sensor ID failed"));
      return;
    }

  self->chip_id = (guint16) (pkt->payload[0] | ((guint16) pkt->payload[1] << 8));
  fp_dbg ("chip_id = 0x%04x", self->chip_id);

  fpi_ssm_next_state (self->task_ssm);
}

static void
open_get_width_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "get sensor width failed"));
      return;
    }

  self->sensor_width = pkt->payload[0];
  fp_dbg ("sensor_width = %u", self->sensor_width);

  fpi_ssm_next_state (self->task_ssm);
}

static void
open_get_height_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "get sensor height failed"));
      return;
    }

  self->sensor_height = pkt->payload[0];
  fp_dbg ("sensor_height = %u", self->sensor_height);

  fpi_ssm_next_state (self->task_ssm);
}

static void
open_clear_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "clear sensor failed"));
      return;
    }

  fpi_ssm_mark_completed (self->task_ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = ssm;

  guint8 sub;
  guint8 *cmd;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_SENSOR_ID:
      sub = FT_SUB_SENSOR_ID;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, open_sensor_id_cb);
      break;

    case OPEN_GET_WIDTH:
      sub = FT_SUB_COLS;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, open_get_width_cb);
      break;

    case OPEN_GET_HEIGHT:
      sub = FT_SUB_ROWS;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, open_get_height_cb);
      break;

    case OPEN_CLEAR_SENSOR:
      sub = FT_SUB_CLEAR;
      cmd = ft_compose_cmd (FT_CMD_CTRL, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, open_clear_cb);
      break;
    }
}

static void
open_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = NULL;

  if (error)
    {
      fpi_device_open_complete (device, error);
      return;
    }

  if (self->sensor_width == 0 || self->sensor_height == 0)
    {
      fpi_device_open_complete (device,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "invalid sensor dimensions"));
      return;
    }

  guint n = self->sensor_width * self->sensor_height;
  g_free (self->img_buf);
  self->img_buf = g_malloc0 (n);

  fp_dbg ("sensor %ux%u chip_id 0x%04x", self->sensor_width, self->sensor_height, self->chip_id);
  fpi_device_open_complete (device, NULL);
}

/* ── Enroll SSM ──────────────────────────────────────────────────────────────
 *
 * Flat state machine where task_ssm always points to the enroll SSM.
 * Each state dispatches one USB command; the callback directly advances
 * task_ssm so that no nested-SSM ownership confusion occurs.
 *
 *   ENROLL_WAIT_ON  →  ENROLL_CAPTURE  →  ENROLL_WAIT_OFF
 *        ↑_______________________________________________|   (if more stages)
 */

enum enroll_states {
  ENROLL_WAIT_ON = 0,
  ENROLL_CAPTURE,
  ENROLL_WAIT_OFF,
  ENROLL_NUM_STATES,
};

static void
enroll_finger_on_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "finger poll failed"));
      return;
    }

  if (pkt->payload[0] == FT_FINGER_PRESENT)
    {
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
      fpi_ssm_jump_to_state_delayed (self->task_ssm, ENROLL_CAPTURE, 200);
    }
  else
    {
      fpi_ssm_jump_to_state_delayed (self->task_ssm, ENROLL_WAIT_ON, FT_FINGER_POLL_MS);
    }
}

static void
enroll_capture_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "capture failed"));
      return;
    }

  guint w = self->sensor_width;
  guint h = self->sensor_height;
  gsize payload_bytes = buf_len - sizeof (FtPacket) - 1;

  if (payload_bytes < (gsize) w * h * 2)
    {
      fp_warn ("short image payload (%zu bytes, retrying)", payload_bytes);
      fpi_ssm_jump_to_state_delayed (self->task_ssm, ENROLL_CAPTURE, 200);
      return;
    }

  ft_normalize_image_16 ((const guint16 *) pkt->payload, w, h, self->img_buf);
  memcpy (self->enroll_images + (gsize) self->enroll_stage * w * h, self->img_buf, w * h);

  fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage + 1, NULL, NULL);
  fpi_ssm_next_state (self->task_ssm);
}

static void
enroll_finger_off_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "finger poll failed"));
      return;
    }

  if (pkt->payload[0] == FT_FINGER_PRESENT)
    {
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);
      self->enroll_stage++;
      if (self->enroll_stage < FT_NUM_ENROLL_STAGES)
        fpi_ssm_jump_to_state (self->task_ssm, ENROLL_WAIT_ON);
      else
        fpi_ssm_mark_completed (self->task_ssm);
    }
  else
    {
      fpi_ssm_jump_to_state_delayed (self->task_ssm, ENROLL_WAIT_OFF, FT_FINGER_POLL_MS);
    }
}

static void
enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = ssm;

  guint8 sub;
  guint8 *cmd;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_WAIT_ON:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      sub = FT_SUB_FINGER;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, enroll_finger_on_cb);
      break;

    case ENROLL_CAPTURE:
      cmd = ft_compose_cmd (FT_CMD_CAPTURE, NULL, 0);
      ft_get_cmd (device, cmd, 5, FT_MAX_CAPTURE_BUF, TRUE, enroll_capture_cb);
      break;

    case ENROLL_WAIT_OFF:
      sub = FT_SUB_FINGER;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, enroll_finger_off_cb);
      break;
    }
}

static void
enroll_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = NULL;

  if (error)
    {
      fpi_device_enroll_complete (device, NULL, error);
      return;
    }

  guint w = self->sensor_width;
  guint h = self->sensor_height;
  gsize n_bytes = (gsize) FT_NUM_ENROLL_STAGES * w * h;

  FpPrint *print;
  fpi_device_get_enroll_data (device, &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);

  GVariant *data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, self->enroll_images, n_bytes, 1);
  g_object_set (print, "fpi-data", data, NULL);

  fpi_device_enroll_complete (device, g_object_ref (print), NULL);
}

static void
dev_enroll (FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);
  guint w = self->sensor_width;
  guint h = self->sensor_height;

  g_free (self->enroll_images);
  self->enroll_images = g_malloc0 ((gsize) FT_NUM_ENROLL_STAGES * w * h);
  self->enroll_stage  = 0;

  FpiSsm *ssm = fpi_ssm_new (device, enroll_run_state, ENROLL_NUM_STATES);
  fpi_ssm_start (ssm, enroll_ssm_done);
}

/* ── Verify SSM ──────────────────────────────────────────────────────────────*/

enum verify_states {
  VERIFY_WAIT_ON = 0,
  VERIFY_CAPTURE,
  VERIFY_WAIT_OFF,
  VERIFY_NUM_STATES,
};

static void
verify_finger_on_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "finger poll failed"));
      return;
    }

  if (pkt->payload[0] == FT_FINGER_PRESENT)
    {
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
      fpi_ssm_jump_to_state_delayed (self->task_ssm, VERIFY_CAPTURE, 200);
    }
  else
    {
      fpi_ssm_jump_to_state_delayed (self->task_ssm, VERIFY_WAIT_ON, FT_FINGER_POLL_MS);
    }
}

static void
verify_capture_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "capture failed"));
      return;
    }

  guint w = self->sensor_width;
  guint h = self->sensor_height;
  gsize payload_bytes = buf_len - sizeof (FtPacket) - 1;

  if (payload_bytes < (gsize) w * h * 2)
    {
      fp_warn ("short image payload (%zu bytes, retrying)", payload_bytes);
      fpi_ssm_jump_to_state_delayed (self->task_ssm, VERIFY_CAPTURE, 200);
      return;
    }

  ft_normalize_image_16 ((const guint16 *) pkt->payload, w, h, self->img_buf);

  FpDevice *device = FP_DEVICE (self);

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *template_print;
      fpi_device_get_verify_data (device, &template_print);

      GVariant *data_var = NULL;
      g_object_get (template_print, "fpi-data", &data_var, NULL);
      if (!data_var)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "no enrollment data"));
          return;
        }

      gsize n_bytes;
      const guint8 *enroll_data = g_variant_get_fixed_array (data_var, &n_bytes, 1);
      guint n_templates = (guint) (n_bytes / ((gsize) w * h));
      gboolean matched = n_templates > 0 &&
                         ft_ncc_match (self->img_buf, enroll_data, n_templates, w, h) >= FT_NCC_THRESHOLD;
      g_variant_unref (data_var);

      fpi_device_verify_report (device,
                                matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
    }
  else
    {
      GPtrArray *prints = NULL;
      fpi_device_get_identify_data (device, &prints);

      FpPrint *best_print = NULL;
      gdouble best_score = FT_NCC_THRESHOLD;

      for (guint i = 0; i < prints->len; i++)
        {
          FpPrint *candidate = g_ptr_array_index (prints, i);
          GVariant *data_var = NULL;
          g_object_get (candidate, "fpi-data", &data_var, NULL);
          if (!data_var)
            continue;

          gsize n_bytes;
          const guint8 *enroll_data = g_variant_get_fixed_array (data_var, &n_bytes, 1);
          guint n_templates = (guint) (n_bytes / ((gsize) w * h));
          if (n_templates > 0)
            {
              gdouble score = ft_ncc_match (self->img_buf, enroll_data, n_templates, w, h);
              if (score > best_score)
                {
                  best_score = score;
                  best_print = candidate;
                }
            }
          g_variant_unref (data_var);
        }

      fpi_device_identify_report (device, best_print, NULL, NULL);
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
verify_finger_off_cb (FpiDeviceFocaltech *self, const guint8 *buf, gsize buf_len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  const FtPacket *pkt = (const FtPacket *) buf;
  if (pkt->code != FT_STATUS_OK)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "finger poll failed"));
      return;
    }

  if (pkt->payload[0] != FT_FINGER_PRESENT)
    {
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);
      fpi_ssm_mark_completed (self->task_ssm);
    }
  else
    {
      fpi_ssm_jump_to_state_delayed (self->task_ssm, VERIFY_WAIT_OFF, FT_FINGER_POLL_MS);
    }
}

static void
verify_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = ssm;

  guint8 sub;
  guint8 *cmd;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VERIFY_WAIT_ON:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      sub = FT_SUB_FINGER;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, verify_finger_on_cb);
      break;

    case VERIFY_CAPTURE:
      cmd = ft_compose_cmd (FT_CMD_CAPTURE, NULL, 0);
      ft_get_cmd (device, cmd, 5, FT_MAX_CAPTURE_BUF, TRUE, verify_capture_cb);
      break;

    case VERIFY_WAIT_OFF:
      sub = FT_SUB_FINGER;
      cmd = ft_compose_cmd (FT_CMD_QUERY, &sub, 1);
      ft_get_cmd (device, cmd, 6, FT_CMD_RESPONSE_BUF, FALSE, verify_finger_off_cb);
      break;
    }
}

static void
verify_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);

  self->task_ssm = NULL;
  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (device, error);
  else
    fpi_device_identify_complete (device, error);
}

static void
dev_verify (FpDevice *device)
{
  FpiSsm *ssm = fpi_ssm_new (device, verify_run_state, VERIFY_NUM_STATES);

  fpi_ssm_start (ssm, verify_ssm_done);
}

static void
dev_identify (FpDevice *device)
{
  FpiSsm *ssm = fpi_ssm_new (device, verify_run_state, VERIFY_NUM_STATES);

  fpi_ssm_start (ssm, verify_ssm_done);
}

/* ── Open / Close ────────────────────────────────────────────────────────────*/

static void
dev_open (FpDevice *device)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (device);
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);
  GPtrArray *interfaces;
  GPtrArray *endpoints;
  GUsbInterface *iface;
  GUsbEndpoint *ep;

  /* Discover bulk endpoints by iterating the interface descriptors. */
  interfaces = g_usb_device_get_interfaces (usb_dev, &error);
  if (!interfaces)
    {
      fpi_device_open_complete (device, error);
      return;
    }

  for (guint i = 0; i < interfaces->len; i++)
    {
      iface     = g_ptr_array_index (interfaces, i);
      endpoints = g_usb_interface_get_endpoints (iface);

      for (guint j = 0; j < endpoints->len; j++)
        {
          ep = g_ptr_array_index (endpoints, j);
          if (g_usb_endpoint_get_direction (ep) == G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST)
            self->bulk_in_ep  = g_usb_endpoint_get_address (ep);
          else
            self->bulk_out_ep = g_usb_endpoint_get_address (ep);
        }

      if (!g_usb_device_claim_interface (usb_dev,
                                         g_usb_interface_get_number (iface),
                                         0, &error))
        {
          g_ptr_array_unref (endpoints);
          g_ptr_array_unref (interfaces);
          fpi_device_open_complete (device, error);
          return;
        }

      g_ptr_array_unref (endpoints);
    }
  g_ptr_array_unref (interfaces);

  fp_dbg ("bulk_in=0x%02x bulk_out=0x%02x", self->bulk_in_ep, self->bulk_out_ep);

  /* Query sensor parameters. */
  self->task_ssm = fpi_ssm_new (device, open_run_state, OPEN_NUM_STATES);
  fpi_ssm_start (self->task_ssm, open_ssm_done);
}

static void
dev_close (FpDevice *device)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (device), 0, 0, &error);
  fpi_device_close_complete (device, error);
}

/* ── GObject boilerplate ─────────────────────────────────────────────────────*/

static const FpIdEntry id_table[] = {
  { .vid = 0x2808, .pid = 0xc652 },
  { .vid = 0,      .pid = 0      },
};

static void
fpi_device_focaltech_init (FpiDeviceFocaltech *self)
{
}

static void
fpi_device_focaltech_finalize (GObject *object)
{
  FpiDeviceFocaltech *self = FPI_DEVICE_FOCALTECH (object);

  g_clear_pointer (&self->img_buf, g_free);
  g_clear_pointer (&self->enroll_images, g_free);
  G_OBJECT_CLASS (fpi_device_focaltech_parent_class)->finalize (object);
}

static void
fpi_device_focaltech_class_init (FpiDeviceFocaltechClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  obj_class->finalize = fpi_device_focaltech_finalize;

  dev_class->id              = "focaltech";
  dev_class->full_name       = "FocalTech Systems Co., Ltd fingerprint";
  dev_class->type            = FP_DEVICE_TYPE_USB;
  dev_class->id_table        = id_table;
  dev_class->scan_type       = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = FT_NUM_ENROLL_STAGES;
  dev_class->features        = FP_DEVICE_FEATURE_VERIFY | FP_DEVICE_FEATURE_IDENTIFY;
  dev_class->open            = dev_open;
  dev_class->close           = dev_close;
  dev_class->enroll          = dev_enroll;
  dev_class->verify          = dev_verify;
  dev_class->identify        = dev_identify;
}
