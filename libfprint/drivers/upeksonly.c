/*
 * UPEK TouchStrip Sensor-Only driver for libfprint
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 *
 * TCS4C (USB ID 147e:1000) support:
 * Copyright (C) 2010 Hugo Grostabussiat <dw23.devel@gmail.com>
 *
 * TCRD5B (USB ID 147e:1001) support:
 * Copyright (C) 2014 Vasily Khoruzhick <anarsoul@gmail.com>
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

#define FP_COMPONENT "upeksonly"

#include "drivers_api.h"
#include "upeksonly.h"

#define CTRL_TIMEOUT 1000
#define NUM_BULK_TRANSFERS 24
#define MAX_ROWS 2048
#define MAX_ROWS_1002 24000
#define MIN_ROWS 64
#define DETAIL_THRESHOLD_SQ_1002 800.0
#define MOTION_THRESHOLD_1002 18.0
#define DETAIL_TRIGGER_ROWS_1002 3
#define MIN_DETECTION_ROW_1002 1000
#define INITIAL_FINGER_IRQ_IGNORE_MS_1002 250
#define MAX_EARLY_FINGER_IRQS_1002 3
#define AWAIT_FINGER_SETTLE_DELAY_MS_1002 100

#define BLANK_THRESHOLD 250
#define FINGER_PRESENT_THRESHOLD 32
#define FINGER_REMOVED_THRESHOLD 100
#define DIFF_THRESHOLD 13

enum {
  UPEKSONLY_2016,
  UPEKSONLY_1000,
  UPEKSONLY_1002,
  UPEKSONLY_1001,
};

enum sonly_kill_transfers_action {
  NOT_KILLING = 0,

  /* report an image session error */
  IMG_SESSION_ERROR,

  /* iterate a SSM to the next state */
  ITERATE_SSM,
};

enum sonly_fs {
  AWAIT_FINGER,
  FINGER_DETECTED,
  FINGER_REMOVED,
};

enum loopsm_states {
  LOOPSM_RUN_AWFSM,
  LOOPSM_DELAY_AWAIT_FINGER,
  LOOPSM_STATUS_READ,
  LOOPSM_AWAIT_FINGER,
  LOOPSM_RUN_CAPSM,
  LOOPSM_CAPTURE,
  LOOPSM_RUN_DEINITSM,
  LOOPSM_FINAL,
  LOOPSM_NUM_STATES,
};

struct _FpiDeviceUpeksonly
{
  FpImageDevice parent;

  gboolean      capturing;
  gboolean      deactivating;
  guint8        read_reg_result;

  int           dev_model;
  int           img_width;

  FpiSsm       *loopsm;

  /* Do we really need multiple concurrent transfers? */
  GCancellable  *img_cancellable;
  GCancellable  *await_cancellable;
  GPtrArray     *img_transfers;
  int            num_flying;

  GSList        *rows;
  unsigned       num_rows;
  unsigned char *rowbuf;
  int            rowbuf_offset;

  int            wraparounds;
  int            num_blank;
  int            num_nonblank;
  enum sonly_fs finger_state;
  int            last_seqnum;
  guint32        marker_window;
  int            marker_skip;
  int            finger_start_row_1002;
  unsigned       detail_rows_1002;
  gboolean       have_avg_row_1002;
  guchar         avg_row_1002[IMG_CROP_WIDTH_1002];
  gint64         await_intr_start_us;
  unsigned       early_finger_irq_count_1002;

  enum sonly_kill_transfers_action killing_transfers;
  GError                   *kill_error;
  FpiSsm                   *kill_ssm;

  struct fpi_line_asmbl_ctx assembling_ctx;
};
G_DECLARE_FINAL_TYPE (FpiDeviceUpeksonly, fpi_device_upeksonly, FPI,
                      DEVICE_UPEKSONLY, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceUpeksonly, fpi_device_upeksonly, FP_TYPE_IMAGE_DEVICE);


/* Calculate squared standard deviation of sum of two lines */
static int
upeksonly_get_deviation2 (struct fpi_line_asmbl_ctx *ctx,
                          GSList *line1, GSList *line2)
{
  unsigned char *buf1 = line1->data, *buf2 = line2->data;
  int res = 0, mean = 0, i;

  g_assert (ctx->line_width > 0);

  for (i = 0; i < ctx->line_width; i += 2)
    mean += (int) buf1[i + 1] + (int) buf2[i];

  mean /= (ctx->line_width / 2);

  for (i = 0; i < ctx->line_width; i += 2)
    {
      int dev = (int) buf1[i + 1] + (int) buf2[i] - mean;
      res += dev * dev;
    }

  return res / (ctx->line_width / 2);
}


static unsigned char
upeksonly_get_pixel (struct fpi_line_asmbl_ctx *ctx,
                     GSList                    *row,
                     unsigned                   x)
{
  unsigned char *buf;
  unsigned offset;

  /* The scans from this device are rolled right by two columns */
  if (x < ctx->line_width - 2)
    offset = x + 2;
  else if ((x > ctx->line_width - 2) && (x < ctx->line_width))
    offset = x - (ctx->line_width - 2);
  else
    return 0;
  /* Each 2nd pixel is shifted 2 pixels down */
  if ((!(x & 1)) && g_slist_next (row) && g_slist_next (g_slist_next (row)))
    buf = g_slist_next (g_slist_next (row))->data;
  else
    buf = row->data;

  return buf[offset];
}

static int
upeksonly_get_deviation_simple (struct fpi_line_asmbl_ctx *ctx,
                                GSList                    *line1,
                                GSList                    *line2)
{
  unsigned char *buf1 = line1->data, *buf2 = line2->data;
  int res = 0, mean = 0;

  g_assert (ctx->line_width > 0);

  for (int i = 0; i < ctx->line_width; i++)
    mean += (int) buf1[i] + (int) buf2[i];

  mean /= ctx->line_width;

  for (int i = 0; i < ctx->line_width; i++)
    {
      int dev = (int) buf1[i] + (int) buf2[i] - mean;
      res += dev * dev;
    }

  return res / ctx->line_width;
}

static unsigned char
upeksonly_get_pixel_simple (struct fpi_line_asmbl_ctx *ctx,
                            GSList                    *row,
                            unsigned                   x)
{
  unsigned char *buf = row->data;

  if (x >= ctx->line_width)
    return 0;

  return buf[x];
}

/***** IMAGE PROCESSING *****/

static void
free_img_transfers (FpiDeviceUpeksonly *sdev)
{
  g_cancellable_cancel (sdev->img_cancellable);
  g_clear_object (&sdev->img_cancellable);
  g_clear_pointer (&sdev->img_transfers, g_ptr_array_unref);
}

static void
last_transfer_killed (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  switch (self->killing_transfers)
    {
    case ITERATE_SSM:
      fp_dbg ("iterate ssm");
      fpi_ssm_next_state (self->kill_ssm);
      return;

    case IMG_SESSION_ERROR:
      fp_dbg ("session error %s", self->kill_error->message);
      fpi_image_device_session_error (dev, g_steal_pointer (&self->kill_error));
      return;

    case NOT_KILLING:
    default:
      return;
    }
}

static void
cancel_img_transfers (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  g_cancellable_cancel (self->img_cancellable);

  if (self->num_flying == 0)
    last_transfer_killed (dev);
}

static gboolean
is_capturing (FpiDeviceUpeksonly *sdev)
{
  guint max_rows = sdev->dev_model == UPEKSONLY_1002 ? MAX_ROWS_1002 : MAX_ROWS;

  return sdev->num_rows < max_rows && (sdev->finger_state != FINGER_REMOVED);
}

