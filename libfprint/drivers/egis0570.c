/*
 * Egis Technology Inc. (aka. LighTuning) 0570 driver for libfprint
 * Copyright (C) 2021 Maxim Kolesnikov <kolesnikov@svyazcom.ru>
 * Copyright (C) 2021 Saeed/Ali Rk <saeed.ali.rahimi@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#define FP_COMPONENT "egis0570"

#include "egis0570.h"
#include "drivers_api.h"
#include "fp-device.h"

/* Packet types */
#define PKT_TYPE_INIT 0
#define PKT_TYPE_REPEAT 1

/* Struct */
struct CalibrationContext
{
  gboolean img_resp;
  int      pkt_size;
  guint8  *pkt_ptr;

  int      cal_step;


  guint8       req_packet[EGIS0570_PKTSIZE];
  guint8      *calibration_images;
  guint8       cal_img_idx;

  guint8       rng_pkt_low;
  guint8       rng_pkt_high;

  gint8        repeat_step;

  guint8       pkt_02_value;

  guint8       bs_steps;
  guint8       bs_condition;
  guint8       bs_mid;
  guint8       bs_twos;
  guint8       bs_zeros;
  guint8       bs_lows[EGIS0570_CAL_BS_ELM];
  guint8       bs_highs[EGIS0570_CAL_BS_ELM];
  unsigned int bs_row_sums[EGIS0570_CAL_BS_ELM];
  guint8       bs_pkts[EGIS0570_CAL_BS_ELM * EGIS0570_PKTSIZE];

};

struct _FpDeviceEgis0570
{
  FpImageDevice              parent;

  gboolean                   running;
  gboolean                   stop;
  gboolean                   calibrating;
  gboolean                   save_resp;

  guint8                     resp_state;
  guint8                     saved_resp;

  GSList                    *strips;
  guint8                    *background;
  gsize                      strips_len;

  int                        pkt_num;
  int                        pkt_type;

  guint8                     query_pkts[EGIS0570_INIT_TOTAL][EGIS0570_PKTSIZE];
  guint8                    *persistent_data;
  struct CalibrationContext *cal_ctx;
};
G_DECLARE_FINAL_TYPE (FpDeviceEgis0570, fpi_device_egis0570, FPI, DEVICE_EGIS0570, FpImageDevice);
G_DEFINE_TYPE (FpDeviceEgis0570, fpi_device_egis0570, FP_TYPE_IMAGE_DEVICE);

static unsigned char
egis_get_pixel (struct fpi_frame_asmbl_ctx *ctx, struct fpi_frame *frame, unsigned int x, unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx assembling_ctx = {
  .frame_width = EGIS0570_IMGWIDTH,
  .frame_height = EGIS0570_RFMGHEIGHT,
  .image_width = EGIS0570_IMGWIDTH * 4 / 3,
  .get_pixel = egis_get_pixel,
};


enum sm_caliberation_states {
  SM_CAL_INIT,
  SM_CAL_STEP_LOGIC,
  SM_CAL_STATIC_PKT,
  SM_CAL_REQ,
  SM_CAL_RESP,
  SM_CAL_RNG_PKT_REQ,
  SM_CAL_RNG_PKT_RESP,
  SM_CAL_BS_PK_REQ,
  SM_CAL_BS_PK_RESP,
  SM_CAL_CHK_NEW_CNF_REQ,
  SM_CAL_CHK_NEW_CNF_RESP,
  SM_CAL_DONE,
  SM_CAL_NUM,
};


/*
 * Service
 */


static gboolean
is_last_pkt (FpDevice *dev)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  int type = self->pkt_type;
  int num = self->pkt_num;

  gboolean r;

  if (self->calibrating)
    {
      struct CalibrationContext *cal_ctx = self->cal_ctx;
      r = (cal_ctx->pkt_size - 1) == num;
    }
  else
    {
      r = ((type == PKT_TYPE_INIT) && (num == (EGIS0570_INIT_TOTAL - 1)));
      r |= ((type == PKT_TYPE_REPEAT) && (num == (EGIS0570_REPEAT_TOTAL - 1)));
    }
  return r;
}

/*
 * Returns a bit for each frame on whether or not a finger has been detected.
 * e.g. 00110 means that there is a finger in frame two and three.
 */
static char
postprocess_frames (FpDeviceEgis0570 *self, guint8 * img)
{
  size_t mean[EGIS0570_IMGCOUNT] = {0, 0, 0, 0, 0};

  if (!self->background)
    {
      self->background = g_malloc (EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT);
      memset (self->background, 255, EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT);

      for (size_t k = 0; k < EGIS0570_IMGCOUNT; k += 1)
        {
          guint8 * frame = &img[(k * EGIS0570_IMGSIZE) + EGIS0570_RFMDIS * EGIS0570_IMGWIDTH];

          for (size_t i = 0; i < EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT; i += 1)
            self->background[i] = MIN (self->background[i], frame[i]);
        }

      return 0;
    }

  for (size_t k = 0; k < EGIS0570_IMGCOUNT; k += 1)
    {
      guint8 * frame = &img[(k * EGIS0570_IMGSIZE) + EGIS0570_RFMDIS * EGIS0570_IMGWIDTH];

      for (size_t i = 0; i < EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT; i += 1)
        {
          if (frame[i] - EGIS0570_MARGIN  > self->background[i])
            frame[i] -= self->background[i];
          else
            frame[i] = 0;

          mean[k] += frame[i];
        }

      mean[k] /= EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT;
    }

  char result = 0;

  for (size_t k = 0; k < EGIS0570_IMGCOUNT; k += 1)
    {
      fp_dbg ("Finger status (picture number, mean) : %ld , %ld", k, mean[k]);
      if (mean[k] > EGIS0570_MIN_MEAN)
        result |= 1 << k;
    }

  return result;
}

