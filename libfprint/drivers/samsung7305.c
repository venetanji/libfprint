/*
 * Samsung Fingerprint Device (04e8:7305) driver for libfprint
 *
 * Reverse-engineered from USB captures of the Windows driver on a
 * Samsung Notebook 7 Spin (NP730QAA-K02US).
 *
 * Copyright (C) 2026 Justin Hall <jwhall@gmail.com>
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

#define FP_COMPONENT "samsung7305"

#include "fpi-log.h"
#include "drivers_api.h"
#include "fpi-assembling.h"

/* ── Frame assembly ─────────────────────────────────────────────────────── */

#define MAX_FRAMES 8
#define MIN_FRAMES 4          /* need 1 to drop + ASSEMBLE_FRAMES to keep */
#define ASSEMBLE_FRAMES 3
#define ASSEMBLE_STRIDE 64    /* fixed y-offset between kept frames */

/* NOTE: The sensor is physically an area (press) sensor — the user holds
 * the finger flat, not swiping. We still declare scan_type = SWIPE and use
 * fpi_assemble_frames because NBIS/bozorth3 cannot extract enough minutiae
 * from a single 56x192 frame (~4–6 minutiae; bozorth3 wants hundreds of
 * edges). Stacking several frames at a fixed stride produces a composite
 * large enough for reliable matching. The stride is deterministic rather
 * than motion-estimated so enroll and verify produce identical geometry.
 * Compare drivers/elan.c, which takes the same approach for similarly
 * narrow area-style sensors. */
static unsigned char
samsung7305_get_pixel (struct fpi_frame_asmbl_ctx *ctx,
                       struct fpi_frame *frame, unsigned int x,
                       unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx assembling_ctx = {
  .frame_width  = 56,
  .frame_height = 192,
  .image_width  = 56 * 3 / 2,
  .get_pixel    = samsung7305_get_pixel,
};

/* ── Image geometry ─────────────────────────────────────────────────────── */

#define FRAME_WIDTH 56
#define FRAME_HEIGHT 192
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT)      /* 10752 bytes */

#define STRIP_WIDTH 56
#define STRIP_HEIGHT 24
#define STRIP_SIZE (STRIP_WIDTH * STRIP_HEIGHT)      /* 1344 bytes */

/* ── USB endpoints ──────────────────────────────────────────────────────── */

#define SAMSUNG_EP_OUT (0x01 | FPI_USB_ENDPOINT_OUT)
#define SAMSUNG_EP_IN (0x02 | FPI_USB_ENDPOINT_IN)

/* ── Timeouts ───────────────────────────────────────────────────────────── */

#define CMD_TIMEOUT 2000
#define IMG_TIMEOUT 5000
#define POLL_DELAY 30
#define DRAIN_TIMEOUT 50
#define DRAIN_BUF_SIZE 16384

/* ── Status register (0x1c 0x00) response byte[1] values ───────────────── */

#define STATUS_FINGER 0x81
#define STATUS_IDLE 0x80
#define STATUS_READY 0x20

/* ── Protocol: init sequence ────────────────────────────────────────────── */

static const guint8 cmd_reset[]    = { 0xf8 };
static const guint8 cmd_status[]   = { 0x1c, 0x00 };
static const guint8 cmd_cfg1[]     = { 0xfc, 0x00, 0x00 };
static const guint8 cmd_cfg2[]     = { 0x90, 0x34 };
static const guint8 cmd_cfg3[]     = { 0x68, 0x33, 0x33, 0x33, 0x33,
                                       0x3f, 0x3f, 0x3f, 0x3f };
static const guint8 cmd_cfg4[]     = { 0x9c, 0x55, 0x40, 0x00, 0x2d, 0x24 };
static const guint8 cmd_cfg5[]     = { 0x6c, 0x2b };
static const guint8 cmd_cfg6[]     = { 0x8c, 0x12 };
static const guint8 cmd_cfg7[]     = { 0x5c, 0x0b };
/* Calibration blob — captured from the Windows driver (04e8:7305) and
 * identical across every init across two independent captures on the same
 * unit.  If future testing on a second physical unit shows a different
 * payload, this blob will need to be read from device storage instead of
 * being hardcoded.  See PROTOCOL.md § Calibration. */
static const guint8 cmd_calib[]    = { 0x98, 0x03, 0xea, 0xbf, 0x68,
                                       0x02, 0x09, 0x15, 0x2a, 0x7a, 0xde };
static const guint8 cmd_capparam[] = { 0xd8, 0x00, 0x01, 0x20, 0x20 };
static const guint8 cmd_cfg8[]     = { 0xe0, 0x32, 0x70, 0x22 };
static const guint8 cmd_cfg9[]     = { 0xe4, 0x32, 0x88, 0x70 };
static const guint8 cmd_cfg10[]    = { 0xe8, 0x32, 0x88 };
static const guint8 cmd_cfg11[]    = { 0xdc, 0x19, 0x01, 0x00, 0x0a };
static const guint8 cmd_cfg12[]    = { 0xa8, 0x0f, 0x0a };
static const guint8 cmd_cfg13[]    = { 0xa0, 0x0a, 0x02 };
static const guint8 cmd_initdone[] = { 0x28 };

static const guint8 cmd_arm[]    = { 0x20 };
static const guint8 cmd_cap_rd[] = { 0xd4, 0x00, 0x00, 0x00, 0x00 };