static void
handoff_img (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  FpImage *img;

  GSList *elem = self->rows;

  if (!elem)
    {
      fp_err ("no rows?");
      return;
    }

  self->rows = g_slist_reverse (self->rows);

  fp_dbg ("%u rows", self->num_rows);

  if (self->dev_model == UPEKSONLY_1002)
    {
      guint crop_start = 0;
      guint cropped_rows;
      guint out_height;
      guint row_count = g_slist_length (self->rows);
      g_autofree guchar **rows = g_new (guchar *, row_count);
      guint row_idx = 0;

      if (row_count != self->num_rows)
        {
          fp_warn ("1002 row list length %u differs from row counter %u",
                   row_count,
                   self->num_rows);
          self->num_rows = row_count;
        }

      if (self->finger_start_row_1002 >= 0)
        crop_start = self->finger_start_row_1002 + IMG_CROP_OFFSET_1002;
      else if (row_count > IMG_CROP_ROWS_1002)
        crop_start = row_count - IMG_CROP_ROWS_1002;

      if (crop_start >= row_count)
        crop_start = 0;

      cropped_rows = MIN (IMG_CROP_ROWS_1002, row_count - crop_start);
      out_height = cropped_rows / IMG_HEIGHT_SCALE_1002;

      for (GSList *row = self->rows; row; row = g_slist_next (row))
        rows[row_idx++] = row->data;

      img = fp_image_new (IMG_CROP_WIDTH_1002, out_height);
      img->flags = FPI_IMAGE_COLORS_INVERTED;

      for (guint y = 0; y < out_height; y++)
        {
          for (guint x = 0; x < IMG_CROP_WIDTH_1002; x++)
            {
              guint sum = 0;

              for (guint i = 0; i < IMG_HEIGHT_SCALE_1002; i++)
                {
                  guint src_y = crop_start + y * IMG_HEIGHT_SCALE_1002 + i;
                  guint src_x = IMG_CROP_X_1002 + x;

                  sum += rows[src_y][src_x];
                }

              img->data[y * IMG_CROP_WIDTH_1002 + x] = sum / IMG_HEIGHT_SCALE_1002;
            }
        }

    }
  else
    {
      img = fpi_assemble_lines (&self->assembling_ctx, self->rows, self->num_rows);
    }

  g_slist_free_full (self->rows, g_free);
  self->rows = NULL;

  fpi_image_device_image_captured (dev, img);
  fpi_image_device_report_finger_status (dev, FALSE);

  self->killing_transfers = ITERATE_SSM;
  self->kill_ssm = self->loopsm;
  cancel_img_transfers (dev);
}

static gboolean
update_1002_finger_detection (FpiDeviceUpeksonly *self)
{
  GSList *row;
  double sum = 0.0;
  double sq_sum = 0.0;
  double motion = 0.0;
  guchar avg_row[IMG_CROP_WIDTH_1002];

  if (self->num_rows % IMG_HEIGHT_SCALE_1002 != 0)
    return FALSE;

  for (guint x = 0; x < IMG_CROP_WIDTH_1002; x++)
    {
      guint col_sum = 0;

      row = self->rows;
      for (guint i = 0; i < IMG_HEIGHT_SCALE_1002 && row; i++, row = g_slist_next (row))
        col_sum += ((guchar *) row->data)[IMG_CROP_X_1002 + x];

      avg_row[x] = col_sum / IMG_HEIGHT_SCALE_1002;
      sum += avg_row[x];
      sq_sum += avg_row[x] * avg_row[x];

      if (self->have_avg_row_1002)
        motion += ABS ((int) avg_row[x] - (int) self->avg_row_1002[x]);
    }

  if (self->have_avg_row_1002)
    motion /= IMG_CROP_WIDTH_1002;

  if (self->num_rows >= MIN_DETECTION_ROW_1002 &&
      (sq_sum / IMG_CROP_WIDTH_1002) -
      (sum / IMG_CROP_WIDTH_1002) * (sum / IMG_CROP_WIDTH_1002) > DETAIL_THRESHOLD_SQ_1002 &&
      self->have_avg_row_1002 &&
      motion > MOTION_THRESHOLD_1002)
    {
      self->detail_rows_1002++;

      if (self->finger_start_row_1002 < 0 &&
          self->detail_rows_1002 >= DETAIL_TRIGGER_ROWS_1002)
        {
          self->finger_start_row_1002 =
            MAX (0, (int) self->num_rows -
                    (int) self->detail_rows_1002 * IMG_HEIGHT_SCALE_1002);
          fp_dbg ("detected 1002 finger data at row %d", self->finger_start_row_1002);
        }
    }
  else if (self->finger_start_row_1002 < 0)
    {
      self->detail_rows_1002 = 0;
    }

  memcpy (self->avg_row_1002, avg_row, sizeof (avg_row));
  self->have_avg_row_1002 = TRUE;

  return self->finger_start_row_1002 >= 0 &&
         self->num_rows >= (guint) self->finger_start_row_1002 +
                           IMG_CROP_OFFSET_1002 +
                           IMG_CROP_ROWS_1002;
}

static void
row_complete (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  int line_width = self->assembling_ctx.line_width;

  self->rowbuf_offset = -1;

  if (self->dev_model == UPEKSONLY_1002)
    {
      if (!is_capturing (self) || self->killing_transfers)
        {
          g_clear_pointer (&self->rowbuf, g_free);
          return;
        }

      self->rows = g_slist_prepend (self->rows, self->rowbuf);
      self->num_rows++;
      self->rowbuf = NULL;

      if (update_1002_finger_detection (self))
        {
          fp_dbg ("row limit met");
          self->finger_state = FINGER_REMOVED;
          handoff_img (dev);
        }
      else if (self->num_rows >= MAX_ROWS_1002)
        {
          fp_dbg ("1002 capture timed out without finger data");
          self->finger_state = FINGER_REMOVED;
          fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_TOO_SHORT);
          fpi_image_device_report_finger_status (dev, FALSE);
          self->killing_transfers = ITERATE_SSM;
          self->kill_ssm = self->loopsm;
          cancel_img_transfers (dev);
        }

      return;
    }

  if (self->num_rows > 0)
    {
      unsigned char *lastrow = self->rows->data;
      int std_sq_dev, mean_sq_diff;

      std_sq_dev = fpi_std_sq_dev (self->rowbuf, line_width);
      mean_sq_diff = fpi_mean_sq_diff_norm (lastrow, self->rowbuf,
                                            line_width);

      switch (self->finger_state)
        {
        case AWAIT_FINGER:
          if (self->deactivating)
            {
              self->killing_transfers = ITERATE_SSM;
              self->kill_ssm = self->loopsm;
              cancel_img_transfers (dev);
            }
          fp_dbg ("std_sq_dev: %d", std_sq_dev);
          if (std_sq_dev > BLANK_THRESHOLD)
            self->num_nonblank++;
          else
            self->num_nonblank = 0;

          if (self->num_nonblank > FINGER_PRESENT_THRESHOLD)
            {
              self->finger_state = FINGER_DETECTED;
              fpi_image_device_report_finger_status (dev,
                                                     TRUE);
            }
          else
            {
              return;
            }
          break;

        case FINGER_DETECTED:
        case FINGER_REMOVED:
        default:
          break;
        }

      if (std_sq_dev > BLANK_THRESHOLD)
        {
          self->num_blank = 0;
        }
      else
        {
          self->num_blank++;
          /* Don't consider the scan complete unless there's at least
           * MIN_ROWS recorded or very long blank read occurred.
           *
           * Typical problem spot: one brief touch before starting the
           * actual scan. Happens most commonly if scan is started
           * from before the first joint resulting in a gap after the initial touch.
           */
          if (self->num_blank > FINGER_REMOVED_THRESHOLD)
            {
              self->finger_state = FINGER_REMOVED;
              fp_dbg ("detected finger removal. Blank rows: %d, Full rows: %u",
                      self->num_blank, self->num_rows);
              handoff_img (dev);
              return;
            }
        }
      fp_dbg ("mean_sq_diff: %d, std_sq_dev: %d", mean_sq_diff, std_sq_dev);
      fp_dbg ("num_blank: %d", self->num_blank);
      if (mean_sq_diff < DIFF_THRESHOLD)
        return;
    }

  switch (self->finger_state)
    {
    case AWAIT_FINGER:
      if (!self->num_rows)
        {
          self->rows = g_slist_prepend (self->rows, self->rowbuf);
          self->num_rows++;
        }
      else
        {
          return;
        }
      break;

    case FINGER_DETECTED:
    case FINGER_REMOVED:
      self->rows = g_slist_prepend (self->rows, self->rowbuf);
      self->num_rows++;
      break;
    }
  self->rowbuf = NULL;

  if (self->num_rows >= MAX_ROWS)
    {
      fp_dbg ("row limit met");
      handoff_img (dev);
    }
}