/*
 * Device communication
 */

static void
data_resp_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  unsigned char *stripdata;
  gboolean end = FALSE;
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  int where_finger_is = postprocess_frames (self, transfer->buffer);

  if (where_finger_is > 0)
    {
      FpiImageDeviceState state;

      fpi_image_device_report_finger_status (img_self, TRUE);

      g_object_get (dev, "fpi-image-device-state", &state, NULL);
      if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
        {
          for (size_t k = 0; k < EGIS0570_IMGCOUNT; k += 1)
            {
              if (where_finger_is & (1 << k))
                {
                  struct fpi_frame *stripe = g_malloc (EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT + sizeof (struct fpi_frame));
                  stripe->delta_x = 0;
                  stripe->delta_y = 0;
                  stripdata = stripe->data;
                  memcpy (stripdata, (transfer->buffer) + (((k) * EGIS0570_IMGSIZE) + EGIS0570_IMGWIDTH * EGIS0570_RFMDIS), EGIS0570_IMGWIDTH * EGIS0570_RFMGHEIGHT);
                  self->strips = g_slist_prepend (self->strips, stripe);
                  self->strips_len += 1;
                }
              else
                {
                  end = TRUE;
                  break;
                }
            }
        }
    }
  else
    {
      end = TRUE;
    }

  if (end)
    {
      if (!self->stop && (self->strips_len > 0))
        {
          g_autoptr(FpImage) img = NULL;
          self->strips = g_slist_reverse (self->strips);
          fpi_do_movement_estimation (&assembling_ctx, self->strips);
          img = fpi_assemble_frames (&assembling_ctx, self->strips);
          img->flags |= (FPI_IMAGE_COLORS_INVERTED | FPI_IMAGE_PARTIAL);
          g_slist_free_full (self->strips, g_free);
          self->strips = NULL;
          self->strips_len = 0;
          FpImage *resizeImage = fpi_image_resize (img, EGIS0570_RESIZE, EGIS0570_RESIZE);
          fpi_image_device_image_captured (img_self, g_steal_pointer (&resizeImage));
        }

      fpi_image_device_report_finger_status (img_self, FALSE);
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
recv_data_resp (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPIN, EGIS0570_INPSIZE);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, data_resp_cb, NULL);
}


static void
data_cal_resp_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);
  struct CalibrationContext *cal_ctx = self->cal_ctx;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  // assert bound ?
  memcpy (cal_ctx->calibration_images + (cal_ctx->cal_img_idx * EGIS0570_IMGSIZE), transfer->buffer, EGIS0570_IMGSIZE);
  cal_ctx->cal_img_idx++;

  fpi_ssm_jump_to_state (transfer->ssm, SM_CAL_STEP_LOGIC);
}

static void
recv_cal_data_resp (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPIN, EGIS0570_CAL_INPSIZE);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, data_cal_resp_cb, NULL);
}

static void
cmd_resp_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  if (error)
    fpi_ssm_mark_failed (transfer->ssm, error);

  if (self->save_resp)
    self->saved_resp = transfer->buffer[5];

  fpi_ssm_jump_to_state (transfer->ssm, self->resp_state);
}

static void
recv_cmd_resp (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPIN, EGIS0570_PKTSIZE);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, cmd_resp_cb, NULL);
}

static void
send_cmd_req (FpiSsm *ssm, FpDevice *dev, unsigned char *pkt)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, EGIS0570_EPOUT, pkt, EGIS0570_PKTSIZE, NULL);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, fpi_ssm_usb_transfer_cb, NULL);
}

/*
 * SSM States
 */

enum sm_states {
  SM_INIT,
  SM_START,
  SM_REQ,
  SM_RESP,
  SM_REC_DATA,
  SM_DONE,
  SM_STATES_NUM
};

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SM_INIT:
      self->calibrating = FALSE;
      self->pkt_type = PKT_TYPE_INIT;
      self->save_resp = FALSE;
      fpi_ssm_next_state (ssm);
      break;

    case SM_START:
      if (self->stop)
        {
          fp_dbg ("deactivating, marking completed");
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_deactivate_complete (img_dev, NULL);
        }
      else
        {
          self->pkt_num = 0;
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_REQ:
      if (self->pkt_type == PKT_TYPE_INIT)
        send_cmd_req (ssm, dev, self->query_pkts[self->pkt_num]);
      else
        send_cmd_req (ssm, dev, repeat_pkts[self->pkt_num]);
      break;

    case SM_RESP:
      if (is_last_pkt (dev) == FALSE)
        {
          self->pkt_num += 1;
          self->resp_state = SM_REQ;
          recv_cmd_resp (ssm, dev);
        }
      else
        {
          if (self->pkt_type == PKT_TYPE_INIT)
            self->pkt_type = PKT_TYPE_REPEAT;

          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_REC_DATA:
      recv_data_resp (ssm, dev);
      break;

    case SM_DONE:
      fpi_ssm_jump_to_state (ssm, SM_START);
      break;

    default:
      g_assert_not_reached ();
    }
}

