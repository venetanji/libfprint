/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 *
 * Copyright (C) 2026 Filip Spanne (original driver, 04f3:0c6e)
 * Copyright (C) 2026 Mohammad Nauman (04f3:0c3d, full-frame capture,
 *                                    multi-frame touches, rotation matcher)
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
 */

#define FP_COMPONENT "elanpress"

#include "drivers_api.h"
#include "elanpress.h"
#include "elanpress-match.h"

#include <math.h>
#include <stdlib.h>

static const guint8 cmd_get_fw_ver[ELANPRESS_CMD_LEN] = {0x40, 0x19};
static const guint8 cmd_get_sensor_dim[ELANPRESS_CMD_LEN] = {0x00, 0x0c};
static const guint8 cmd_get_calib_mean[ELANPRESS_CMD_LEN] = {0x40, 0x24};
static const guint8 cmd_led_on[ELANPRESS_CMD_LEN] = {0x40, 0x31};
static const guint8 cmd_pre_scan[ELANPRESS_CMD_LEN] = {0x40, 0x3f};
static const guint8 cmd_get_image[ELANPRESS_CMD_LEN] = {0x00, 0x09};
static const guint8 cmd_stop[ELANPRESS_CMD_LEN] = {0x00, 0x0b};

struct _FpiDeviceElanPress
{
  FpDevice             parent;

  guint16              fw_ver;
  int                  frame_width;
  int                  frame_height;

  ElanpressMatchParams params;
  double               min_mean;
  double               min_contrast;
  double               min_coverage;
  char                *dump_dir;
  char                *dump_session;
  int                  dump_touch;

  /* raw background frame (no finger), row-major */
  guint16 *background;
  guint16  calib_mean;
  int      bg_attempts;

  /* raw frames of the touch being captured, newest first */
  GSList  *frames;
  int      num_frames;
  guint8   finger_byte;
  gboolean wait_timed_out;

  /* processed images collected during enrollment */
  GPtrArray *enroll_images;
  int        enroll_stage;
  int        enroll_stages;
};

G_DEFINE_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FP_TYPE_DEVICE);

/* === helpers === */

static unsigned int
elanpress_frame_size (FpiDeviceElanPress *self)
{
  return (unsigned int) self->frame_width * self->frame_height;
}

static void
elanpress_dump16 (FpiDeviceElanPress *self, const char *tag, int idx,
                  const guint16 *px)
{
  g_autofree char *path = NULL;

  if (!self->dump_dir)
    return;
  path = g_strdup_printf ("%s/%s-%s-t%03d-f%02d.pgm", self->dump_dir, tag,
                          self->dump_session, self->dump_touch, idx);
  if (!elanpress_pgm_write16 (path, px, self->frame_width, self->frame_height))
    fp_warn ("could not write %s", path);
}

static void
elanpress_dump8 (FpiDeviceElanPress *self, const char *tag, int idx,
                 const guint8 *px)
{
  g_autofree char *path = NULL;

  if (!self->dump_dir)
    return;
  path = g_strdup_printf ("%s/%s-%s-t%03d-f%02d.pgm", self->dump_dir, tag,
                          self->dump_session, self->dump_touch, idx);
  if (!elanpress_pgm_write8 (path, px, self->frame_width, self->frame_height))
    fp_warn ("could not write %s", path);
}

/* === image processing === */

typedef struct
{
  guint8          *img;
  ElanpressQuality q;
  int              idx;
} Processed;

static void
processed_free (gpointer p)
{
  Processed *pr = p;

  g_free (pr->img);
  g_free (pr);
}

static gint
processed_cmp (gconstpointer a, gconstpointer b)
{
  const Processed *pa = *(Processed * const *) a;
  const Processed *pb = *(Processed * const *) b;
  double sa = pa->q.contrast * pa->q.coverage;
  double sb = pb->q.contrast * pb->q.coverage;

  return sa < sb ? 1 : sa > sb ? -1 : 0;
}

/* Process every frame of the touch, drop the ones that fail the quality
 * gates, then keep up to max_keep mutually distinct images, best first.
 * The finger settles and often rolls slightly during a press, so a touch
 * can yield several genuinely different patches of finger for free. */