static const guint8 cmd_capgeo[]         = { 0x54, 0x50, 0x18, 0x00, 0x38 };
static const guint8 cmd_capcfg1[]        = { 0xa0, 0x10, 0x00 };
static const guint8 cmd_capcfg2[]        = { 0xa8, 0x0f, 0x1a };
static const guint8 cmd_capcfg3[]        = { 0x64, 0x0c };
static const guint8 cmd_capcfg4[]        = { 0x5c, 0x0a };

static const guint8 cmd_trigger[]        = { 0xc0 };
static const guint8 cmd_between_strips[] = { 0xa8, 0x0f, 0x0a };

static const guint8 cmd_ff_cfg1[]        = { 0xa0, 0x0e, 0x06 };
static const guint8 cmd_ff_cfg2[]        = { 0x64, 0x1e };
static const guint8 cmd_ff_cfg3[]        = { 0x5c, 0x0b };
static const guint8 cmd_ff_geo[]         = { 0x54, 0x00, 0xc0, 0x00, 0x38 };

/* Per-transfer heartbeat payload (PROTOCOL.md → Transport) — identical in
 * every occurrence across the Windows capture. */
static const guint8 heartbeat_payload[16] = {
  0xc0, 0xc6, 0x2d, 0x00, 0x08, 0x00, 0xff, 0x01,
  0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

typedef struct
{
  const guint8 *data;
  gsize         len;
} SamsungCmd;

#define SCMD(arr) { (arr), G_N_ELEMENTS (arr) }

static const SamsungCmd init_seq[] = {
  SCMD (cmd_reset),
  SCMD (cmd_status),
  SCMD (cmd_status),
  SCMD (cmd_cfg1),
  SCMD (cmd_cfg2),
  SCMD (cmd_cfg3),
  SCMD (cmd_cfg4),
  SCMD (cmd_cfg5),
  SCMD (cmd_cfg6),
  SCMD (cmd_cfg7),
  SCMD (cmd_calib),
  SCMD (cmd_capparam),
  SCMD (cmd_cfg8),
  SCMD (cmd_cfg9),
  SCMD (cmd_cfg10),
  SCMD (cmd_cfg11),
  SCMD (cmd_cfg12),
  SCMD (cmd_cfg13),
  SCMD (cmd_initdone),
};

static const SamsungCmd cap_setup_seq[] = {
  SCMD (cmd_capgeo),
  SCMD (cmd_capcfg1),
  SCMD (cmd_capcfg2),
  SCMD (cmd_capcfg3),
  SCMD (cmd_capcfg4),
};

static const SamsungCmd ff_setup_seq[] = {
  SCMD (cmd_ff_cfg1),
  SCMD (cmd_ff_cfg2),
  SCMD (cmd_ff_cfg3),
  SCMD (cmd_ff_geo),
};

/* ── SSM states ─────────────────────────────────────────────────────────── */

enum {
  M_DRAIN,
  M_INIT,
  M_POLL_ARM,
  M_POLL_STATUS,
  M_POLL_CAP,
  M_CAP_SETUP,
  M_TRIGGER1,
  M_WAIT1,
  M_STRIP1_READ,
  M_STRIP1_FTR,
  M_BETWEEN_S1_S2,
  M_TRIGGER2,
  M_WAIT2,
  M_STRIP2_READ,
  M_STRIP2_FTR,
  M_REPOLL_ARM,
  M_REPOLL_STATUS,
  M_REPOLL_CAP,
  M_TRIGGER3,
  M_WAIT3,
  M_STRIP3_READ,
  M_STRIP3_FTR,
  M_FF_SETUP,
  M_TRIGGER4,
  M_WAIT4,
  M_FRAME_READ,
  M_STORE_FRAME,
  M_ASSEMBLE,
  M_NUM_STATES,
};

struct _FpiDeviceSamsung7305
{
  FpImageDevice parent;
  FpiSsm       *ssm;
  guint8       *img_buf;
  guint8       *img_cmd_buf;
  guint8       *img_strip[3];
  guint         cur_strip;
  gboolean      deactivating;
  guint         seq_idx;
  guint         strip_bytes_received;
  guint         frame_bytes_received;
  GSList       *frames;          /* GSList of struct fpi_frame*, prepended */
  guint         frames_captured;
};

G_DECLARE_FINAL_TYPE (FpiDeviceSamsung7305, fpi_device_samsung7305,
                      FPI, DEVICE_SAMSUNG7305, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceSamsung7305, fpi_device_samsung7305, FP_TYPE_IMAGE_DEVICE);

/* ── Forward declarations ───────────────────────────────────────────────── */

typedef void (*ExchangeDone) (FpDevice     *dev,
                              FpiSsm       *ssm,
                              const guint8 *resp,
                              gsize         resp_len,
                              GError       *error);

static void run_init_seq (FpiDeviceSamsung7305 *self);
static void run_cap_seq (FpiDeviceSamsung7305 *self);
static void strip_image_read_cb (FpiUsbTransfer *transfer,
                                 FpDevice       *dev,
                                 gpointer        unused,
                                 GError         *error);
static void image_read_cb (FpiUsbTransfer *transfer,
                           FpDevice       *dev,
                           gpointer        unused,
                           GError         *error);
static void img_cmd_write_cb (FpiUsbTransfer *transfer,
                              FpDevice       *dev,
                              gpointer        unused,
                              GError         *error);

static void bracketed_submit (FpDevice     *dev,
                              FpiSsm       *ssm,
                              const guint8 *cmd,
                              gsize         cmd_len,
                              guint16       prime_len,
                              gboolean      doorbell,
                              ExchangeDone  done_cb);
static void bracketed_c4_submit (FpDevice              *dev,
                                 FpiSsm                *ssm,
                                 guint8                *cmd_buf,
                                 gsize                  len,
                                 FpiUsbTransferCallback read_cb);

/* ── Deactivation guard ─────────────────────────────────────────────────── */

static gboolean
check_deactivating (FpiUsbTransfer *transfer, GError *error)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (transfer->device);

  if (error)
    {
      fp_warn ("samsung7305: error in SSM state %d (seq_idx=%u): %s",
               fpi_ssm_get_cur_state (transfer->ssm),
               self->seq_idx,
               error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
      return TRUE;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return TRUE;
    }
  return FALSE;
}

/* ── Bracketed bulk exchange ────────────────────────────────────────────── *
 *
 * Every bulk write+read on this device must be wrapped in vendor control
 * transfers on EP0 (PROTOCOL.md → Transport).  The chain is:
 *
 *   1. CTRL-OUT 0xc3 (heartbeat, 16-byte fixed payload)
 *   2. CTRL-OUT 0xca wValue=0x0003 wIndex=N  (primes EP2 response length)
 *   3. BULK-OUT cmd
 *   4. BULK-IN N bytes
 *   5. CTRL-IN 0xda wValue=0x0007 (2 B doorbell) — only if doorbell==TRUE,
 *      which is required for 0x20 / 0x28 / 0xc0 / 0xf8.
 *
 * The context is allocated on ex start and freed on ex completion.  The
 * response buffer is handed to done_cb as a read-only snapshot; done_cb
 * owns the error (if non-NULL) and must consume it via fpi_ssm_mark_failed
 * or g_error_free.
 */

typedef struct
{
  const guint8 *cmd;
  gsize         cmd_len;
  guint16       prime_len;
  gboolean      doorbell;
  ExchangeDone  done_cb;
  guint8       *resp_buf;
  gsize         resp_actual;
} ExchangeCtx;

static void ex_after_heartbeat (FpiUsbTransfer *t,
                                FpDevice       *dev,
                                gpointer        ud,
                                GError         *err);
static void ex_after_prime (FpiUsbTransfer *t,
                            FpDevice       *dev,
                            gpointer        ud,
                            GError         *err);
static void ex_after_write (FpiUsbTransfer *t,
                            FpDevice       *dev,
                            gpointer        ud,
                            GError         *err);
static void ex_after_read (FpiUsbTransfer *t,
                           FpDevice       *dev,
                           gpointer        ud,
                           GError         *err);
static void ex_after_doorbell (FpiUsbTransfer *t,
                               FpDevice       *dev,
                               gpointer        ud,
                               GError         *err);

static void
ex_complete (FpDevice *dev, FpiSsm *ssm, ExchangeCtx *ctx, GError *err)
{
  ExchangeDone cb = ctx->done_cb;
  guint8 *buf = ctx->resp_buf;
  gsize n   = ctx->resp_actual;

  ctx->resp_buf = NULL;
  g_free (ctx);

  cb (dev, ssm, buf, n, err);
  g_free (buf);
}

static gboolean
ex_check_bail (FpiUsbTransfer *t, ExchangeCtx *ctx, GError *err)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (t->device);

  if (self->deactivating)
    {
      if (err)
        g_error_free (err);
      fpi_ssm_mark_completed (t->ssm);
      g_free (ctx->resp_buf);
      g_free (ctx);
      return TRUE;
    }
  if (err)
    {
      ex_complete (t->device, t->ssm, ctx, err);
      return TRUE;
    }
  return FALSE;
}