/*
 * Calibration
 */


static void
set_ctx_pkts_type (FpDeviceEgis0570 *self, guint8 * element_ptr)
{
  self->pkt_type = -1;
  for (guint8 i = 0; i < EGIS0570_CAL_STATIC_LIST_SIZEOF; i++)
    {
      if (EGIS0570_CAL_STATIC_LIST[i] == element_ptr)
        {
          self->pkt_type = i;
          fp_dbg ("ptr said index %d", i);
          return;
        }
    }
  fp_dbg ("messed up list for static packets");
}

static void
set_ctx_pkts (FpDeviceEgis0570 *self)
{
  struct CalibrationContext *cal_ctx = self->cal_ctx;

  self->pkt_num = 0;
  cal_ctx->pkt_ptr = EGIS0570_CAL_STATIC_LIST[self->pkt_type];
  cal_ctx->pkt_size = EGIS0570_CAL_STATIC_LIST_SIZE[self->pkt_type];
}


static void
check_black_white_values (struct CalibrationContext *cal_ctx)
{
  guint8 * white_img = cal_ctx->calibration_images + (0 * EGIS0570_IMGSIZE);
  guint8 * black_img = cal_ctx->calibration_images + (1 * EGIS0570_IMGSIZE);

  for (size_t i = 0; i < EGIS0570_IMGSIZE; i++)
    {
      if (white_img[i] != 0xfe || black_img[i] != 0x00)
        {
          fp_dbg ("Black and white values are not right!");
          break;
        }
    }
}


static void
sum_pictures (guint8 * data, unsigned int * sums, guint8 f_idx, guint8 l_idx)
{
  for (guint8 pic_idx = f_idx; pic_idx <= l_idx; pic_idx++)
    {
      for (guint8 i = IMG_COL_IGNORE ; i < EGIS0570_IMGWIDTH - IMG_COL_IGNORE ; i++)
        {
          sums[0] += data[pic_idx * EGIS0570_IMGSIZE + i];
          sums[1] += data[pic_idx * EGIS0570_IMGSIZE + i + 1 * EGIS0570_IMGWIDTH];
          sums[2] += data[pic_idx * EGIS0570_IMGSIZE + i + 2 * EGIS0570_IMGWIDTH];
          sums[3] += data[pic_idx * EGIS0570_IMGSIZE + i + 3 * EGIS0570_IMGWIDTH];
          sums[4] += data[pic_idx * EGIS0570_IMGSIZE + i + 4 * EGIS0570_IMGWIDTH];
          sums[5] += data[pic_idx * EGIS0570_IMGSIZE + i + 5 * EGIS0570_IMGWIDTH];
          sums[6] += data[pic_idx * EGIS0570_IMGSIZE + i + 28 * EGIS0570_IMGWIDTH];
        }
    }
}