static GPtrArray *
elanpress_process_touch (FpiDeviceElanPress *self, int max_keep)
{
  int w = self->frame_width, h = self->frame_height;

  g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func (processed_free);
  GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
  int idx = self->num_frames;
  double best_contrast = 0;

  /* the list is newest-first; number frames in capture order */
  for (GSList *l = self->frames; l; l = l->next)
    {
      Processed *p = g_new0 (Processed, 1);

      idx--;
      p->idx = idx;
      p->img = elanpress_process_frame (l->data, self->background, w, h,
                                        self->params.norm_win, &p->q);
      elanpress_dump16 (self, "raw", idx, l->data);
      fp_dbg ("frame %d: mean %.0f contrast %.1f coverage %.2f",
              idx, p->q.mean, p->q.contrast, p->q.coverage);
      best_contrast = MAX (best_contrast, p->q.contrast);
      g_ptr_array_add (all, p);
    }

  g_ptr_array_sort (all, processed_cmp);

  for (guint i = 0; i < all->len && (int) keep->len < max_keep; i++)
    {
      Processed *p = g_ptr_array_index (all, i);
      gboolean dup = FALSE;

      if (p->q.mean < self->min_mean || p->q.contrast < self->min_contrast ||
          p->q.coverage < self->min_coverage ||
          p->q.contrast < 0.5 * best_contrast)
        {
          fp_dbg ("frame %d rejected by quality gate", p->idx);
          continue;
        }

      for (guint j = 0; j < keep->len; j++)
        {
          double s = elanpress_similarity (p->img, g_ptr_array_index (keep, j), w, h);

          if (s > ELANPRESS_DUP_SIMILARITY)
            {
              dup = TRUE;
              break;
            }
        }
      if (dup)
        continue;

      elanpress_dump8 (self, "img", p->idx, p->img);
      g_ptr_array_add (keep, g_steal_pointer (&p->img));
    }

  fp_dbg ("touch %d: %d frames, kept %u images", self->dump_touch,
          self->num_frames, keep->len);
  self->dump_touch++;
  return keep;
}

/* === print (de)serialization === */

static void
elanpress_print_set_data (FpiDeviceElanPress *self, FpPrint *print,
                          GPtrArray *images)
{
  GVariantBuilder images_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aay"));
  GVariant *fpi_data;

  for (guint i = 0; i < images->len; i++)
    g_variant_builder_add_value (
      &images_builder,
      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                 g_ptr_array_index (images, i),
                                 elanpress_frame_size (self),
                                 sizeof (guint8)));

  fpi_data = g_variant_new ("(qqqaay)",
                            (guint16) ELANPRESS_PRINT_VERSION,
                            (guint16) self->frame_width,
                            (guint16) self->frame_height,
                            &images_builder);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

/* returns the stored images or NULL if the print is not usable */
static GPtrArray *
elanpress_print_get_images (FpiDeviceElanPress *self, FpPrint *print)
{
  g_autoptr(GVariant) fpi_data = NULL;
  g_autoptr(GVariant) images_v = NULL;
  GPtrArray *images;
  guint16 version = 0, width = 0, height = 0;
  gsize n_images;

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  if (!fpi_data || !g_variant_check_format_string (fpi_data, "(qqq@aay)", FALSE))
    return NULL;

  g_variant_get (fpi_data, "(qqq@aay)", &version, &width, &height, &images_v);
  if (version != ELANPRESS_PRINT_VERSION ||
      width != self->frame_width || height != self->frame_height)
    {
      fp_warn ("print version %d / %dx%d does not match driver %d / %dx%d",
               version, width, height, ELANPRESS_PRINT_VERSION,
               self->frame_width, self->frame_height);
      return NULL;
    }

  n_images = g_variant_n_children (images_v);
  if (n_images == 0)
    return NULL;

  images = g_ptr_array_new_full (n_images, g_free);
  for (gsize i = 0; i < n_images; i++)
    {
      g_autoptr(GVariant) img_v = g_variant_get_child_value (images_v, i);
      gsize len = 0;
      gconstpointer data = g_variant_get_fixed_array (img_v, &len, sizeof (guint8));

      if (len != elanpress_frame_size (self))
        {
          g_ptr_array_unref (images);
          return NULL;
        }
      g_ptr_array_add (images, g_memdup2 (data, len));
    }

  return images;
}