/* add data to row buffer */
static void
add_to_rowbuf (FpImageDevice *dev, unsigned char *data, int size)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  memcpy (self->rowbuf + self->rowbuf_offset, data, size);
  self->rowbuf_offset += size;
  if (self->rowbuf_offset >= self->img_width)
    row_complete (dev);

}

static void
start_new_row (FpiDeviceUpeksonly *self, unsigned char *data,
               int size)
{
  if (!self->rowbuf)
    self->rowbuf = g_malloc (self->img_width);
  memcpy (self->rowbuf, data, size);
  self->rowbuf_offset = size;
}

/* returns number of bytes left to be copied into rowbuf (capped to 62)
 * or -1 if we aren't capturing anything */
static int
rowbuf_remaining (FpiDeviceUpeksonly *sdev)
{
  int r;

  if (sdev->rowbuf_offset == -1)
    return -1;

  r = sdev->img_width - sdev->rowbuf_offset;
  if (r > 62)
    r = 62;
  return r;
}

static void
handle_packet (FpImageDevice *dev, unsigned char *data)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  guint16 seqnum = data[0] << 8 | data[1];
  int abs_base_addr;
  int for_rowbuf;
  int next_row_addr;
  int diff;
  unsigned char dummy_data[62];

  /* Init dummy data to something neutral */
  memset (dummy_data, 204, 62);

  data += 2;       /* skip sequence number */
  if (seqnum != self->last_seqnum + 1)
    {
      if (seqnum != 0 && self->last_seqnum != 16383)
        {
          int missing_data = seqnum - self->last_seqnum;
          int i;
          fp_warn ("lost %d packets of data between %d and %d", missing_data,
                   self->last_seqnum, seqnum );

          /* Minimize distortions for readers that lose a lot of packets */
          for (i = 1; i < missing_data; i++)
            {
              abs_base_addr = (self->last_seqnum + 1) * 62;

              /* If possible take the replacement data from last row */
              if (self->num_rows > 1)
                {
                  int row_left = self->img_width - self->rowbuf_offset;
                  unsigned char *last_row = g_slist_nth_data (self->rows,
                                                              0);

                  if (row_left >= 62)
                    {
                      memcpy (dummy_data,
                              last_row + self->rowbuf_offset,
                              62);
                    }
                  else
                    {
                      memcpy (dummy_data,
                              last_row + self->rowbuf_offset,
                              row_left);
                      memcpy (dummy_data + row_left, last_row, 62 - row_left);
                    }
                }

              fp_warn ("adding dummy input for %d, i=%d",
                       self->last_seqnum + i, i);
              for_rowbuf = rowbuf_remaining (self);
              if (for_rowbuf != -1)
                {
                  add_to_rowbuf (dev, dummy_data, for_rowbuf);
                  /* row boundary */
                  if (for_rowbuf < 62)
                    {
                      start_new_row (self,
                                     dummy_data + for_rowbuf,
                                     62 - for_rowbuf);
                    }
                }
              else if (abs_base_addr % self->img_width == 0)
                {
                  start_new_row (self, dummy_data, 62);
                }
              else
                {
                  /* does the data in the packet reside on a row boundary?
                   * if so capture it */
                  next_row_addr = ((abs_base_addr / self->img_width) + 1) * self->img_width;
                  diff = next_row_addr - abs_base_addr;
                  if (diff < 62)
                    start_new_row (self,
                                   dummy_data + diff,
                                   62 - diff);
                }
              self->last_seqnum = self->last_seqnum + 1;
            }
        }
    }
  if (seqnum <= self->last_seqnum)
    {
      fp_dbg ("detected wraparound");
      self->wraparounds++;
    }

  self->last_seqnum = seqnum;
  seqnum += self->wraparounds * 16384;
  abs_base_addr = seqnum * 62;

  /* are we already capturing a row? if so append the data to the
   * row buffer */
  for_rowbuf = rowbuf_remaining (self);
  if (for_rowbuf != -1)
    {
      add_to_rowbuf (dev, data, for_rowbuf);
      /*row boundary*/
      if (for_rowbuf < 62)
        start_new_row (self, data + for_rowbuf,
                       62 - for_rowbuf);
      return;
    }

  /* does the packet START on a boundary? if so we want it in full */
  if (abs_base_addr % self->img_width == 0)
    {
      start_new_row (self, data, 62);
      return;
    }

  /* does the data in the packet reside on a row boundary?
   * if so capture it */
  next_row_addr = ((abs_base_addr / self->img_width) + 1) * self->img_width;
  diff = next_row_addr - abs_base_addr;
  if (diff < 62)
    start_new_row (self, data + diff, 62 - diff);
}

static void
handle_packet_1002 (FpImageDevice *dev, unsigned char *data)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  data += 2;       /* skip sequence number */

  for (int i = 0; i < 62; i++)
    {
      guint8 byte = data[i];

      if (self->marker_skip > 0)
        {
          self->marker_skip--;
          continue;
        }

      if (self->rowbuf)
        {
          if (self->rowbuf_offset < self->img_width)
            self->rowbuf[self->rowbuf_offset++] = byte;
        }

      self->marker_window = ((self->marker_window << 8) | byte) & 0xffffff;

      if (self->marker_window == 0xffa5a5)
        {
          if (self->rowbuf)
            {
              if (self->rowbuf_offset >= 3)
                self->rowbuf_offset -= 3;

              if (self->rowbuf_offset > IMG_WIDTH_1002 / 2)
                {
                  row_complete (dev);
                  if (!is_capturing (self))
                    return;
                }
              else
                g_clear_pointer (&self->rowbuf, g_free);
            }

          self->rowbuf = g_malloc0 (self->img_width);
          self->rowbuf_offset = 0;
          self->marker_skip = 5;
          self->marker_window = 0;
        }
    }
}