static void
bracketed_submit (FpDevice *dev, FpiSsm *ssm,
                  const guint8 *cmd, gsize cmd_len,
                  guint16 prime_len, gboolean doorbell,
                  ExchangeDone done_cb)
{
  ExchangeCtx *ctx = g_new0 (ExchangeCtx, 1);
  FpiUsbTransfer *t;

  ctx->cmd       = cmd;
  ctx->cmd_len   = cmd_len;
  ctx->prime_len = prime_len;
  ctx->doorbell  = doorbell;
  ctx->done_cb   = done_cb;

  t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0xc3, 0x0000, 0x0000, 16);
  memcpy (t->buffer, heartbeat_payload, 16);
  fpi_usb_transfer_submit (t, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ex_after_heartbeat, ctx);
}

static void
ex_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (ex_check_bail (t, ctx, err))
    return;

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0xca, 0x0003, ctx->prime_len, 0);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ex_after_prime, ctx);
}

static void
ex_after_prime (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (ex_check_bail (t, ctx, err))
    return;

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (n, SAMSUNG_EP_OUT,
                                   (guint8 *) ctx->cmd, ctx->cmd_len, NULL);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ex_after_write, ctx);
}

static void
ex_after_write (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (ex_check_bail (t, ctx, err))
    return;

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (n, SAMSUNG_EP_IN, ctx->prime_len);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ex_after_read, ctx);
}

static void
ex_after_read (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (ex_check_bail (t, ctx, err))
    return;

  ctx->resp_actual = t->actual_length;
  ctx->resp_buf = g_memdup2 (t->buffer, t->actual_length);

  if (!ctx->doorbell)
    {
      ex_complete (dev, t->ssm, ctx, NULL);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0xda, 0x0007, 0x0000, 2);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ex_after_doorbell, ctx);
}