/* Matching is spread over the CPUs: each worker takes every n-th template
 * image and scores all probes against it; the first worker to reach the
 * threshold flags the others to stop. */
typedef struct
{
  GPtrArray           *probes;   /* ElanpressProbe*, shared read-only */
  GPtrArray           *images;   /* guint8*, shared read-only */
  guint                start, step;
  double               threshold;
  volatile gint       *done;
  double               best;
  ElanpressMatchResult res;
  int                  best_probe, best_image;
} MatchJob;

static gpointer
elanpress_match_worker (gpointer data)
{
  MatchJob *job = data;

  job->best = -1.0;
  for (guint i = job->start; i < job->images->len; i += job->step)
    {
      for (guint p = 0; p < job->probes->len; p++)
        {
          ElanpressMatchResult r;
          double s;

          if (g_atomic_int_get (job->done))
            return NULL;
          s = elanpress_probe_match (g_ptr_array_index (job->probes, p),
                                     g_ptr_array_index (job->images, i), &r);
          if (s > job->best)
            {
              job->best = s;
              job->res = r;
              job->best_probe = p;
              job->best_image = i;
            }
          if (s >= job->threshold)
            {
              g_atomic_int_set (job->done, 1);
              return NULL;
            }
        }
    }
  return NULL;
}

/* best score of any probe image against any image of the print; stops as
 * soon as the threshold is reached. Returns -1 if the print is unusable. */
static double
elanpress_match_print (FpiDeviceElanPress *self, GPtrArray *probe_imgs,
                       FpPrint *print, ElanpressMatchResult *best_res)
{
  g_autoptr(GPtrArray) images = elanpress_print_get_images (self, print);
  g_autoptr(GPtrArray) probes = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_probe_free);
  guint n_threads;
  MatchJob *jobs;
  GThread **threads;
  volatile gint done = 0;
  double best = -1.0;
  gint64 t0 = g_get_monotonic_time ();

  if (!images)
    return -1.0;

  for (guint p = 0; p < probe_imgs->len; p++)
    g_ptr_array_add (probes, elanpress_probe_new (g_ptr_array_index (probe_imgs, p),
                                                  self->frame_width,
                                                  self->frame_height,
                                                  &self->params));

  n_threads = CLAMP (g_get_num_processors (), 1, 16);
  n_threads = MIN (n_threads, images->len);
  jobs = g_new0 (MatchJob, n_threads);
  threads = g_new0 (GThread *, n_threads);
  for (guint t = 0; t < n_threads; t++)
    {
      jobs[t].probes = probes;
      jobs[t].images = images;
      jobs[t].start = t;
      jobs[t].step = n_threads;
      jobs[t].threshold = self->params.threshold;
      jobs[t].done = &done;
      threads[t] = g_thread_new ("elanpress-match", elanpress_match_worker, &jobs[t]);
    }
  for (guint t = 0; t < n_threads; t++)
    {
      g_thread_join (threads[t]);
      if (jobs[t].best > best)
        {
          best = jobs[t].best;
          if (best_res)
            *best_res = jobs[t].res;
          fp_dbg ("best so far: probe %d vs image %d: %.3f (dx %d dy %d rot %.0f n %d)",
                  jobs[t].best_probe, jobs[t].best_image, best, jobs[t].res.dx,
                  jobs[t].res.dy, jobs[t].res.rot, jobs[t].res.overlap);
        }
    }
  fp_dbg ("matched %u probes against %u images on %u threads in %.0f ms",
          probes->len, images->len, n_threads,
          (g_get_monotonic_time () - t0) / 1000.0);

  g_free (jobs);
  g_free (threads);
  return best;
}

/* === USB helpers === */

static void
elanpress_reset_capture (FpiDeviceElanPress *self)
{
  g_slist_free_full (g_steal_pointer (&self->frames), g_free);
  self->num_frames = 0;
}