static void
img_data_cb (FpiUsbTransfer *transfer, FpDevice *device,
             gpointer user_data, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  int i;

  self->num_flying--;

  if (self->dev_model != UPEKSONLY_1002)
    fp_dbg ("bulk cb: error=%s actual_length=%zu num_flying=%d capturing=%d",
            error ? error->message : "none",
            transfer->actual_length,
            self->num_flying,
            self->capturing);

  if (self->killing_transfers)
    {
      if (self->num_flying == 0)
        last_transfer_killed (dev);

      /* don't care about error or success if we're terminating */
      g_clear_error (&error);
      return;
    }

  /* NOTE: The old code assume 4096 bytes are received each time
   * but there is no reason we need to enforce that. However, we
   * always need full lines. */
  if (transfer->actual_length % 64 != 0)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                      "Data packets need to be multiple of 64 bytes, got %zi bytes",
                                      transfer->actual_length);

  if (error)
    {
      fp_warn ("bad status %s, terminating session", error->message);
      self->killing_transfers = IMG_SESSION_ERROR;

      /* This cannot really happen, but just in case. */
      if (!self->kill_error)
        self->kill_error = error;
      else
        g_error_free (error);

      cancel_img_transfers (dev);
      return;
    }

  /* there are 64 packets in the transfer buffer
   * each packet is 64 bytes in length
   * the first 2 bytes are a sequence number
   * then there are 62 bytes for image data
   */
  for (i = 0; i + 64 <= transfer->actual_length; i += 64)
    {
      if (!is_capturing (self))
        return;

      if (self->dev_model == UPEKSONLY_1002)
        handle_packet_1002 (dev, transfer->buffer + i);
      else
        handle_packet (dev, transfer->buffer + i);
    }

  if (is_capturing (self))
    {
      if (self->dev_model != UPEKSONLY_1002)
        fp_dbg ("re-submitting bulk transfer actual_length=%zu rows=%u finger_state=%d",
                transfer->actual_length,
                self->num_rows,
                self->finger_state);
      fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer),
                               0,
                               self->img_cancellable,
                               img_data_cb,
                               user_data);
      self->num_flying++;
    }
}

/***** STATE MACHINE HELPERS *****/

struct write_regs_data
{
  FpDevice                    *dev;
  FpiSsm                      *ssm;
  FpiUsbTransfer              *transfer;
  const struct sonly_regwrite *regs;
  size_t                       num_regs;
  size_t                       regs_written;
};

struct write_regs_data_data
{
  FpDevice                         *dev;
  FpiSsm                           *ssm;
  const struct sonly_regwrite_data *regs;
  size_t                            num_regs;
  size_t                            regs_written;
};

static void sm_await_intr (FpiSsm *ssm, FpImageDevice *dev);

static void
write_regs_finished (struct write_regs_data *wrdata, GError *error)
{
  if (!error)
    fpi_ssm_next_state (wrdata->ssm);
  else
    fpi_ssm_mark_failed (wrdata->ssm, error);

  g_free (wrdata);
}

static void write_regs_iterate (struct write_regs_data *wrdata);

static void
write_regs_cb (FpiUsbTransfer *transfer, FpDevice *device,
               gpointer user_data, GError *error)
{
  struct write_regs_data *wrdata = user_data;

  if (error)
    {
      write_regs_finished (wrdata, error);
      return;
    }

  wrdata->regs_written++;
  write_regs_iterate (wrdata);
}

static void
write_regs_iterate (struct write_regs_data *wrdata)
{
  FpiUsbTransfer *transfer;
  const struct sonly_regwrite *regwrite;

  if (wrdata->regs_written >= wrdata->num_regs)
    {
      write_regs_finished (wrdata, NULL);
      return;
    }

  regwrite = &wrdata->regs[wrdata->regs_written];
  fp_dbg ("set %02x=%02x", regwrite->reg, regwrite->value);

  transfer = fpi_usb_transfer_new (wrdata->dev);
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x0c,
                                 0,
                                 regwrite->reg,
                                 1);
  transfer->short_is_error = TRUE;
  transfer->ssm = wrdata->ssm;
  transfer->buffer[0] = regwrite->value;

  fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL, write_regs_cb, wrdata);
}

static void
sm_write_regs (FpiSsm                      *ssm,
               FpDevice                    *dev,
               const struct sonly_regwrite *regs,
               size_t                       num_regs)
{
  struct write_regs_data *wrdata = g_malloc (sizeof (*wrdata));

  wrdata->ssm = ssm;
  wrdata->regs = regs;
  wrdata->num_regs = num_regs;
  wrdata->regs_written = 0;
  wrdata->dev = dev;

  write_regs_iterate (wrdata);
}

static void write_regs_data_iterate (struct write_regs_data_data *wrdata);
static void write_regs_data_finished (struct write_regs_data_data *wrdata, GError *error);

static void
write_regs_data_readback_cb (FpiUsbTransfer *transfer, FpDevice *device,
                             gpointer user_data, GError *error)
{
  struct write_regs_data_data *wrdata = user_data;

  if (error)
    {
      write_regs_data_finished (wrdata, error);
      return;
    }

  fp_dbg ("read %02x result = %02x %02x %02x %02x %02x %02x %02x %02x",
          transfer->endpoint & 0xff,
          transfer->buffer[0], transfer->buffer[1], transfer->buffer[2], transfer->buffer[3],
          transfer->buffer[4], transfer->buffer[5], transfer->buffer[6], transfer->buffer[7]);

  wrdata->regs_written++;
  write_regs_data_iterate (wrdata);
}

static void
write_regs_data_finished (struct write_regs_data_data *wrdata, GError *error)
{
  if (!error)
    fpi_ssm_next_state (wrdata->ssm);
  else
    fpi_ssm_mark_failed (wrdata->ssm, error);

  g_free (wrdata);
}

static void
write_regs_data_cb (FpiUsbTransfer *transfer, FpDevice *device,
                    gpointer user_data, GError *error)
{
  struct write_regs_data_data *wrdata = user_data;
  const struct sonly_regwrite_data *regwrite;

  if (error)
    {
      write_regs_data_finished (wrdata, error);
      return;
    }

  regwrite = &wrdata->regs[wrdata->regs_written];

  if (regwrite->reg == 0x47 &&
      regwrite->len == 1 &&
      (regwrite->value[0] == 0x00 || regwrite->value[0] == 0x04))
    {
      FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (wrdata->dev);

      fp_dbg ("read 58 len=8");
      fpi_usb_transfer_fill_control (read_transfer,
                                     G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     0x0c,
                                     0,
                                     0x58,
                                     8);
      read_transfer->short_is_error = TRUE;
      read_transfer->ssm = wrdata->ssm;
      read_transfer->endpoint = 0x58;

      fpi_usb_transfer_submit (read_transfer,
                               CTRL_TIMEOUT,
                               NULL,
                               write_regs_data_readback_cb,
                               wrdata);
      return;
    }

  wrdata->regs_written++;
  write_regs_data_iterate (wrdata);
}

static void
write_regs_data_iterate (struct write_regs_data_data *wrdata)
{
  FpiUsbTransfer *transfer;
  const struct sonly_regwrite_data *regwrite;

  if (wrdata->regs_written >= wrdata->num_regs)
    {
      write_regs_data_finished (wrdata, NULL);
      return;
    }

  regwrite = &wrdata->regs[wrdata->regs_written];
  fp_dbg ("set %02x len=%u", regwrite->reg, regwrite->len);

  if (regwrite->len == 0)
    {
      guint8 read_len = regwrite->value[0] ? regwrite->value[0] : 8;

      fp_dbg ("read %02x len=%u", regwrite->reg, read_len);
      transfer = fpi_usb_transfer_new (wrdata->dev);
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     0x0c,
                                     0,
                                     regwrite->reg,
                                     read_len);
      transfer->short_is_error = TRUE;
      transfer->ssm = wrdata->ssm;
      transfer->endpoint = regwrite->reg;

      fpi_usb_transfer_submit (transfer,
                               CTRL_TIMEOUT,
                               NULL,
                               write_regs_data_readback_cb,
                               wrdata);
      return;
    }

  transfer = fpi_usb_transfer_new (wrdata->dev);
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x0c,
                                 0,
                                 regwrite->reg,
                                 regwrite->len);
  transfer->short_is_error = TRUE;
  transfer->ssm = wrdata->ssm;
  memcpy (transfer->buffer, regwrite->value, regwrite->len);

  fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL, write_regs_data_cb, wrdata);
}