static void
ex_after_doorbell (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (self->deactivating)
    {
      if (err)
        g_error_free (err);
      fpi_ssm_mark_completed (t->ssm);
      g_free (ctx->resp_buf);
      g_free (ctx);
      return;
    }

  fp_dbg ("samsung7305: doorbell %02x %02x",
          err ? 0 : t->buffer[0], err ? 0 : t->buffer[1]);
  ex_complete (dev, t->ssm, ctx, err);
}

/* ── Bracketed 0xc4 image exchange ──────────────────────────────────────── *
 *
 * The 0xc4 image read uses a concurrent EP1 OUT write + EP2 IN read after
 * the control-transfer preamble.  Concurrency is still needed because the
 * device starts staging image bytes on EP2 while the host is still writing
 * the padded command on EP1 (see STATE.md § "0xc4 image read command").
 */

typedef struct
{
  guint8                *cmd_buf;
  gsize                  len;
  FpiUsbTransferCallback read_cb;
} C4Ctx;

static void c4_after_heartbeat (FpiUsbTransfer *t,
                                FpDevice       *dev,
                                gpointer        ud,
                                GError         *err);
static void c4_after_prime (FpiUsbTransfer *t,
                            FpDevice       *dev,
                            gpointer        ud,
                            GError         *err);

static void
bracketed_c4_submit (FpDevice *dev, FpiSsm *ssm,
                     guint8 *cmd_buf, gsize len,
                     FpiUsbTransferCallback read_cb)
{
  C4Ctx *c = g_new0 (C4Ctx, 1);
  FpiUsbTransfer *t;

  c->cmd_buf = cmd_buf;
  c->len     = len;
  c->read_cb = read_cb;

  t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0xc3, 0x0000, 0x0000, 16);
  memcpy (t->buffer, heartbeat_payload, 16);
  fpi_usb_transfer_submit (t, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           c4_after_heartbeat, c);
}

static void
c4_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  C4Ctx *c    = ud;
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);
  FpiUsbTransfer *n;

  if (self->deactivating)
    {
      if (err)
        g_error_free (err);
      fpi_ssm_mark_completed (t->ssm);
      g_free (c);
      return;
    }
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      g_free (c);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0xca, 0x0003, (guint16) c->len, 0);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           c4_after_prime, c);
}

static void
c4_after_prime (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  C4Ctx *c    = ud;
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);
  FpiUsbTransfer *rd, *wr;

  if (self->deactivating)
    {
      if (err)
        g_error_free (err);
      fpi_ssm_mark_completed (t->ssm);
      g_free (c);
      return;
    }
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      g_free (c);
      return;
    }

  rd = fpi_usb_transfer_new (dev);
  rd->ssm = t->ssm;
  rd->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (rd, SAMSUNG_EP_IN, c->len);
  fpi_usb_transfer_submit (rd, IMG_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           c->read_cb, NULL);

  wr = fpi_usb_transfer_new (dev);
  wr->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk_full (wr, SAMSUNG_EP_OUT,
                                   c->cmd_buf, c->len, NULL);
  fpi_usb_transfer_submit (wr, IMG_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           img_cmd_write_cb, NULL);

  g_free (c);
}

/* ── EP2 drain (session-start flush + strip footer drain) ──────────────── */

static void
drain_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer unused, GError *error)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (self->deactivating)
    {
      if (error)
        g_error_free (error);
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  if (error)
    {
      fp_dbg ("samsung7305: drain complete (state %d): %s",
              fpi_ssm_get_cur_state (transfer->ssm), error->message);
      g_error_free (error);
      fpi_ssm_next_state (transfer->ssm);
    }
  else
    {
      fp_dbg ("samsung7305: drain: flushed %zu bytes (state %d)",
              transfer->actual_length,
              fpi_ssm_get_cur_state (transfer->ssm));
      fpi_ssm_jump_to_state (transfer->ssm,
                             fpi_ssm_get_cur_state (transfer->ssm));
    }
}

/* ── Per-state done callbacks ──────────────────────────────────────────── */

static void
init_done_cb (FpDevice *dev, FpiSsm *ssm,
              const guint8 *resp, gsize n, GError *err)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  fp_dbg ("samsung7305: init[%u] read %zu bytes: %02x %02x",
          self->seq_idx, n,
          n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0);

  self->seq_idx++;
  if (self->seq_idx < G_N_ELEMENTS (init_seq))
    run_init_seq (self);
  else
    fpi_ssm_next_state (ssm);
}

static void
run_init_seq (FpiDeviceSamsung7305 *self)
{
  const SamsungCmd *c = &init_seq[self->seq_idx];
  /* 0xf8 (reset) and 0x28 (init-complete) require the 0xda doorbell. */
  gboolean doorbell = (c->data[0] == 0xf8 || c->data[0] == 0x28);

  bracketed_submit (FP_DEVICE (self), self->ssm,
                    c->data, c->len,
                    (guint16) c->len, doorbell,
                    init_done_cb);
}

/* Poll arm (0x20): doorbell required. */
static void
arm_done_cb (FpDevice *dev, FpiSsm *ssm,
             const guint8 *resp, gsize n, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fp_dbg ("samsung7305: arm %zu bytes", n);
  fpi_ssm_next_state (ssm);
}