static void
elanpress_send_cmd (FpiSsm *ssm, FpDevice *dev, const guint8 *cmd)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd, ELANPRESS_CMD_LEN, NULL);
  fpi_usb_transfer_submit (transfer, ELANPRESS_CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
elanpress_read (FpiSsm *ssm, FpDevice *dev, guint8 ep, gsize len,
                guint timeout, FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (transfer, ep, len);
  fpi_usb_transfer_submit (transfer, timeout,
                           fpi_device_get_cancellable (dev),
                           callback, NULL);
}

/* === touch capture state machine === */

enum capture_states {
  CAPTURE_STOP,
  CAPTURE_LED_ON,
  CAPTURE_CALIB_MEAN_SEND,
  CAPTURE_CALIB_MEAN_READ,
  CAPTURE_BG_SEND,
  CAPTURE_BG_READ,
  CAPTURE_BG_CHECK,
  CAPTURE_WAIT_ON_SEND,
  CAPTURE_WAIT_ON_READ,
  CAPTURE_FRAME_DECIDE,
  CAPTURE_FRAME_READ,
  CAPTURE_FRAME_NEXT,
  CAPTURE_NUM_STATES,
};

static void
elanpress_status_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                     gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->wait_timed_out = FALSE;
  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          /* the device blocks the pre-scan read until a finger arrives (or
           * for a while after it leaves); nothing to report yet */
          g_error_free (error);
          self->finger_byte = 0;
          self->wait_timed_out = TRUE;
          fpi_ssm_next_state (transfer->ssm);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->finger_byte = transfer->buffer[0];
  fpi_ssm_next_state (transfer->ssm);
}

static void
elanpress_calib_mean_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                         gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* same arithmetic as the elan driver */
  self->calib_mean = transfer->buffer[0] * 0xff + transfer->buffer[1];
  fpi_ssm_next_state (transfer->ssm);
}