static void
sm_write_regs_data (FpiSsm                           *ssm,
                    FpDevice                         *dev,
                    const struct sonly_regwrite_data *regs,
                    size_t                            num_regs)
{
  struct write_regs_data_data *wrdata = g_malloc (sizeof (*wrdata));

  wrdata->ssm = ssm;
  wrdata->regs = regs;
  wrdata->num_regs = num_regs;
  wrdata->regs_written = 0;
  wrdata->dev = dev;

  write_regs_data_iterate (wrdata);
}

static void
sm_write_reg (FpiSsm        *ssm,
              FpImageDevice *dev,
              guint8         reg,
              guint8         value)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fp_dbg ("set %02x=%02x", reg, value);
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x0c,
                                 0,
                                 reg,
                                 1);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;
  transfer->buffer[0] = value;

  fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
sm_read_reg_cb (FpiUsbTransfer *transfer, FpDevice *device,
                gpointer user_data, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
    }
  else
    {
      self->read_reg_result = transfer->buffer[0];
      fp_dbg ("read reg result = %02x", self->read_reg_result);
      fpi_ssm_next_state (transfer->ssm);
    }
}

static void
sm_read_reg (FpiSsm        *ssm,
             FpImageDevice *dev,
             guint8         reg)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fp_dbg ("read reg %02x", reg);
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x0c,
                                 0,
                                 reg,
                                 8);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           CTRL_TIMEOUT,
                           NULL,
                           sm_read_reg_cb,
                           NULL);
}

static void
sm_read_status_cb (FpiUsbTransfer *transfer, FpDevice *device,
                   gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("read status result = %02x %02x %02x %02x %02x %02x %02x %02x",
          transfer->buffer[0], transfer->buffer[1], transfer->buffer[2], transfer->buffer[3],
          transfer->buffer[4], transfer->buffer[5], transfer->buffer[6], transfer->buffer[7]);
  fpi_ssm_next_state (transfer->ssm);
}

static void
sm_read_status (FpiSsm        *ssm,
                FpImageDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (dev));

  fp_dbg ("read status");
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x04,
                                 0,
                                 0,
                                 8);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer,
                           CTRL_TIMEOUT,
                           NULL,
                           sm_read_status_cb,
                           NULL);
}

static void
sm_await_intr_cb (FpiUsbTransfer *transfer, FpDevice *device,
                  gpointer user_data, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (device);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  gboolean finger_detected = TRUE;

  g_clear_object (&self->await_cancellable);

  if (error)
    {
      if (self->deactivating &&
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }

      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("interrupt received: %02x %02x %02x %02x",
          transfer->buffer[0], transfer->buffer[1],
          transfer->buffer[2], transfer->buffer[3]);

  if (self->dev_model == UPEKSONLY_1002)
    {
      gint64 elapsed_ms = (g_get_monotonic_time () - self->await_intr_start_us) / 1000;

      /* Windows keeps polling 0x83 with 12 12 12 12 while idle and only
       * switches to 01 00 00 00 when a finger is actually detected. */
      finger_detected = transfer->buffer[0] == 0x01 &&
                        transfer->buffer[1] == 0x00 &&
                        transfer->buffer[2] == 0x00 &&
                        transfer->buffer[3] == 0x00;

      if (finger_detected && elapsed_ms < INITIAL_FINGER_IRQ_IGNORE_MS_1002)
        {
          self->early_finger_irq_count_1002++;

          if (self->early_finger_irq_count_1002 < MAX_EARLY_FINGER_IRQS_1002)
            {
              fp_dbg ("1002 early finger interrupt after %" G_GINT64_FORMAT " ms, restart wait setup",
                      elapsed_ms);
              fpi_ssm_jump_to_state_delayed (transfer->ssm, LOOPSM_RUN_AWFSM, 200);
              return;
            }

          fp_dbg ("1002 accepting repeated early finger interrupt after %u attempts",
                  self->early_finger_irq_count_1002);
        }

      if (!finger_detected)
        {
          fp_dbg ("1002 idle interrupt, continue waiting");
          sm_await_intr (transfer->ssm, dev);
          return;
        }
    }

  self->early_finger_irq_count_1002 = 0;
  self->finger_state = FINGER_DETECTED;
  fpi_image_device_report_finger_status (dev, TRUE);
  fpi_ssm_next_state (transfer->ssm);
}

static void
sm_await_intr (FpiSsm        *ssm,
               FpImageDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  G_DEBUG_HERE ();

  self->await_intr_start_us = g_get_monotonic_time ();
  g_clear_object (&self->await_cancellable);
  self->await_cancellable = g_cancellable_new ();

  fpi_usb_transfer_fill_interrupt (transfer, 0x83, 4);
  transfer->short_is_error = TRUE;
  transfer->ssm = ssm;

  /* NOTE: This was changed to be cancellable with the version 2 port! */
  fpi_usb_transfer_submit (transfer,
                           0,
                           self->await_cancellable,
                           sm_await_intr_cb,
                           NULL);
}

/***** AWAIT FINGER *****/

enum awfsm_2016_states {
  AWFSM_2016_WRITEV_1,
  AWFSM_2016_READ_01,
  AWFSM_2016_WRITE_01,
  AWFSM_2016_WRITEV_2,
  AWFSM_2016_READ_13,
  AWFSM_2016_WRITE_13,
  AWFSM_2016_WRITEV_3,
  AWFSM_2016_READ_07,
  AWFSM_2016_WRITE_07,
  AWFSM_2016_WRITEV_4,
  AWFSM_2016_NUM_STATES,
};

enum awfsm_1000_states {
  AWFSM_1000_WRITEV_1,
  AWFSM_1000_WRITEV_2,
  AWFSM_1000_NUM_STATES,
};

static void
awfsm_2016_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case AWFSM_2016_WRITEV_1:
      sm_write_regs (ssm, _dev, awfsm_2016_writev_1, G_N_ELEMENTS (awfsm_2016_writev_1));
      break;

    case AWFSM_2016_READ_01:
      sm_read_reg (ssm, dev, 0x01);
      break;

    case AWFSM_2016_WRITE_01:
      if (self->read_reg_result != 0xc6)
        sm_write_reg (ssm, dev, 0x01, 0x46);
      else
        sm_write_reg (ssm, dev, 0x01, 0xc6);
      break;

    case AWFSM_2016_WRITEV_2:
      sm_write_regs (ssm, _dev, awfsm_2016_writev_2, G_N_ELEMENTS (awfsm_2016_writev_2));
      break;

    case AWFSM_2016_READ_13:
      sm_read_reg (ssm, dev, 0x13);
      break;

    case AWFSM_2016_WRITE_13:
      if (self->read_reg_result != 0x45)
        sm_write_reg (ssm, dev, 0x13, 0x05);
      else
        sm_write_reg (ssm, dev, 0x13, 0x45);
      break;

    case AWFSM_2016_WRITEV_3:
      sm_write_regs (ssm, _dev, awfsm_2016_writev_3, G_N_ELEMENTS (awfsm_2016_writev_3));
      break;

    case AWFSM_2016_READ_07:
      sm_read_reg (ssm, dev, 0x07);
      break;

    case AWFSM_2016_WRITE_07:
      if (self->read_reg_result != 0x10 && self->read_reg_result != 0x90)
        fp_warn ("odd reg7 value %x", self->read_reg_result);
      sm_write_reg (ssm, dev, 0x07, self->read_reg_result);
      break;

    case AWFSM_2016_WRITEV_4:
      sm_write_regs (ssm, _dev, awfsm_2016_writev_4, G_N_ELEMENTS (awfsm_2016_writev_4));
      break;
    }
}