/* Poll status (0x1c 0x00): check finger-present bit. */
static void
status_done_cb (FpDevice *dev, FpiSsm *ssm,
                const guint8 *resp, gsize n, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fp_dbg ("samsung7305: status %zu bytes: %02x %02x",
          n, n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0);
  if (n >= 2 && resp[1] == STATUS_FINGER)
    fpi_ssm_next_state (ssm);
  else
    fpi_ssm_jump_to_state_delayed (ssm, M_POLL_ARM, POLL_DELAY);
}

/* Capacitive read (0xd4): require full saturation.
 * If the finger has lifted after at least one frame was captured, assemble
 * the collected frames instead of looping. */
static void
cap_check_done_cb (FpDevice *dev, FpiSsm *ssm,
                   const guint8 *resp, gsize n, GError *err)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fp_dbg ("samsung7305: cap_check %zu bytes: %02x %02x %02x %02x %02x (frames=%u)",
          n,
          n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0,
          n > 2 ? resp[2] : 0, n > 3 ? resp[3] : 0,
          n > 4 ? resp[4] : 0, self->frames_captured);
  if (n >= 3 && resp[1] == 0x0f && resp[2] == 0xff)
    {
      if (self->frames_captured >= MAX_FRAMES)
        fpi_ssm_jump_to_state (ssm, M_ASSEMBLE);
      else
        fpi_ssm_next_state (ssm);
    }
  else if (self->frames_captured > 0)
    {
      fpi_ssm_jump_to_state (ssm, M_ASSEMBLE);
    }
  else
    {
      fpi_ssm_jump_to_state_delayed (ssm, M_POLL_ARM, POLL_DELAY);
    }
}

/* Capture-setup-sequence loop (cap_setup_seq or ff_setup_seq). */
static const SamsungCmd *
active_cap_seq (FpiDeviceSamsung7305 *self, gsize *out_len)
{
  if (fpi_ssm_get_cur_state (self->ssm) == M_FF_SETUP)
    {
      *out_len = G_N_ELEMENTS (ff_setup_seq);
      return ff_setup_seq;
    }
  *out_len = G_N_ELEMENTS (cap_setup_seq);
  return cap_setup_seq;
}

static void
cap_seq_done_cb (FpDevice *dev, FpiSsm *ssm,
                 const guint8 *resp, gsize n, GError *err)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);
  gsize seq_len;

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  fp_dbg ("samsung7305: cap_seq[%u] %zu bytes", self->seq_idx, n);
  self->seq_idx++;
  active_cap_seq (self, &seq_len);
  if (self->seq_idx < seq_len)
    run_cap_seq (self);
  else
    fpi_ssm_next_state (ssm);
}

static void
run_cap_seq (FpiDeviceSamsung7305 *self)
{
  gsize seq_len;
  const SamsungCmd *seq = active_cap_seq (self, &seq_len);
  const SamsungCmd *c   = &seq[self->seq_idx];

  bracketed_submit (FP_DEVICE (self), self->ssm,
                    c->data, c->len,
                    (guint16) c->len, FALSE,
                    cap_seq_done_cb);
}

/* Simple oneshot: advance SSM on completion. Used for trigger (with
 * doorbell), between-strips, and the re-poll triple. */
static void
oneshot_done_cb (FpDevice *dev, FpiSsm *ssm,
                 const guint8 *resp, gsize n, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fp_dbg ("samsung7305: oneshot %zu bytes (state %d)",
          n, fpi_ssm_get_cur_state (ssm));
  fpi_ssm_next_state (ssm);
}

/* cap_wait: poll status until STATUS_READY bit set. */
static void
cap_wait_done_cb (FpDevice *dev, FpiSsm *ssm,
                  const guint8 *resp, gsize n, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fp_dbg ("samsung7305: cap_wait %zu bytes: %02x %02x (state %d)",
          n, n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0,
          fpi_ssm_get_cur_state (ssm));
  if (n >= 2 && (resp[1] & STATUS_READY))
    fpi_ssm_next_state_delayed (ssm, 5);
  else
    fpi_ssm_jump_to_state_delayed (ssm,
                                   fpi_ssm_get_cur_state (ssm),
                                   POLL_DELAY);
}

/* ── Image read callbacks (shared, used by M_STRIP*_READ / M_FRAME_READ) ─ */

static void
img_cmd_write_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer unused, GError *error)
{
  if (error)
    {
      fp_warn ("samsung7305: 0xc4 write error: %s", error->message);
      g_error_free (error);
    }
  else
    {
      fp_dbg ("samsung7305: 0xc4 write complete (%zu bytes)",
              transfer->actual_length);
    }
}

static void
strip_image_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                     gpointer unused, GError *error)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (check_deactivating (transfer, error))
    return;

  {
    gsize to_copy = MIN ((gsize) transfer->actual_length,
                         (gsize) (STRIP_SIZE - self->strip_bytes_received));
    if (to_copy > 0 && self->cur_strip < 3)
      memcpy (self->img_strip[self->cur_strip] + self->strip_bytes_received,
              transfer->buffer, to_copy);
  }
  self->strip_bytes_received += (guint) transfer->actual_length;
  fp_dbg ("samsung7305: strip chunk %zu total=%u/%d",
          (gsize) transfer->actual_length,
          self->strip_bytes_received, STRIP_SIZE);

  if (self->strip_bytes_received < STRIP_SIZE)
    {
      FpiUsbTransfer *rd = fpi_usb_transfer_new (dev);
      rd->ssm = transfer->ssm;
      rd->short_is_error = FALSE;
      fpi_usb_transfer_fill_bulk (rd, SAMSUNG_EP_IN, DRAIN_BUF_SIZE);
      fpi_usb_transfer_submit (rd, IMG_TIMEOUT,
                               fpi_device_get_cancellable (dev),
                               strip_image_read_cb, NULL);
    }
  else
    {
      fpi_ssm_next_state (transfer->ssm);
    }
}