static void
elanpress_frame_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  guint16 *frame;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  frame = g_malloc (elanpress_frame_size (self) * sizeof (guint16));
  elanpress_rotate_frame (transfer->buffer, frame,
                          self->frame_width, self->frame_height);

  if (fpi_ssm_get_cur_state (transfer->ssm) == CAPTURE_BG_READ)
    {
      g_free (self->background);
      self->background = frame;
    }
  else
    {
      self->frames = g_slist_prepend (self->frames, frame);
      self->num_frames++;
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* A background frame is only usable if no finger was on the pad when it
 * was taken. The device's own calibration mean is what the elan driver
 * compares against, and a finger raises the frame mean by ~1500 on 0c3d;
 * this is also how we notice that the previous touch has not lifted yet. */
static gboolean
elanpress_background_ok (FpiDeviceElanPress *self)
{
  double mean, sd, delta;

  elanpress_frame_stats (self->background, self->frame_width,
                         self->frame_height, &mean, &sd);
  if (self->fw_ver < ELANPRESS_MIN_CALIBRATION_FW)
    {
      fp_dbg ("background mean %.0f sd %.0f (no calibration mean on this fw)",
              mean, sd);
      return TRUE;
    }
  delta = fabs (mean - self->calib_mean);
  fp_dbg ("background mean %.0f sd %.0f, calibration mean %d, delta %.0f",
          mean, sd, self->calib_mean, delta);
  return delta <= ELANPRESS_BG_MAX_DELTA;
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  gsize frame_bytes = elanpress_frame_size (self) * 2;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_STOP:
      /* cancels any pre-scan still pending in the device from a previous
       * touch or a timed-out wait, so the next read is not a stale byte */
      elanpress_send_cmd (ssm, dev, cmd_stop);
      break;

    case CAPTURE_LED_ON:
      elanpress_send_cmd (ssm, dev, cmd_led_on);
      break;

    case CAPTURE_CALIB_MEAN_SEND:
      if (self->fw_ver < ELANPRESS_MIN_CALIBRATION_FW)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_BG_SEND);
          break;
        }
      elanpress_send_cmd (ssm, dev, cmd_get_calib_mean);
      break;

    case CAPTURE_CALIB_MEAN_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 2,
                      ELANPRESS_CMD_TIMEOUT, elanpress_calib_mean_cb);
      break;

    case CAPTURE_BG_SEND:
      elanpress_send_cmd (ssm, dev, cmd_get_image);
      break;

    case CAPTURE_BG_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_BG_CHECK:
      self->bg_attempts++;
      if (!elanpress_background_ok (self))
        {
          if (self->bg_attempts < ELANPRESS_BG_ATTEMPTS)
            {
              fp_dbg ("finger still on the pad, waiting for a clean background");
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_CALIB_MEAN_SEND, 150);
              break;
            }
          fp_warn ("no clean background after %d attempts, using it anyway",
                   self->bg_attempts);
        }
      self->bg_attempts = 0;
      elanpress_dump16 (self, "bg", 0, self->background);
      fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_WAIT_ON_SEND:
      /* NEEDED once per touch; PRESENT is added by the first frame and
       * only removed when the touch ends, so clients don't see the flag
       * flicker at frame rate */
      if (self->num_frames == 0)
        fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NEEDED,
                                                 FP_FINGER_STATUS_PRESENT);
      elanpress_send_cmd (ssm, dev, cmd_pre_scan);
      break;

    case CAPTURE_WAIT_ON_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 1,
                      self->num_frames > 0 ? ELANPRESS_FINGER_GONE_TIMEOUT :
                      ELANPRESS_WAIT_FINGER_TIMEOUT,
                      elanpress_status_cb);
      break;

    case CAPTURE_FRAME_DECIDE:
      if (self->finger_byte != ELANPRESS_FINGER_PRESENT)
        {
          if (self->num_frames >= ELANPRESS_MIN_FRAMES)
            {
              /* finger lifted after enough frames: touch complete */
              fpi_ssm_mark_completed (ssm);
            }
          else if (self->num_frames > 0)
            {
              /* bounced touch, start over */
              fp_dbg ("finger lifted after only %d frames, retrying",
                      self->num_frames);
              elanpress_reset_capture (self);
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_WAIT_ON_SEND,
                                             ELANPRESS_POLL_INTERVAL_MS);
            }
          else if (self->wait_timed_out)
            {
              /* nobody touched the pad for a while: restart the cycle so
               * the pending pre-scan is cancelled and the background is
               * fresh */
              fpi_ssm_jump_to_state (ssm, CAPTURE_STOP);
            }
          else
            {
              /* 0x00 not ready / 0xaf busy: ask again shortly */
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_WAIT_ON_SEND,
                                             ELANPRESS_POLL_INTERVAL_MS);
            }
          break;
        }
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
      elanpress_send_cmd (ssm, dev, cmd_get_image);
      break;

    case CAPTURE_FRAME_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_FRAME_NEXT:
      if (self->num_frames >= ELANPRESS_MAX_FRAMES)
        fpi_ssm_mark_completed (ssm);
      else
        fpi_ssm_jump_to_state (ssm, CAPTURE_WAIT_ON_SEND);
      break;
    }
}

static void
elanpress_capture_touch (FpiDeviceElanPress *self, FpiSsmCompletedCallback done)
{
  FpiSsm *ssm;

  elanpress_reset_capture (self);
  self->bg_attempts = 0;
  ssm = fpi_ssm_new (FP_DEVICE (self), capture_run_state, CAPTURE_NUM_STATES);
  fpi_ssm_start (ssm, done);
}

/* === stop helper: turn the sensor off after an action === */

static void
elanpress_send_stop (FpDevice *dev)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (dev);
  g_autoptr(GError) error = NULL;

  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd_stop, ELANPRESS_CMD_LEN,
                                   NULL);
  if (!fpi_usb_transfer_submit_sync (transfer, ELANPRESS_CMD_TIMEOUT, &error))
    fp_warn ("failed to send stop command: %s", error->message);
}

/* === enroll === */