static void
apply_next_step_setting (FpiSsm *ssm, FpDeviceEgis0570 *self)
{
  struct CalibrationContext *cal_ctx = self->cal_ctx;

  cal_ctx->cal_step++;
  cal_ctx->pkt_size = 0;
  cal_ctx->img_resp = FALSE;

  self->pkt_num = 0;
  self->pkt_type = -1;
  self->save_resp = FALSE;

  if (cal_ctx->cal_step == EGIS0570_CAL_PROC_SIZEOF)
    {
      fp_dbg ("calibration step is higher thatn expected.");
      return;
    }
  fp_dbg ("Procedure Index %d", cal_ctx->cal_step);

  switch (calibration_procedure[cal_ctx->cal_step])
    {
    case CAL_CONF_PKT:
      set_ctx_pkts_type (self, *EGIS0570_CAL_CONFIGURATION_MODE_PKT);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_PKT_ZERO_RNG:
      set_ctx_pkts_type (self, *EGIS0570_CAL_PKT_ZERO_RANGE);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_CAPT_AREA:
      set_ctx_pkts_type (self, *EGIS0570_CAL_CAPTURING_AREA);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_SENS_AND_EMIT:
      set_ctx_pkts_type (self, *EGIS0570_CAL_SENSOR_AND_EMITTER);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_RNG_20_58:
      cal_ctx->rng_pkt_low = 0x20;
      cal_ctx->rng_pkt_high = 0x58;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = 0x00;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_RNG_60_D0:
      cal_ctx->rng_pkt_low = 0x60;
      cal_ctx->rng_pkt_high = 0xd0;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = 0x00;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_WHITE_SET:
      set_ctx_pkts_type (self, *EGIS0570_CAL_WHITE_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_BLACK_WHITE_GET_IMG:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BLACK_WHITE_GET_IMAGE);
      cal_ctx->img_resp = TRUE;
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_BLACK_WHITE_AFTER_IMAGE:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BLACK_WHITE_AFTER_IMAGE);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_BLACK_SET:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BLACK_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_CHECK_BLACK_WHITE:
      check_black_white_values (cal_ctx);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_5_ROWS_ZEROS:
      /* repeat this case*/
      if (cal_ctx->repeat_step == 0)
        {
          cal_ctx->repeat_step--;
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
          break;
        }
      cal_ctx->cal_step--;
      if (cal_ctx->repeat_step == -1)
        cal_ctx->repeat_step = 12;
      /* end repeat */

      if (cal_ctx->repeat_step % 2)
        cal_ctx->rng_pkt_low = 0x58 - ((12 - cal_ctx->repeat_step) / 2);
      else
        cal_ctx->rng_pkt_low = 0x20 + ((12 - cal_ctx->repeat_step) / 2);

      cal_ctx->rng_pkt_high = cal_ctx->rng_pkt_low;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = 0x00;
      cal_ctx->repeat_step--;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_BLACK_WHITE_CLEAR:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BLACK_WHITE_CLEAR);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_RNG_60_D1:
      cal_ctx->rng_pkt_low = 0x60;
      cal_ctx->rng_pkt_high = 0xd1;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = 0x00;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_MID_BLACK_SET:
      set_ctx_pkts_type (self, *EGIS0570_CAL_MIDDLE_BLACK_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_GET_IMG:
      set_ctx_pkts_type (self, *EGIS0570_CAL_GET_IMAGE);
      cal_ctx->img_resp = TRUE;
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_AFTER_IMG:
      set_ctx_pkts_type (self, *EGIS0570_CAL_AFTER_IMAGE);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_GET_9:
      /* repeat this case*/
      if (cal_ctx->repeat_step == 0)
        {
          cal_ctx->repeat_step--;
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
          break;
        }
      cal_ctx->cal_step--;
      if (cal_ctx->repeat_step == -1)
        cal_ctx->repeat_step = 9 * 3; // 27
      /* end repeat */

      if (cal_ctx->repeat_step % 3 == 0)
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_REPEAT);
        }
      else if (cal_ctx->repeat_step % 3 == 2)
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_GET_IMAGE);
          cal_ctx->img_resp = TRUE;
        }
      else
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_AFTER_IMAGE);
        }
      cal_ctx->repeat_step--;
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;


    case CAL_GET_21:
      /* repeat this case*/
      if (cal_ctx->repeat_step == 0)
        {
          cal_ctx->repeat_step--;
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
          break;
        }
      cal_ctx->cal_step--;
      if (cal_ctx->repeat_step == -1)
        cal_ctx->repeat_step = 21 * 3; // 27
      /* end repeat */

      if (cal_ctx->repeat_step % 3 == 0)
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_REPEAT);
        }
      else if (cal_ctx->repeat_step % 3 == 2)
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_GET_IMAGE);
          cal_ctx->img_resp = TRUE;
        }
      else
        {
          set_ctx_pkts_type (self, *EGIS0570_CAL_AFTER_IMAGE);
        }
      cal_ctx->repeat_step--;
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_BORDER_WHITE_SET:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BORDER_WHITE_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;


    case CAL_FIRST_BS_SET:
      cal_ctx->bs_steps = 0;
      cal_ctx->bs_condition = 0x7f;
      cal_ctx->bs_mid = 0;

      for (guint8 i = 0; i < EGIS0570_CAL_BS_ELM; i++)
        {
          cal_ctx->bs_lows[i] = 0x00;
          cal_ctx->bs_highs[i] = 0xff;
          cal_ctx->bs_row_sums[i] = 0x00;
          for (guint8 k = 0; k < EGIS0570_PKTSIZE; k++)
            cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + k] = init_pkts[i * 2][k];
          cal_ctx->bs_pkts[(i + 1) * EGIS0570_PKTSIZE - 1] = 0x00;
        }
      cal_ctx->bs_pkts[EGIS0570_CAL_BS_ELM * EGIS0570_PKTSIZE - 2] = 0x3c;

      set_ctx_pkts_type (self, *EGIS0570_CAL_FIRST_BS_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;


    case CAL_BS_CHK_SET:
      if (cal_ctx->bs_condition && cal_ctx->bs_steps < 8)
        {
          cal_ctx->cal_img_idx = 0;
          cal_ctx->bs_steps++;
          fp_dbg ("CALIBERATION: BINARY SEARCH: CONDITION: %b, STEPS: %d", cal_ctx->bs_condition, cal_ctx->bs_steps);
          for (guint8 i = 0; i < EGIS0570_CAL_BS_ELM; i++)
            {
              fp_dbg ("CALIBERATION: BINARY SEARCH: %d: LOW: %x, HIGH: %x", i, cal_ctx->bs_lows[i], cal_ctx->bs_highs[i]);
              cal_ctx->bs_row_sums[i] = 0;
            }

          fpi_ssm_jump_to_state (ssm, SM_CAL_BS_PK_REQ);
        }
      else
        {
          cal_ctx->cal_step += 6;// Based on calibration_procedure.
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      break;

    case CAL_PKT_02:
      cal_ctx->rng_pkt_low = 0x02;
      cal_ctx->rng_pkt_high = 0x02;
      cal_ctx->req_packet[4] = 0x00;
      cal_ctx->req_packet[6] = cal_ctx->pkt_02_value;
      cal_ctx->pkt_02_value = 0xff;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_BEFORE_IMG:
      set_ctx_pkts_type (self, *EGIS0570_CAL_BEFORE_GET_IMAGE);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_BS_JUMP:
      sum_pictures (cal_ctx->calibration_images, cal_ctx->bs_row_sums, cal_ctx->cal_img_idx - 10, cal_ctx->cal_img_idx - 1);

      for (guint8 i = 0; i < EGIS0570_CAL_BS_ELM; i++)
        {
          cal_ctx->bs_row_sums[i] /= 10 * (EGIS0570_IMGWIDTH - (2 * IMG_COL_IGNORE));
          fp_dbg ("CALIBERATION: BINARY SEARCH: %d: PKT: %x, ROW_SUM: %d", i, cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6], cal_ctx->bs_row_sums[i]);
          if (cal_ctx->bs_row_sums[i] >= TARGET_UPPER + (SIDE_DIFF * (i < 6)))
            cal_ctx->bs_lows[i] = cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6] + ((cal_ctx->bs_highs[i] - cal_ctx->bs_lows[i]) == 1);
          else if (cal_ctx->bs_row_sums[i] <= TARGET_LOWER + (SIDE_DIFF * (i < 6)))
            cal_ctx->bs_highs[i] = cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6];
          else
            cal_ctx->bs_condition = cal_ctx->bs_condition & (0x7f - (1 << i));
        }

      cal_ctx->cal_step = cal_ctx->cal_step - 7;// Based on calibration_procedure.
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_BS_END:
      if (cal_ctx->bs_steps >= 8)
        fp_dbg ("CALIBERATION: ERROR EXTRA STEPS ON BINARY SEARCH.");

      /* save in list */
      for (guint8 i = 0; i < EGIS0570_CAL_BS_ELM; i++)
        {
          self->persistent_data[i] = ((cal_ctx->bs_pkts[6 * EGIS0570_PKTSIZE + 6] * (i < 6)) > cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6]) ? 0x00 : (cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6] - (cal_ctx->bs_pkts[6 * EGIS0570_PKTSIZE + 6] * (i < 6)));
          fp_dbg ("CALIBERATION: SAVED presistenet data %x", self->persistent_data[i]);
        }
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;


    case CAL_PKT_15_0:
      cal_ctx->cal_img_idx = 0;

      cal_ctx->rng_pkt_low = 0x15;
      cal_ctx->rng_pkt_high = 0x15;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = 0x00;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_PKT_16_3C:
      cal_ctx->rng_pkt_low = 0x16;
      cal_ctx->rng_pkt_high = 0x16;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = self->persistent_data[6];
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_CHK_NEW_CONF:
      for (guint8 i = 0; i < (EGIS0570_CAL_BS_ELM - 1); i++)
        cal_ctx->bs_pkts[i * EGIS0570_PKTSIZE + 6] = self->persistent_data[i];
      cal_ctx->pkt_02_value = 0x00;
      fpi_ssm_jump_to_state (ssm, SM_CAL_CHK_NEW_CNF_REQ);
      break;

    case CAL_NO_IMG_CAP_AREA:
      cal_ctx->cal_img_idx = 0;

      set_ctx_pkts_type (self, *EGIS0570_CAL_NO_IMAGE_CAPTURING_AREA);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_NO_IMG_SET_0:
      set_ctx_pkts_type (self, *EGIS0570_CAL_NO_IMAGE_SETTING_0);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_NO_IMG_03_1C:
      self->save_resp = TRUE;
      cal_ctx->rng_pkt_low = 0x03;
      cal_ctx->rng_pkt_high = 0x03;
      cal_ctx->req_packet[4] = 0x00;
      cal_ctx->req_packet[6] = 0x1C;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_NO_CHK_03_80:
      if (self->saved_resp != 0x80)
        fp_dbg ("unexpected value (%x)for 0x00 0x03 0x1c -> 0x80.", self->saved_resp);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_NO_SET_03_80:
      cal_ctx->rng_pkt_low = 0x03;
      cal_ctx->rng_pkt_high = 0x03;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = self->saved_resp;
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_NO_IMG_14_BS_SET:
      cal_ctx->bs_steps = 0;
      cal_ctx->bs_condition = 0x01;
      cal_ctx->bs_mid = 0;
      cal_ctx->bs_lows[0] = 0x00;
      cal_ctx->bs_highs[0] = 0x3f;

      set_ctx_pkts_type (self, *EGIS0570_CAL_NO_IMAGE_BINARY_14_SETTING);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;


    case CAL_NO_IMG_14_BS_CHK_SET:
      if (cal_ctx->bs_condition && cal_ctx->bs_steps < 6 )
        {
          cal_ctx->bs_twos = 0;
          cal_ctx->bs_zeros = 0;
          cal_ctx->bs_steps++;

          fp_dbg ("CALIBERATION: NO IMAGE 14: CONDITION: %b, STEPS: %d", cal_ctx->bs_condition, cal_ctx->bs_steps);
          fp_dbg ("CALIBERATION: NO IMAGE 14: LOW: %x, HIGH: %x", cal_ctx->bs_lows[0], cal_ctx->bs_highs[0]);

          self->save_resp = FALSE;
          cal_ctx->rng_pkt_low = 0x14;
          cal_ctx->rng_pkt_high = 0x14;
          cal_ctx->req_packet[4] = 0x01;
          cal_ctx->req_packet[6] = cal_ctx->bs_lows[0] + ((cal_ctx->bs_highs[0] - cal_ctx->bs_lows[0]) / 2);
          fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
        }
      else
        {
          cal_ctx->cal_step += 3; // Based on calibration_procedure. //
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      break;


    case CAL_NO_IMG_16_BS_CHK_SET_15:
      if (cal_ctx->bs_condition && cal_ctx->bs_steps < 7)
        {
          cal_ctx->bs_twos = 0;
          cal_ctx->bs_zeros = 0;
          cal_ctx->bs_steps++;

          fp_dbg ("CALIBERATION: NO IMAGE 16: CONDITION: %b, STEPS: %d", cal_ctx->bs_condition, cal_ctx->bs_steps);
          fp_dbg ("CALIBERATION: NO IMAGE 16: LOW: %x, HIGH: %x", cal_ctx->bs_lows[0], cal_ctx->bs_highs[0]);

          self->save_resp = FALSE;
          cal_ctx->rng_pkt_low = 0x15;
          cal_ctx->rng_pkt_high = 0x15;
          cal_ctx->req_packet[4] = 0x01;
          cal_ctx->req_packet[6] = 0x00;
          fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
        }
      else
        {
          cal_ctx->cal_step += 4; // Based on calibration_procedure.
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      break;

    case CAL_NO_IMG_16_BS_SET_16:
      self->save_resp = FALSE;
      cal_ctx->rng_pkt_low = 0x16;
      cal_ctx->rng_pkt_high = 0x16;
      cal_ctx->req_packet[4] = 0x01;
      cal_ctx->req_packet[6] = cal_ctx->bs_lows[0] + ((cal_ctx->bs_highs[0] - cal_ctx->bs_lows[0]) / 2);
      fpi_ssm_jump_to_state (ssm, SM_CAL_RNG_PKT_REQ);
      break;

    case CAL_NO_IMG_PRE_FIRST:
      set_ctx_pkts_type (self, *EGIS0570_CAL_NO_IMAGE_PRE_FIRST);
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;

    case CAL_NO_IMG_GET_8:
      /* repeat this case*/
      if (cal_ctx->repeat_step == 0)
        {

          if (self->saved_resp == 0x02)
            cal_ctx->bs_twos++;
          else if(self->saved_resp == 0x00)
            cal_ctx->bs_zeros++;
          else
            fp_dbg ("unexpected value for no img 14/16 request (%x)", self->saved_resp);

          cal_ctx->repeat_step--;
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
          break;
        }
      cal_ctx->cal_step--;
      if (cal_ctx->repeat_step == -1)
        cal_ctx->repeat_step = 8;
      /* end repeat */

      if (cal_ctx->repeat_step < 8)
        {
          if (self->saved_resp == 0x02)
            cal_ctx->bs_twos++;
          else if(self->saved_resp == 0x00)
            cal_ctx->bs_zeros++;
          else
            fp_dbg ("unexpected value for no img 14/16 request (%x)", self->saved_resp);
        }

      set_ctx_pkts_type (self, *EGIS0570_CAL_NO_IMAGE_REQUEST);
      self->save_resp = TRUE;
      cal_ctx->repeat_step--;
      fpi_ssm_jump_to_state (ssm, SM_CAL_STATIC_PKT);
      break;


    case CAL_NO_IMG_14_BS_JUMP:
      if (cal_ctx->bs_twos > cal_ctx->bs_zeros)
        cal_ctx->bs_highs[0] = cal_ctx->req_packet[6];
      else if (cal_ctx->bs_twos < cal_ctx->bs_zeros)
        cal_ctx->bs_lows[0] = cal_ctx->req_packet[6];
      else
        cal_ctx->bs_condition = 0x00;

      fp_dbg ("no image bs 14: twos: %d, zeros: %d.", cal_ctx->bs_twos, cal_ctx->bs_zeros);

      cal_ctx->cal_step = cal_ctx->cal_step - 4;// Based on calibration_procedure.
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;


    case CAL_NO_IMG_16_BS_JUMP:
      if (cal_ctx->bs_twos > cal_ctx->bs_zeros)
        cal_ctx->bs_highs[0] = cal_ctx->req_packet[6];
      else if (cal_ctx->bs_twos < cal_ctx->bs_zeros)
        cal_ctx->bs_lows[0] = cal_ctx->req_packet[6];
      else
        cal_ctx->bs_condition = 0x00;

      fp_dbg ("no image bs 16: twos: %d, zeros: %d.", cal_ctx->bs_twos, cal_ctx->bs_zeros);

      cal_ctx->cal_step = cal_ctx->cal_step - 5;// Based on calibration_procedure.
      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_NO_IMG_14_BS_END:
      if (cal_ctx->bs_steps >= 6)
        fp_dbg ("CALIBERATION: ERROR EXTRA STEPS ON BINARY SEARCH.");

      /* save in list */
      fp_dbg ("NO BS 14 found value (%x), set value (0x00)", cal_ctx->req_packet[6]);
      self->persistent_data[8] = 0x00;

      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_NO_IMG_16_BS_SET:
      cal_ctx->bs_steps = 0;
      cal_ctx->bs_condition = 0x01;
      cal_ctx->bs_mid = 0;
      cal_ctx->bs_lows[0] = 0x00;
      cal_ctx->bs_highs[0] = 0x7f;

      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_NO_IMG_16_BS_END:
      if (cal_ctx->bs_steps >= 7)
        fp_dbg ("CALIBERATION: ERROR EXTRA STEPS ON BINARY SEARCH.");

      /* save in list */
      fp_dbg ("NO BS 16 found value (%x), set value (0x0a)", cal_ctx->req_packet[6]);
      self->persistent_data[7] = 0x0a;

      fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
      break;

    case CAL_END:
      fpi_ssm_jump_to_state (ssm, SM_CAL_DONE);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
ssm_cal_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);
  struct CalibrationContext *cal_ctx = self->cal_ctx;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SM_CAL_INIT:
      cal_ctx->cal_step = -1;
      cal_ctx->repeat_step = -1;
      cal_ctx->calibration_images = g_malloc (EGIS0570_CAL_IMG_TOT * EGIS0570_IMGSIZE);
      memcpy (cal_ctx->req_packet, EGIS0570_CAL_NO_IMAGE_REQUEST[1], EGIS0570_PKTSIZE);
      cal_ctx->cal_img_idx = 0;
      cal_ctx->pkt_02_value = 0xff;

      self->pkt_num = 0;
      self->pkt_type = 0;
      self->persistent_data = g_malloc0 (PRESISTENT_DATA_LENGTH);
      self->save_resp = FALSE;
      self->saved_resp = 0x00;
      fpi_ssm_next_state (ssm);
      break;

    case SM_CAL_STEP_LOGIC:
      apply_next_step_setting (ssm, self);
      break;

    case SM_CAL_STATIC_PKT:
      set_ctx_pkts (self);
      fpi_ssm_next_state (ssm);
      break;

    case SM_CAL_REQ:
      send_cmd_req (ssm, dev, cal_ctx->pkt_ptr + (EGIS0570_PKTSIZE * self->pkt_num));
      break;

    case SM_CAL_RESP:
      if (is_last_pkt (dev) == FALSE)
        {
          self->pkt_num += 1;
          self->resp_state = SM_CAL_REQ;
          recv_cmd_resp (ssm, dev);
        }
      else
        {
          if (cal_ctx->img_resp)
            {
              recv_cal_data_resp (ssm, dev);
            }
          else
            {
              self->resp_state = SM_CAL_STEP_LOGIC;
              recv_cmd_resp (ssm, dev);
            }
        }
      break;

    case SM_CAL_RNG_PKT_REQ:
      if (cal_ctx->rng_pkt_low <= cal_ctx->rng_pkt_high)
        {
          cal_ctx->req_packet[5] = cal_ctx->rng_pkt_low;
          send_cmd_req (ssm, dev, cal_ctx->req_packet);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      break;

    case SM_CAL_RNG_PKT_RESP:
      cal_ctx->rng_pkt_low++;
      self->resp_state = SM_CAL_RNG_PKT_REQ;
      recv_cmd_resp (ssm, dev);
      break;


    case SM_CAL_BS_PK_REQ:
      if (self->pkt_num < EGIS0570_CAL_BS_ELM)
        {
          if (cal_ctx->bs_condition & (1 << self->pkt_num))
            {
              cal_ctx->bs_mid = cal_ctx->bs_lows[self->pkt_num] + ((cal_ctx->bs_highs[self->pkt_num] - cal_ctx->bs_lows[self->pkt_num]) / 2);
              cal_ctx->bs_pkts[self->pkt_num * EGIS0570_PKTSIZE + 6] = cal_ctx->bs_mid;
              cal_ctx->pkt_02_value = (cal_ctx->pkt_02_value > cal_ctx->bs_mid) ? cal_ctx->bs_mid : cal_ctx->pkt_02_value;
              send_cmd_req (ssm, dev, cal_ctx->bs_pkts + (self->pkt_num * EGIS0570_PKTSIZE));
            }
          else
            {
              self->pkt_num++;
              fpi_ssm_jump_to_state (ssm, SM_CAL_BS_PK_REQ);
            }
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      break;

    case SM_CAL_BS_PK_RESP:
      self->pkt_num += 1;
      self->resp_state = SM_CAL_BS_PK_REQ;
      recv_cmd_resp (ssm, dev);
      break;

    case SM_CAL_CHK_NEW_CNF_REQ:
      if (self->pkt_num == (EGIS0570_CAL_BS_ELM * 2 - 1))
        {
          fpi_ssm_jump_to_state (ssm, SM_CAL_STEP_LOGIC);
        }
      else
        {
          if (self->pkt_num < 6)
            {
              send_cmd_req (ssm, dev, cal_ctx->bs_pkts + (self->pkt_num * EGIS0570_PKTSIZE));
            }
          else if (self->pkt_num < 12)
            {
              cal_ctx->bs_pkts[(self->pkt_num - 6) * EGIS0570_PKTSIZE + 5] = 0x58 - (self->pkt_num - 6);
              send_cmd_req (ssm, dev, cal_ctx->bs_pkts + ((self->pkt_num - 6) * EGIS0570_PKTSIZE));
            }
          else
            {
              cal_ctx->bs_pkts[6 * EGIS0570_PKTSIZE + 6] = 0x00;
              send_cmd_req (ssm, dev, cal_ctx->bs_pkts + (6 * EGIS0570_PKTSIZE));
            }
        }
      break;

    case SM_CAL_CHK_NEW_CNF_RESP:
      self->pkt_num += 1;
      self->resp_state = SM_CAL_CHK_NEW_CNF_REQ;
      recv_cmd_resp (ssm, dev);
      break;

    case SM_CAL_DONE:
      g_clear_pointer (&cal_ctx->calibration_images, g_free);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
set_query_pkts (FpDeviceEgis0570 *self, guchar * data)
{
  // 20, 58
  self->query_pkts[0][6] = data[0];
  self->query_pkts[1][6] = data[0];
  // 21, 57
  self->query_pkts[2][6] = data[1];
  self->query_pkts[3][6] = data[1];
  // 22, 56
  self->query_pkts[4][6] = data[2];
  self->query_pkts[5][6] = data[2];
  // 23, 55
  self->query_pkts[6][6] = data[3];
  self->query_pkts[7][6] = data[3];
  // 24, 54
  self->query_pkts[8][6] = data[4];
  self->query_pkts[9][6] = data[4];
  // 25, 53
  self->query_pkts[10][6] = data[5];
  self->query_pkts[11][6] = data[5];
  // 16
  self->query_pkts[12][6] = data[6];
  // 9
  self->query_pkts[13][6] = data[7];
  // 14
  self->query_pkts[14][6] = data[8];
}

/*
 * Activation
 */

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  self->running = FALSE;
  g_clear_pointer (&self->background, g_free);

  if (error)
    fpi_image_device_session_error (img_dev, error);
}

static void
dev_activate (FpImageDevice *dev)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), ssm_run_state, SM_STATES_NUM);

  self->stop    = FALSE;

  fpi_ssm_start (ssm, loop_complete);

  self->running = TRUE;

  fpi_image_device_activate_complete (dev, NULL);
}