static void
image_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer unused, GError *error)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);
  gsize to_copy;

  if (check_deactivating (transfer, error))
    return;

  to_copy = MIN ((gsize) transfer->actual_length,
                 (gsize) (FRAME_SIZE - self->frame_bytes_received));
  if (to_copy > 0)
    memcpy (self->img_buf + self->frame_bytes_received,
            transfer->buffer, to_copy);
  self->frame_bytes_received += (guint) transfer->actual_length;

  fp_dbg ("samsung7305: frame chunk %zu total=%u/%d",
          (gsize) transfer->actual_length,
          self->frame_bytes_received, FRAME_SIZE);

  if (self->frame_bytes_received < FRAME_SIZE - 2)
    {
      FpiUsbTransfer *rd = fpi_usb_transfer_new (dev);
      rd->ssm = transfer->ssm;
      rd->short_is_error = FALSE;
      fpi_usb_transfer_fill_bulk (rd, SAMSUNG_EP_IN, DRAIN_BUF_SIZE);
      fpi_usb_transfer_submit (rd, IMG_TIMEOUT,
                               fpi_device_get_cancellable (dev),
                               image_read_cb, NULL);
    }
  else
    {
      fpi_ssm_next_state (transfer->ssm);
    }
}

/* ── SSM state handler ──────────────────────────────────────────────────── */