static void
elanpress_enroll_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpPrint *print = NULL;

  g_autoptr(GPtrArray) images = NULL;

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      elanpress_send_stop (dev);
      elanpress_reset_capture (self);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  images = elanpress_process_touch (self, ELANPRESS_MAX_IMAGES_PER_TOUCH);
  elanpress_reset_capture (self);

  if (images->len == 0)
    {
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  for (guint i = 0; i < images->len; i++)
    g_ptr_array_add (self->enroll_images, g_steal_pointer (&images->pdata[i]));
  g_ptr_array_set_free_func (images, NULL);

  self->enroll_stage++;
  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);

  if (self->enroll_stage < self->enroll_stages)
    {
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  elanpress_send_stop (dev);

  fp_dbg ("enrolment complete: %u images (%u bytes)", self->enroll_images->len,
          self->enroll_images->len * elanpress_frame_size (self));
  fpi_device_get_enroll_data (dev, &print);
  elanpress_print_set_data (self, print, self->enroll_images);
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);

  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
elanpress_enroll (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->enroll_stage = 0;
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  self->enroll_images = g_ptr_array_new_with_free_func (g_free);

  elanpress_capture_touch (self, elanpress_enroll_touch_done);
}

/* === verify and identify === */

static void
elanpress_match_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  g_autoptr(GPtrArray) probes = NULL;
  ElanpressMatchResult res = { 0 };

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
  elanpress_send_stop (dev);

  if (error)
    {
      elanpress_reset_capture (self);
      if (action == FPI_DEVICE_ACTION_IDENTIFY)
        fpi_device_identify_complete (dev, error);
      else
        fpi_device_verify_complete (dev, error);
      return;
    }

  probes = elanpress_process_touch (self, ELANPRESS_MAX_PROBES);
  elanpress_reset_capture (self);

  if (probes->len == 0)
    {
      GError *retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);

      if (action == FPI_DEVICE_ACTION_IDENTIFY)
        {
          fpi_device_identify_report (dev, NULL, NULL, retry);
          fpi_device_identify_complete (dev, NULL);
        }
      else
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, retry);
          fpi_device_verify_complete (dev, NULL);
        }
      return;
    }

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery = NULL;
      FpPrint *best_print = NULL;
      double best = -1.0;

      fpi_device_get_identify_data (dev, &gallery);
      for (guint i = 0; i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          ElanpressMatchResult r;
          double c = elanpress_match_print (self, probes, print, &r);

          if (c > best)
            {
              best = c;
              best_print = print;
              res = r;
            }
          if (best >= self->params.threshold)
            break;
        }

      fp_dbg ("identify: best %.3f (threshold %.2f) dx %d dy %d rot %.0f",
              best, self->params.threshold, res.dx, res.dy, res.rot);
      if (best >= self->params.threshold)
        fpi_device_identify_report (dev, best_print, NULL, NULL);
      else
        fpi_device_identify_report (dev, NULL, NULL, NULL);
      fpi_device_identify_complete (dev, NULL);
    }
  else
    {
      FpPrint *print = NULL;
      double c;

      fpi_device_get_verify_data (dev, &print);
      c = elanpress_match_print (self, probes, print, &res);
      fp_dbg ("verify: best %.3f (threshold %.2f) dx %d dy %d rot %.0f n %d",
              c, self->params.threshold, res.dx, res.dy, res.rot, res.overlap);

      if (c < 0)
        {
          fpi_device_verify_complete (dev,
                                      fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      fpi_device_verify_report (dev,
                                c >= self->params.threshold ?
                                FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
    }
}

static void
elanpress_identify_verify (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  elanpress_capture_touch (self, elanpress_match_touch_done);
}

/* === open and close === */

enum open_states {
  OPEN_FW_SEND,
  OPEN_FW_READ,
  OPEN_GET_DIM_SEND,
  OPEN_GET_DIM_READ,
  OPEN_NUM_STATES,
};

static void
elanpress_fw_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                 gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  self->fw_ver = (transfer->buffer[0] << 8) | transfer->buffer[1];
  fp_dbg ("FW ver 0x%04x", self->fw_ver);
  fpi_ssm_next_state (transfer->ssm);
}

static void
elanpress_dim_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* raw frames are column-major: byte 0 is the column height, byte 2 the
   * number of columns (see the elan driver) */
  self->frame_height = transfer->buffer[0];
  self->frame_width = transfer->buffer[2];

  /* work around sensors returning the sizes as zero-based index rather
   * than the number of pixels (same quirk as the elan driver) */
  if ((self->frame_width % 2 == 1) && (self->frame_height % 2 == 1))
    {
      self->frame_width++;
      self->frame_height++;
    }
  fp_dbg ("sensor dimensions, WxH: %dx%d", self->frame_width,
          self->frame_height);

  if (self->frame_width < 32 || self->frame_height < 32 ||
      self->frame_width > 255 || self->frame_height > 255)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected sensor dimensions %dx%d",
                                                     self->frame_width,
                                                     self->frame_height));
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_FW_SEND:
      elanpress_send_cmd (ssm, dev, cmd_get_fw_ver);
      break;

    case OPEN_FW_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 2,
                      ELANPRESS_CMD_TIMEOUT, elanpress_fw_cb);
      break;

    case OPEN_GET_DIM_SEND:
      elanpress_send_cmd (ssm, dev, cmd_get_sensor_dim);
      break;

    case OPEN_GET_DIM_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 4,
                      ELANPRESS_CMD_TIMEOUT, elanpress_dim_cb);
      break;
    }
}