/*
 * Calibration Activation
 */

static void
cal_loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  set_query_pkts (self, self->persistent_data);

  // TODO to save uncomment
  /*
   *  GError *save_error = NULL;
   *  fp_device_set_persistent_data (dev, self->persistent_data, PRESISTENT_DATA_LENGTH, &save_error);
   *
   * if (save_error)
   *   fpi_image_device_session_error (img_dev, save_error);
   */

  g_clear_pointer (&self->persistent_data, g_free);

  g_clear_pointer (&self->cal_ctx, g_free);

  if (error)
    fpi_image_device_session_error (img_dev, error);

  fpi_image_device_open_complete (img_dev, error);
}

static void
cal_activate (FpImageDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), ssm_cal_run_state, SM_CAL_NUM);
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  self->calibrating = TRUE;
  self->cal_ctx = malloc (sizeof (struct CalibrationContext));

  fpi_ssm_start (ssm, cal_loop_complete);
}

/*
 * Opening
 */

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);
  memcpy (self->query_pkts, init_pkts, EGIS0570_INIT_TOTAL * EGIS0570_PKTSIZE);
  fp_dbg ("initialization");

  switch (fpi_device_get_driver_data (FP_DEVICE (dev)))
    {
    case PACKET_VERSION_1:
      fpi_image_device_open_complete (dev, error);
      fp_dbg ("static driver");
      break;

    case PACKET_CALIBRATION:
      fp_dbg ("calibration driver");

      gsize length;
      // check if caliberation happend
      if (fp_device_get_persistent_data (FP_DEVICE (dev), &self->persistent_data, &length, &error))
        {
          if (length == PRESISTENT_DATA_LENGTH)
            {
              fp_dbg ("saved data");

              set_query_pkts (self, self->persistent_data);

              fpi_image_device_open_complete (dev, error);
            }
          else
            {
              fp_dbg ("no Saved data, len is: %ld", length);
              g_clear_pointer (&self->persistent_data, g_free);
              cal_activate (dev);
            }
        }
      break;
    }

}