static void
awfsm_1000_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case AWFSM_1000_WRITEV_1:
      if (FPI_DEVICE_UPEKSONLY (_dev)->dev_model == UPEKSONLY_1002)
        fpi_ssm_next_state (ssm);
      else
        sm_write_regs (ssm, _dev, awfsm_1000_writev_1, G_N_ELEMENTS (awfsm_1000_writev_1));
      break;

    case AWFSM_1000_WRITEV_2:
      if (FPI_DEVICE_UPEKSONLY (_dev)->dev_model == UPEKSONLY_1002)
        sm_write_regs (ssm, _dev, awfsm_1002_writev_2, G_N_ELEMENTS (awfsm_1002_writev_2));
      else
        sm_write_regs (ssm, _dev, awfsm_1000_writev_2, G_N_ELEMENTS (awfsm_1000_writev_2));
      break;
    }
}

/***** CAPTURE MODE *****/

enum capsm_2016_states {
  CAPSM_2016_INIT,
  CAPSM_2016_WRITE_15,
  CAPSM_2016_WRITE_30,
  CAPSM_2016_FIRE_BULK,
  CAPSM_2016_WRITEV,
  CAPSM_2016_NUM_STATES,
};

enum capsm_1000_states {
  CAPSM_1000_INIT,
  CAPSM_1000_FIRE_BULK,
  CAPSM_1000_WRITEV,
  CAPSM_1000_NUM_STATES,
};

enum capsm_1002_states {
  CAPSM_1002_INIT,
  CAPSM_1002_STREAM_SETUP,
  CAPSM_1002_FIRE_BULK,
  CAPSM_1002_WRITEV,
  CAPSM_1002_NUM_STATES,
};

enum capsm_1001_states {
  CAPSM_1001_INIT,
  CAPSM_1001_FIRE_BULK,
  CAPSM_1001_WRITEV_1,
  CAPSM_1001_WRITEV_2,
  CAPSM_1001_WRITEV_3,
  CAPSM_1001_WRITEV_4,
  CAPSM_1001_WRITEV_5,
  CAPSM_1001_NUM_STATES,
};

static void
capsm_fire_bulk (FpiSsm   *ssm,
                 FpDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  int i;

  g_assert (self->capturing == FALSE);

  g_clear_object (&self->img_cancellable);
  self->img_cancellable = g_cancellable_new ();
  fp_dbg ("submitting %u bulk transfers on ep 0x81", self->img_transfers->len);
  for (i = 0; i < self->img_transfers->len; i++)
    {
      fpi_usb_transfer_submit (fpi_usb_transfer_ref (g_ptr_array_index (self->img_transfers, i)),
                               0,
                               self->img_cancellable,
                               img_data_cb,
                               NULL);
      self->num_flying++;
    }
  self->capturing = TRUE;
  fpi_ssm_next_state (ssm);
}

static void
clear_capture_rows (FpiDeviceUpeksonly *self)
{
  g_clear_pointer (&self->rowbuf, g_free);
  g_slist_free_full (self->rows, g_free);
  self->rows = NULL;
}

static void
capsm_2016_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPSM_2016_INIT:
      clear_capture_rows (self);
      self->rowbuf_offset = -1;
      self->num_rows = 0;
      self->wraparounds = -1;
      self->num_blank = 0;
      self->num_nonblank = 0;
      self->finger_state = FINGER_DETECTED;
      self->last_seqnum = 16383;
      self->marker_window = 0;
      self->marker_skip = 0;
      self->killing_transfers = 0;
      fpi_ssm_next_state (ssm);
      break;

    case CAPSM_2016_WRITE_15:
      sm_write_reg (ssm, dev, 0x15, 0x20);
      break;

    case CAPSM_2016_WRITE_30:
      sm_write_reg (ssm, dev, 0x30, 0xe0);
      break;

    case CAPSM_2016_FIRE_BULK:;
      capsm_fire_bulk (ssm, _dev);
      break;

    case CAPSM_2016_WRITEV:
      sm_write_regs (ssm, _dev, capsm_2016_writev, G_N_ELEMENTS (capsm_2016_writev));
      break;
    }
}

static void
capsm_1000_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPSM_1000_INIT:
      clear_capture_rows (self);
      self->rowbuf_offset = -1;
      self->num_rows = 0;
      self->wraparounds = -1;
      self->num_blank = 0;
      self->num_nonblank = 0;
      self->finger_state = FINGER_DETECTED;
      self->last_seqnum = 16383;
      self->marker_window = 0;
      self->marker_skip = 0;
      self->killing_transfers = 0;
      fpi_ssm_next_state (ssm);
      break;

    case CAPSM_1000_FIRE_BULK:;
      capsm_fire_bulk (ssm, _dev);
      break;

    case CAPSM_1000_WRITEV:
      sm_write_regs (ssm, _dev, capsm_1000_writev, G_N_ELEMENTS (capsm_1000_writev));
      break;
    }
}

static void
capsm_1001_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPSM_1001_INIT:
      clear_capture_rows (self);
      self->rowbuf_offset = -1;
      self->num_rows = 0;
      self->wraparounds = -1;
      self->num_blank = 0;
      self->num_nonblank = 0;
      self->finger_state = AWAIT_FINGER;
      self->last_seqnum = 16383;
      self->marker_window = 0;
      self->marker_skip = 0;
      self->killing_transfers = 0;
      fpi_ssm_next_state (ssm);
      break;

    case CAPSM_1001_FIRE_BULK:;
      capsm_fire_bulk (ssm, _dev);
      break;

    case CAPSM_1001_WRITEV_1:
      sm_write_regs (ssm, _dev, capsm_1001_writev_1, G_N_ELEMENTS (capsm_1001_writev_1));
      break;

    case CAPSM_1001_WRITEV_2:
      sm_write_regs (ssm, _dev, capsm_1001_writev_2, G_N_ELEMENTS (capsm_1001_writev_2));
      break;

    case CAPSM_1001_WRITEV_3:
      sm_write_regs (ssm, _dev, capsm_1001_writev_3, G_N_ELEMENTS (capsm_1001_writev_3));
      break;

    case CAPSM_1001_WRITEV_4:
      sm_write_regs (ssm, _dev, capsm_1001_writev_4, G_N_ELEMENTS (capsm_1001_writev_4));
      break;

    case CAPSM_1001_WRITEV_5:
      sm_write_regs (ssm, _dev, capsm_1001_writev_5, G_N_ELEMENTS (capsm_1001_writev_5));
      break;
    }
}

static void
capsm_1002_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPSM_1002_INIT:
      clear_capture_rows (self);
      self->rowbuf_offset = -1;
      self->num_rows = 0;
      self->wraparounds = -1;
      self->num_blank = 0;
      self->num_nonblank = 0;
      self->finger_state = FINGER_DETECTED;
      self->last_seqnum = 16383;
      self->marker_window = 0;
      self->marker_skip = 0;
      self->finger_start_row_1002 = -1;
      self->detail_rows_1002 = 0;
      self->have_avg_row_1002 = FALSE;
      self->early_finger_irq_count_1002 = 0;
      self->killing_transfers = 0;
      fpi_ssm_next_state (ssm);
      break;

    case CAPSM_1002_STREAM_SETUP:
      sm_write_regs_data (ssm, _dev, capsm_1002_writev_data, G_N_ELEMENTS (capsm_1002_writev_data));
      break;

    case CAPSM_1002_WRITEV:
      sm_write_regs (ssm, _dev, capsm_1002_writev, G_N_ELEMENTS (capsm_1002_writev));
      break;

    case CAPSM_1002_FIRE_BULK:
      capsm_fire_bulk (ssm, _dev);
      break;
    }
}

/***** DEINITIALIZATION *****/

enum deinitsm_2016_states {
  DEINITSM_2016_WRITEV,
  DEINITSM_2016_NUM_STATES,
};