static void
open_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_device_open_complete (dev, error);
}

static double
env_double_default (const char *name, double def)
{
  const char *v = g_getenv (name);
  char *end;
  double d;

  if (!v || !*v)
    return def;
  d = g_ascii_strtod (v, &end);
  return end == v ? def : d;
}

static void
elanpress_open (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;
  const char *dump;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  elanpress_match_params_from_env (&self->params);
  self->min_mean = env_double_default ("FP_ELANPRESS_MIN_MEAN", ELANPRESS_MIN_MEAN);
  self->min_contrast = env_double_default ("FP_ELANPRESS_MIN_CONTRAST", ELANPRESS_MIN_CONTRAST);
  self->min_coverage = env_double_default ("FP_ELANPRESS_MIN_COVERAGE", ELANPRESS_MIN_COVERAGE);
  self->enroll_stages = (int) env_double_default ("FP_ELANPRESS_ENROLL_STAGES", ELANPRESS_ENROLL_STAGES);
  self->enroll_stages = CLAMP (self->enroll_stages, 1, 40);
  fpi_device_set_nr_enroll_stages (dev, self->enroll_stages);

  dump = g_getenv ("FP_ELANPRESS_DUMP_DIR");
  if (dump && g_file_test (dump, G_FILE_TEST_IS_DIR))
    {
      g_autoptr(GDateTime) now = g_date_time_new_now_local ();

      self->dump_dir = g_strdup (dump);
      self->dump_session = g_date_time_format (now, "%H%M%S");
      fp_dbg ("dumping frames to %s (session %s)", self->dump_dir, self->dump_session);
    }

  fp_dbg ("threshold %.3f rot +/-%.0f step %.0f min_overlap %.2f norm_win %d, "
          "gates mean>%.0f contrast>%.0f coverage>%.2f, %d enroll stages",
          self->params.threshold, self->params.rot_range, self->params.rot_step,
          self->params.min_overlap, self->params.norm_win, self->min_mean,
          self->min_contrast, self->min_coverage, self->enroll_stages);

  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES),
                 open_complete);
}

static void
elanpress_close (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;

  elanpress_reset_capture (self);
  g_clear_pointer (&self->background, g_free);
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->dump_dir, g_free);
  g_clear_pointer (&self->dump_session, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
elanpress_cancel (FpDevice *dev)
{
  /* in-flight transfers are submitted with the action's cancellable, so
   * they fail on their own; the stop command is sent when the action's
   * state machine completes */
}

static void
fpi_device_elanpress_init (FpiDeviceElanPress *self)
{
}

static void
fpi_device_elanpress_class_init (FpiDeviceElanPressClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "elanpress";
  dev_class->full_name = "ElanTech press-type fingerprint sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = elanpress_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = ELANPRESS_ENROLL_STAGES;
  /* a tiny capacitive sensor designed to be always-on; the elan driver's
   * thermal model only ever tripped on its own retry loops */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = elanpress_open;
  dev_class->close = elanpress_close;
  dev_class->enroll = elanpress_enroll;
  dev_class->verify = elanpress_identify_verify;
  dev_class->identify = elanpress_identify_verify;
  dev_class->cancel = elanpress_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