static void
m_loop_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);
  FpImageDevice *img  = FP_IMAGE_DEVICE (dev);

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_DRAIN:
      {
        FpiUsbTransfer *rd = fpi_usb_transfer_new (dev);
        rd->ssm = ssm;
        rd->short_is_error = FALSE;
        fpi_usb_transfer_fill_bulk (rd, SAMSUNG_EP_IN, DRAIN_BUF_SIZE);
        fpi_usb_transfer_submit (rd, DRAIN_TIMEOUT,
                                 fpi_device_get_cancellable (dev),
                                 drain_read_cb, NULL);
      }
      break;

    case M_INIT:
      self->seq_idx = 0;
      run_init_seq (self);
      break;

    case M_POLL_ARM:
      bracketed_submit (dev, ssm, cmd_arm, G_N_ELEMENTS (cmd_arm),
                        (guint16) G_N_ELEMENTS (cmd_arm), TRUE,
                        arm_done_cb);
      break;

    case M_POLL_STATUS:
      bracketed_submit (dev, ssm, cmd_status, G_N_ELEMENTS (cmd_status),
                        (guint16) G_N_ELEMENTS (cmd_status), FALSE,
                        status_done_cb);
      break;

    case M_POLL_CAP:
      bracketed_submit (dev, ssm, cmd_cap_rd, G_N_ELEMENTS (cmd_cap_rd),
                        (guint16) G_N_ELEMENTS (cmd_cap_rd), FALSE,
                        cap_check_done_cb);
      break;

    case M_CAP_SETUP:
      fpi_image_device_report_finger_status (img, TRUE);
      self->seq_idx = 0;
      run_cap_seq (self);
      break;

    case M_FF_SETUP:
      self->seq_idx = 0;
      run_cap_seq (self);
      break;

    case M_TRIGGER1:
    case M_TRIGGER2:
    case M_TRIGGER3:
    case M_TRIGGER4:
      bracketed_submit (dev, ssm, cmd_trigger, G_N_ELEMENTS (cmd_trigger),
                        (guint16) G_N_ELEMENTS (cmd_trigger), TRUE,
                        oneshot_done_cb);
      break;

    case M_BETWEEN_S1_S2:
      bracketed_submit (dev, ssm,
                        cmd_between_strips, G_N_ELEMENTS (cmd_between_strips),
                        (guint16) G_N_ELEMENTS (cmd_between_strips), FALSE,
                        oneshot_done_cb);
      break;

    case M_REPOLL_ARM:
      bracketed_submit (dev, ssm, cmd_arm, G_N_ELEMENTS (cmd_arm),
                        (guint16) G_N_ELEMENTS (cmd_arm), TRUE,
                        oneshot_done_cb);
      break;

    case M_REPOLL_STATUS:
      bracketed_submit (dev, ssm, cmd_status, G_N_ELEMENTS (cmd_status),
                        (guint16) G_N_ELEMENTS (cmd_status), FALSE,
                        oneshot_done_cb);
      break;

    case M_REPOLL_CAP:
      bracketed_submit (dev, ssm, cmd_cap_rd, G_N_ELEMENTS (cmd_cap_rd),
                        (guint16) G_N_ELEMENTS (cmd_cap_rd), FALSE,
                        oneshot_done_cb);
      break;

    case M_WAIT1:
    case M_WAIT2:
    case M_WAIT3:
    case M_WAIT4:
      bracketed_submit (dev, ssm, cmd_status, G_N_ELEMENTS (cmd_status),
                        (guint16) G_N_ELEMENTS (cmd_status), FALSE,
                        cap_wait_done_cb);
      break;

    case M_STRIP1_READ:
    case M_STRIP2_READ:
    case M_STRIP3_READ:
      self->strip_bytes_received = 0;
      self->cur_strip = (fpi_ssm_get_cur_state (ssm) == M_STRIP1_READ) ? 0 :
                        (fpi_ssm_get_cur_state (ssm) == M_STRIP2_READ) ? 1 : 2;
      memset (self->img_strip[self->cur_strip], 0, STRIP_SIZE);
      bracketed_c4_submit (dev, ssm, self->img_cmd_buf, STRIP_SIZE + 2,
                           strip_image_read_cb);
      break;

    case M_STRIP1_FTR:
    case M_STRIP2_FTR:
    case M_STRIP3_FTR:
      {
        FpiUsbTransfer *rd = fpi_usb_transfer_new (dev);
        rd->ssm = ssm;
        rd->short_is_error = FALSE;
        fpi_usb_transfer_fill_bulk (rd, SAMSUNG_EP_IN, DRAIN_BUF_SIZE);
        fpi_usb_transfer_submit (rd, DRAIN_TIMEOUT,
                                 fpi_device_get_cancellable (dev),
                                 drain_read_cb, NULL);
      }
      break;

    case M_FRAME_READ:
      self->frame_bytes_received = 0;
      bracketed_c4_submit (dev, ssm, self->img_cmd_buf, FRAME_SIZE + 2,
                           image_read_cb);
      break;

    case M_STORE_FRAME:
      {
        struct fpi_frame *fr = g_malloc0 (sizeof (struct fpi_frame) + FRAME_SIZE);
        memcpy (fr->data, self->img_buf + 2, FRAME_SIZE - 2);
        self->frames = g_slist_prepend (self->frames, fr);
        self->frames_captured++;
        fp_dbg ("samsung7305: stored frame %u", self->frames_captured);
        if (self->frames_captured >= MAX_FRAMES)
          fpi_ssm_jump_to_state (ssm, M_ASSEMBLE);
        else
          fpi_ssm_jump_to_state (ssm, M_POLL_ARM);
      }
      break;

    case M_ASSEMBLE:
      {
        FpImage *assembled;
        GSList *ordered = g_slist_reverse (self->frames);
        self->frames = NULL;

        if (self->frames_captured < MIN_FRAMES)
          {
            fp_dbg ("samsung7305: too few frames (%u < %u), requesting retry",
                    self->frames_captured, MIN_FRAMES);
            g_slist_free_full (ordered, g_free);
            self->frames_captured = 0;
            fpi_image_device_retry_scan (img, FP_DEVICE_RETRY_TOO_SHORT);
            fpi_image_device_report_finger_status (img, FALSE);
            fpi_ssm_mark_completed (ssm);
            return;
          }

        /* Drop the first frame (partial contact / AGC settle), then keep
         * exactly ASSEMBLE_FRAMES so the assembled geometry is identical
         * between enroll and verify regardless of how many frames the
         * capture loop collected. */
        {
          GSList *head = ordered;
          ordered = ordered->next;
          head->next = NULL;
          g_slist_free_full (head, g_free);
          self->frames_captured--;

          GSList *keep_tail = ordered;
          for (guint i = 1; i < ASSEMBLE_FRAMES && keep_tail; i++)
            keep_tail = keep_tail->next;
          if (keep_tail && keep_tail->next)
            {
              g_slist_free_full (keep_tail->next, g_free);
              keep_tail->next = NULL;
            }
          self->frames_captured = MIN (self->frames_captured, ASSEMBLE_FRAMES);
          fp_dbg ("samsung7305: dropped first + truncated, assembling %u",
                  self->frames_captured);
        }

        if (self->frames_captured >= 2)
          {
            /* Fixed stride instead of motion estimation — see the note
             * near ASSEMBLE_STRIDE. First frame's delta_y is forced to 0
             * inside fpi_assemble_frames. */
            for (GSList *l = ordered->next; l != NULL; l = l->next)
              {
                struct fpi_frame *f = l->data;
                f->delta_x = 0;
                f->delta_y = ASSEMBLE_STRIDE;
              }
            assembled = fpi_assemble_frames (&assembling_ctx, ordered);
          }
        else if (ordered)
          {
            struct fpi_frame *only = ordered->data;
            assembled = fp_image_new (FRAME_WIDTH, FRAME_HEIGHT);
            memcpy (assembled->data, only->data, FRAME_SIZE);
          }
        else
          {
            assembled = NULL;
          }

        g_slist_free_full (ordered, g_free);
        self->frames_captured = 0;

        if (!assembled)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
            return;
          }

        assembled->ppmm = 19.69;
        fp_dbg ("samsung7305: assembled image %ux%u",
                assembled->width, assembled->height);

        fpi_image_device_image_captured (img, assembled);
        fpi_image_device_report_finger_status (img, FALSE);
        fpi_ssm_mark_completed (ssm);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

/* ── SSM completion ─────────────────────────────────────────────────────── */