enum deinitsm_1000_states {
  DEINITSM_1000_WRITEV,
  DEINITSM_1000_NUM_STATES,
};

enum deinitsm_1001_states {
  DEINITSM_1001_WRITEV,
  DEINITSM_1001_NUM_STATES,
};

static void
deinitsm_2016_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEINITSM_2016_WRITEV:
      sm_write_regs (ssm, _dev, deinitsm_2016_writev, G_N_ELEMENTS (deinitsm_2016_writev));
      break;
    }
}

static void
deinitsm_1000_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEINITSM_1000_WRITEV:
      sm_write_regs (ssm, _dev, deinitsm_1000_writev, G_N_ELEMENTS (deinitsm_1000_writev));
      break;
    }
}

static void
deinitsm_1001_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEINITSM_1001_WRITEV:
      sm_write_regs (ssm, _dev, deinitsm_1001_writev, G_N_ELEMENTS (deinitsm_1001_writev));
      break;
    }
}

/***** INITIALIZATION *****/

enum initsm_2016_states {
  INITSM_2016_WRITEV_1,
  INITSM_2016_READ_09,
  INITSM_2016_WRITE_09,
  INITSM_2016_READ_13,
  INITSM_2016_WRITE_13,
  INITSM_2016_WRITE_04,
  INITSM_2016_WRITE_05,
  INITSM_2016_NUM_STATES,
};

enum initsm_1000_states {
  INITSM_1000_WRITEV_1,
  INITSM_1000_NUM_STATES,
};

enum initsm_1001_states {
  INITSM_1001_WRITEV_1,
  INITSM_1001_WRITEV_2,
  INITSM_1001_WRITEV_3,
  INITSM_1001_WRITEV_4,
  INITSM_1001_WRITEV_5,
  INITSM_1001_NUM_STATES,
};

static void
initsm_2016_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INITSM_2016_WRITEV_1:
      sm_write_regs (ssm, _dev, initsm_2016_writev_1, G_N_ELEMENTS (initsm_2016_writev_1));
      break;

    case INITSM_2016_READ_09:
      sm_read_reg (ssm, dev, 0x09);
      break;

    case INITSM_2016_WRITE_09:
      sm_write_reg (ssm, dev, 0x09, self->read_reg_result & ~0x08);
      break;

    case INITSM_2016_READ_13:
      sm_read_reg (ssm, dev, 0x13);
      break;

    case INITSM_2016_WRITE_13:
      sm_write_reg (ssm, dev, 0x13, self->read_reg_result & ~0x10);
      break;

    case INITSM_2016_WRITE_04:
      sm_write_reg (ssm, dev, 0x04, 0x00);
      break;

    case INITSM_2016_WRITE_05:
      sm_write_reg (ssm, dev, 0x05, 0x00);
      break;
    }
}

static void
initsm_1000_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INITSM_1000_WRITEV_1:
      if (FPI_DEVICE_UPEKSONLY (_dev)->dev_model == UPEKSONLY_1002)
        sm_write_regs_data (ssm, _dev, initsm_1002_writev_1, G_N_ELEMENTS (initsm_1002_writev_1));
      else
        sm_write_regs (ssm, _dev, initsm_1000_writev_1, G_N_ELEMENTS (initsm_1000_writev_1));
      break;
    }
}

static void
initsm_1001_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INITSM_1001_WRITEV_1:
      sm_write_regs (ssm, _dev, initsm_1001_writev_1, G_N_ELEMENTS (initsm_1001_writev_1));
      break;

    case INITSM_1001_WRITEV_2:
      sm_write_regs (ssm, _dev, initsm_1001_writev_2, G_N_ELEMENTS (initsm_1001_writev_2));
      break;

    case INITSM_1001_WRITEV_3:
      sm_write_regs (ssm, _dev, initsm_1001_writev_3, G_N_ELEMENTS (initsm_1001_writev_3));
      break;

    case INITSM_1001_WRITEV_4:
      sm_write_regs (ssm, _dev, initsm_1001_writev_4, G_N_ELEMENTS (initsm_1001_writev_4));
      break;

    case INITSM_1001_WRITEV_5:
      sm_write_regs (ssm, _dev, initsm_1001_writev_5, G_N_ELEMENTS (initsm_1001_writev_5));
      break;
    }
}

/***** CAPTURE LOOP *****/

static void
loopsm_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LOOPSM_RUN_AWFSM:;
      switch (self->dev_model)
        {
        case UPEKSONLY_1001:
          if (self->deactivating)
            fpi_ssm_mark_completed (ssm);
          else
            fpi_ssm_next_state (ssm);
          break;

        default:
          if (self->deactivating)
            {
              fpi_ssm_mark_completed (ssm);
            }
          else
            {
              FpiSsm *awfsm = NULL;
              switch (self->dev_model)
                {
                case UPEKSONLY_2016:
                  awfsm = fpi_ssm_new (FP_DEVICE (dev),
                                       awfsm_2016_run_state,
                                       AWFSM_2016_NUM_STATES);
                  break;

                case UPEKSONLY_1000:
                case UPEKSONLY_1002:
                  awfsm = fpi_ssm_new (FP_DEVICE (dev),
                                       awfsm_1000_run_state,
                                       AWFSM_1000_NUM_STATES);
                  break;

                default:
                  g_assert_not_reached ();
                }
              fpi_ssm_start_subsm (ssm, awfsm);
            }
          break;
        }
      break;

    case LOOPSM_DELAY_AWAIT_FINGER:
      if (self->deactivating)
        {
          fpi_ssm_mark_completed (ssm);
          break;
        }

      if (self->dev_model == UPEKSONLY_1002)
        fpi_ssm_next_state_delayed (ssm, AWAIT_FINGER_SETTLE_DELAY_MS_1002);
      else
        fpi_ssm_next_state (ssm);
      break;

    case LOOPSM_STATUS_READ:
      if (self->deactivating)
        {
          fpi_ssm_mark_completed (ssm);
          break;
        }

      if (self->dev_model == UPEKSONLY_1002)
        sm_read_status (ssm, dev);
      else
        fpi_ssm_next_state (ssm);
      break;

    case LOOPSM_AWAIT_FINGER:
      if (self->deactivating)
        {
          fpi_ssm_mark_completed (ssm);
          break;
        }

      switch (self->dev_model)
        {
        case UPEKSONLY_1001:
          fpi_ssm_next_state (ssm);
          break;

        default:
          sm_await_intr (ssm, dev);
          break;
        }
      break;

    case LOOPSM_RUN_CAPSM:;
      FpiSsm *capsm = NULL;
      switch (self->dev_model)
        {
        case UPEKSONLY_2016:
          capsm = fpi_ssm_new (FP_DEVICE (dev),
                               capsm_2016_run_state,
                               CAPSM_2016_NUM_STATES);
          break;

        case UPEKSONLY_1000:
          capsm = fpi_ssm_new (FP_DEVICE (dev),
                               capsm_1000_run_state,
                               CAPSM_1000_NUM_STATES);
          break;

        case UPEKSONLY_1002:
          capsm = fpi_ssm_new (FP_DEVICE (dev),
                               capsm_1002_run_state,
                               CAPSM_1002_NUM_STATES);
          break;

        case UPEKSONLY_1001:
          capsm = fpi_ssm_new (FP_DEVICE (dev),
                               capsm_1001_run_state,
                               CAPSM_1001_NUM_STATES);
          break;

        default:
          g_assert_not_reached ();
        }
      fpi_ssm_start_subsm (ssm, capsm);
      break;

    case LOOPSM_CAPTURE:
      break;

    case LOOPSM_RUN_DEINITSM:;
      FpiSsm *deinitsm = NULL;
      switch (self->dev_model)
        {
        case UPEKSONLY_2016:
          deinitsm = fpi_ssm_new (FP_DEVICE (dev),
                                  deinitsm_2016_run_state,
                                  DEINITSM_2016_NUM_STATES);
          break;

        case UPEKSONLY_1000:
        case UPEKSONLY_1002:
          deinitsm = fpi_ssm_new (FP_DEVICE (dev),
                                  deinitsm_1000_run_state,
                                  DEINITSM_1000_NUM_STATES);
          break;

        case UPEKSONLY_1001:
          deinitsm = fpi_ssm_new (FP_DEVICE (dev),
                                  deinitsm_1001_run_state,
                                  DEINITSM_1001_NUM_STATES);
          break;

        default:
          g_assert_not_reached ();
        }
      self->capturing = FALSE;
      fpi_ssm_start_subsm (ssm, deinitsm);
      break;

    case LOOPSM_FINAL:
      if (self->deactivating)
        fpi_ssm_mark_completed (ssm);
      else
        fpi_ssm_jump_to_state (ssm, LOOPSM_RUN_AWFSM);
      break;
    }

}