/*
 * Closing
 */

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  fpi_image_device_close_complete (dev, error);
}

/*
 * Deactivation
 */

static void
dev_deactivate (FpImageDevice *dev)
{
  FpDeviceEgis0570 *self = FPI_DEVICE_EGIS0570 (dev);

  if (self->running)
    self->stop = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

/*
 * Driver data
 */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0570, .driver_data = PACKET_CALIBRATION},
  // { .vid = 0x1c7a, .pid = 0x0570, .driver_data = PACKET_VERSION_1},
  { .vid = 0x1c7a, .pid = 0x0571, .driver_data = PACKET_CALIBRATION},
  // { .vid = 0x1c7a, .pid = 0x0571, .driver_data = PACKET_VERSION_1},
  { .vid = 0,      .pid = 0, .driver_data = 0},
};

static void
fpi_device_egis0570_init (FpDeviceEgis0570 *self)
{
}

static void
fpi_device_egis0570_class_init (FpDeviceEgis0570Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "egis0570";
  dev_class->full_name = "Egis Technology Inc. (aka. LighTuning) 0570";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;

  img_class->img_width = EGIS0570_IMGWIDTH;
  img_class->img_height = -1;

  img_class->bz3_threshold = EGIS0570_BZ3_THRESHOLD; /* security issue */
}