static void
m_loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *img  = FP_IMAGE_DEVICE (dev);
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  /* report_finger_status(FALSE) on the completing SSM can synchronously
   * drive fprintd's state machine into AWAIT_FINGER_ON, which invokes
   * samsung_dev_change_state and installs a new SSM on self->ssm before
   * this completion callback fires. Only null the pointer if it is still
   * referring to the SSM that just completed, so we don't clobber the
   * successor. */
  if (self->ssm == ssm)
    self->ssm = NULL;

  if (self->frames)
    {
      g_slist_free_full (g_steal_pointer (&self->frames), g_free);
      self->frames_captured = 0;
    }

  if (self->deactivating)
    fpi_image_device_deactivate_complete (img, error);
  else if (error)
    fpi_image_device_session_error (img, error);
}

/* ── Session-level vendor control transfers ─────────────────────────────── *
 *
 * Once per device open/close (PROTOCOL.md → Transport).  Synchronous —
 * issued outside the SSM.
 */

static gboolean
session_ctrl_out (GUsbDevice *usb, guint16 value, guint16 idx, GError **error)
{
  return g_usb_device_control_transfer (usb,
                                        G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                        G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                        G_USB_DEVICE_RECIPIENT_DEVICE,
                                        0xdb, value, idx,
                                        NULL, 0, NULL,
                                        CMD_TIMEOUT, NULL, error);
}

/* ── Device lifecycle ───────────────────────────────────────────────────── */

static void
samsung_dev_init (FpImageDevice *dev)
{
  FpiDeviceSamsung7305 *self  = FPI_DEVICE_SAMSUNG7305 (dev);
  GUsbDevice *usb   = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;

  /* NOTE: g_usb_device_reset() was previously invoked here to clear stale
   * EP2 data between sessions, but it also resets the device's AGC/calibration
   * state, producing bit-wise-different captures of the same finger across
   * fprintd sessions (enroll vs. verify). The M_DRAIN loop in the SSM now
   * handles stale data without touching firmware state. */

  g_usb_device_claim_interface (usb, 0, 0, &error);
  if (error)
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  /* Session-open vendor control transfers: 0xdb wValue=0x0006 wIndex=0,1
   * then 0xdb wValue=0x000b wIndex=0,1.  Non-fatal on failure — log only. */
  {
    GError *werr = NULL;
    if (!session_ctrl_out (usb, 0x0006, 0, &werr))
      {
        fp_warn ("samsung7305: session open 0xdb/6/0 failed: %s",
                 werr->message);
        g_clear_error (&werr);
      }
    if (!session_ctrl_out (usb, 0x0006, 1, &werr))
      {
        fp_warn ("samsung7305: session open 0xdb/6/1 failed: %s",
                 werr->message);
        g_clear_error (&werr);
      }
    if (!session_ctrl_out (usb, 0x000b, 0, &werr))
      {
        fp_warn ("samsung7305: session open 0xdb/b/0 failed: %s",
                 werr->message);
        g_clear_error (&werr);
      }
    if (!session_ctrl_out (usb, 0x000b, 1, &werr))
      {
        fp_warn ("samsung7305: session open 0xdb/b/1 failed: %s",
                 werr->message);
        g_clear_error (&werr);
      }
  }

  self->img_buf     = g_malloc0 (FRAME_SIZE + 2);
  self->img_cmd_buf = g_malloc0 (FRAME_SIZE + 2);
  self->img_cmd_buf[0] = 0xc4;
  for (guint s = 0; s < 3; s++)
    self->img_strip[s] = g_malloc0 (STRIP_SIZE);

  fpi_image_device_open_complete (dev, NULL);
}

static void
samsung_dev_deinit (FpImageDevice *dev)
{
  FpiDeviceSamsung7305 *self  = FPI_DEVICE_SAMSUNG7305 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->img_buf,     g_free);
  g_clear_pointer (&self->img_cmd_buf, g_free);
  for (guint s = 0; s < 3; s++)
    g_clear_pointer (&self->img_strip[s], g_free);
  if (self->frames)
    {
      g_slist_free_full (g_steal_pointer (&self->frames), g_free);
      self->frames_captured = 0;
    }

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
samsung_dev_activate (FpImageDevice *dev)
{
  FPI_DEVICE_SAMSUNG7305 (dev)->deactivating = FALSE;
  fpi_image_device_activate_complete (dev, NULL);
}

static void
samsung_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  self->deactivating = TRUE;
  if (self->ssm == NULL)
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
samsung_dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceSamsung7305 *self = FPI_DEVICE_SAMSUNG7305 (dev);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), m_loop_state, M_NUM_STATES);
      self->ssm = ssm;
      fpi_ssm_start (ssm, m_loop_complete);
    }
}

/* ── USB ID table ───────────────────────────────────────────────────────── */

static const FpIdEntry id_table[] = {
  { .vid = 0x04e8, .pid = 0x7305 },
  { .vid = 0,      .pid = 0      },
};

/* ── GObject boilerplate ────────────────────────────────────────────────── */

static void
fpi_device_samsung7305_init (FpiDeviceSamsung7305 *self)
{
}

static void
fpi_device_samsung7305_class_init (FpiDeviceSamsung7305Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id        = FP_COMPONENT;
  dev_class->full_name = "Samsung Fingerprint Device (04e8:7305)";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  img_class->img_width     = FRAME_WIDTH;
  img_class->img_height    = FRAME_HEIGHT;
  img_class->bz3_threshold = 10;

  img_class->img_open     = samsung_dev_init;
  img_class->img_close    = samsung_dev_deinit;
  img_class->activate     = samsung_dev_activate;
  img_class->deactivate   = samsung_dev_deactivate;
  img_class->change_state = samsung_dev_change_state;
}