/***** DRIVER STUFF *****/

static void
deactivate_done (FpImageDevice *dev, GError *error)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  G_DEBUG_HERE ();
  free_img_transfers (self);
  g_clear_object (&self->await_cancellable);
  g_free (self->rowbuf);
  self->rowbuf = NULL;

  g_slist_free_full (self->rows, g_free);
  self->rows = NULL;

  fpi_image_device_deactivate_complete (dev, error);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  if (!self->capturing)
    {
      if (self->loopsm)
        {
          self->deactivating = TRUE;
          if (self->await_cancellable)
            g_cancellable_cancel (self->await_cancellable);
        }
      else
        {
          deactivate_done (dev, NULL);
        }
      return;
    }

  self->deactivating = TRUE;
  self->killing_transfers = ITERATE_SSM;
  self->kill_ssm = self->loopsm;
  cancel_img_transfers (dev);
}

static void
loopsm_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);


  self->loopsm = NULL;

  if (self->deactivating)
    {
      deactivate_done (dev, error);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (dev, error);
      return;
    }
}

static void
initsm_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (_dev);

  fpi_image_device_activate_complete (dev, error);
  if (error)
    return;

  self->loopsm = fpi_ssm_new (FP_DEVICE (dev), loopsm_run_state,
                              LOOPSM_NUM_STATES);
  fpi_ssm_start (self->loopsm, loopsm_complete);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);
  FpiSsm *ssm = NULL;
  int i;

  self->deactivating = FALSE;
  self->capturing = FALSE;

  self->num_flying = 0;
  self->img_transfers = g_ptr_array_new_with_free_func ((GFreeFunc) fpi_usb_transfer_unref);

  /* This might seem odd, but we do need multiple in-flight URBs so that
   * we never stop polling the device for more data.
   */
  for (i = 0; i < NUM_BULK_TRANSFERS; i++)
    {
      FpiUsbTransfer *transfer;

      transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
      fpi_usb_transfer_fill_bulk (transfer, 0x81, 4096);

      g_ptr_array_add (self->img_transfers, transfer);
    }

  switch (self->dev_model)
    {
    case UPEKSONLY_2016:
      ssm = fpi_ssm_new (FP_DEVICE (dev), initsm_2016_run_state,
                         INITSM_2016_NUM_STATES);
      break;

    case UPEKSONLY_1000:
    case UPEKSONLY_1002:
      ssm = fpi_ssm_new (FP_DEVICE (dev), initsm_1000_run_state,
                         INITSM_1000_NUM_STATES);
      break;

    case UPEKSONLY_1001:
      ssm = fpi_ssm_new (FP_DEVICE (dev), initsm_1001_run_state,
                         INITSM_1001_NUM_STATES);
      break;

    default:
      g_assert_not_reached ();
    }
  fpi_ssm_start (ssm, initsm_complete);
}

static void dev_init (FpImageDevice *dev);

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static gint
dev_discover (GUsbDevice *usb_device)
{
  guint16 pid = g_usb_device_get_pid (usb_device);
  guint16 bcd = g_usb_device_get_release (usb_device);

  if (pid == 0x2016)
    if (bcd == 1)             /* Revision 1 is what we're interested in */
      return 1;
  if (pid == 0x1000)
    if (bcd == 0x0033)             /* Looking for revision 0.33 */
      return 1;

  if (pid == 0x1002)
    if (bcd == 0x0048)             /* TouchStrip Fingerprint Sensor */
      return 1;

  if (pid == 0x1001)
    return 1;

  return 0;
}

static const FpIdEntry id_table[] = {
  { .vid = 0x147e,  .pid = 0x2016, .driver_data = UPEKSONLY_2016 },
  { .vid = 0x147e,  .pid = 0x1000, .driver_data = UPEKSONLY_1000 },
  { .vid = 0x147e,  .pid = 0x1002, .driver_data = UPEKSONLY_1002 },
  { .vid = 0x147e,  .pid = 0x1001, .driver_data = UPEKSONLY_1001 },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },
};

static void
fpi_device_upeksonly_init (FpiDeviceUpeksonly *self)
{
}
static void
fpi_device_upeksonly_class_init (FpiDeviceUpeksonlyClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "upeksonly";
  dev_class->full_name = "UPEK TouchStrip Sensor-Only";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  dev_class->usb_discover = dev_discover;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;

  img_class->img_width = -1;
  img_class->img_height = -1;
}

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;
  FpiDeviceUpeksonly *self = FPI_DEVICE_UPEKSONLY (dev);

  if (!g_usb_device_set_configuration (fpi_device_get_usb_device (FP_DEVICE (dev)), 1, &error))
    {
      fp_err ("could not set configuration 1");
      fpi_image_device_open_complete (dev, error);
    }

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->assembling_ctx.max_height = 1024;
  self->assembling_ctx.resolution = 8;
  self->assembling_ctx.median_filter_size = 25;
  self->assembling_ctx.max_search_offset = 30;

  self = FPI_DEVICE_UPEKSONLY (dev);
  self->dev_model = (int) fpi_device_get_driver_data (FP_DEVICE (dev));
  switch (self->dev_model)
    {
    case UPEKSONLY_1000:
      self->assembling_ctx.get_deviation = upeksonly_get_deviation2;
      self->assembling_ctx.get_pixel = upeksonly_get_pixel;
      self->img_width = IMG_WIDTH_1000;
      self->assembling_ctx.line_width = IMG_WIDTH_1000;
      break;

    case UPEKSONLY_1002:
      self->assembling_ctx.get_deviation = upeksonly_get_deviation_simple;
      self->assembling_ctx.get_pixel = upeksonly_get_pixel_simple;
      self->img_width = IMG_WIDTH_1002;
      self->assembling_ctx.line_width = IMG_WIDTH_1002;
      fpi_image_device_set_bz3_threshold (dev, 7);
      break;

    case UPEKSONLY_1001:
      self->assembling_ctx.get_deviation = upeksonly_get_deviation2;
      self->assembling_ctx.get_pixel = upeksonly_get_pixel;
      self->img_width = IMG_WIDTH_1001;
      self->assembling_ctx.line_width = IMG_WIDTH_1001;

      /* The sensor resolution is too low for the normal threshold. */
      fpi_image_device_set_bz3_threshold (dev, 25);
      break;

    case UPEKSONLY_2016:
      self->assembling_ctx.get_deviation = upeksonly_get_deviation2;
      self->assembling_ctx.get_pixel = upeksonly_get_pixel;
      self->img_width = IMG_WIDTH_2016;
      self->assembling_ctx.line_width = IMG_WIDTH_2016;
      break;

    default:
      g_assert_not_reached ();
    }
  fpi_image_device_open_complete (dev, NULL);
}
