/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Capture sequencing derived from the working Goodix TLS driver, copyright
 * 2021 Alexander Meiler, Matthieu CHARETTE and Michael Teuscher. */
#define FP_COMPONENT "goodix521d"
#include "goodix-acquire.h"
#include "transport/goodix.h"
#include <string.h>

struct _GoodixAcquisition {
  FpDevice *device;
  guint8 otp[64];
  guint8 paired_fdt[3][16], paired_seed[16];
  guint paired_retry_count;
  gboolean paired_f1_valid;
  GxImage paired_images[2];
  GxImage contact;
  FpiSsm *ssm;
  guint frames, refs;
  guint64 generation;
  guint8 last_down[20];
  guint8 down[39];
  GoodixAcquired callback;
  gpointer user_data;
  gboolean reference, cancelled;
};

static gboolean
acquisition_cancelled (GoodixAcquisition *self)
{
  GCancellable *cancel=fpi_device_get_cancellable(self->device);
  return self->cancelled || (cancel && g_cancellable_is_cancelled(cancel));
}
static gboolean
acquisition_continue (GoodixAcquisition *self, FpiSsm *ssm)
{
  if (self->ssm!=ssm) return FALSE;
  if (!acquisition_cancelled(self)) return TRUE;
  fpi_ssm_mark_failed(ssm,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Goodix acquisition cancelled"));
  return FALSE;
}

#include "goodix-paired-reference.h"

enum { CONTACT_DOWN0, CONTACT_DOWN1, CONTACT_MODE, CONTACT_REGISTER,
       CONTACT_IMAGE, CONTACT_RESTORE, CONTACT_UP0, CONTACT_UP1, CONTACT_STATES };

static void
acquisition_complete (FpiSsm *ssm, FpDevice *device, GError *error)
{
  GoodixAcquisition *self = fpi_ssm_get_data (ssm);
  GoodixAcquired callback = self->callback;
  gpointer user_data = self->user_data;
  GxImage image = self->reference ? self->paired_images[0] : self->contact;
  self->ssm = NULL;
  self->callback = NULL;
  memset (self->paired_images, 0, sizeof self->paired_images);
  memset (&self->contact, 0, sizeof self->contact);
  callback (device, error ? NULL : &image, user_data, error);
  memset (&image, 0, sizeof image);
}

static void
contact_command (FpDevice *device, guint8 *data, guint16 length, gpointer user_data, GError *error)
{
  FpiSsm *ssm=user_data;
  GoodixAcquisition *self=fpi_ssm_get_data(ssm);
  if (!error && fpi_ssm_get_cur_state(ssm)==CONTACT_DOWN1) {
    if (!data || length!=20 || data[0]!=2 || data[1]!=0)
      error=fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Invalid current Goodix DOWN event");
    else {
      memcpy(self->last_down,data,20);
      fpi_device_report_finger_status(device,FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
      if (!acquisition_continue(self,ssm)) return;
    }
  }
  if (error) fpi_ssm_mark_failed (user_data, error);
  else fpi_ssm_next_state (user_data);
}

static void
contact_register (FpDevice *device, gpointer user_data, GError *error)
{
  (void)device;
  if (error) fpi_ssm_mark_failed (user_data, error);
  else fpi_ssm_next_state (user_data);
}

static void
contact_image (FpDevice *device, guint8 *data, guint16 length, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  GoodixAcquisition *self = fpi_ssm_get_data (ssm);
  const guint8 prefix[5] = {0};
  guint64 generation = (guint64)(guintptr)ssm;
  (void)device;
  if (error) { fpi_ssm_mark_failed (ssm, error); return; }
  if (gx_image_blob (data, length, prefix, generation, generation, &self->contact) != GX_OK)
    { fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO)); return; }
  if (++self->frames < 10)
    fpi_ssm_jump_to_state (ssm, CONTACT_MODE);
  else
    fpi_ssm_jump_to_state (ssm, CONTACT_RESTORE);
}

typedef struct { GoodixAcquisition *self; FpDevice *device; guint64 generation; } UpOwner;
static UpOwner *up_owner_new(GoodixAcquisition *self) {
  UpOwner *owner=g_new0(UpOwner,1);owner->self=self;self->refs++;
  owner->device=g_object_ref(self->device);owner->generation=self->generation;return owner;
}
static void up_owner_free(gpointer data) {
  UpOwner *owner=data;goodix_acquisition_free(owner->self);g_object_unref(owner->device);g_free(owner);
}
static void contact_removed(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  if (!self->ssm || self->generation!=owner->generation) { g_clear_error(&error);return; }
  if (!error && (!data || length!=20 || data[0]!=0 || data[1]!=2)) error=fpi_device_error_new(FP_DEVICE_ERROR_PROTO);
  if (error) fpi_ssm_mark_failed(self->ssm,error);
  else {
    FpiSsm *ssm=self->ssm;
    fpi_device_report_finger_status(device,FP_FINGER_STATUS_NONE);
    if (acquisition_continue(self,ssm)) fpi_ssm_mark_completed(ssm);
  }
}
static void contact_up_ack(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  (void)device;(void)data;(void)length;
  if (self->ssm && self->generation==owner->generation) {
    if (error) fpi_ssm_mark_failed(self->ssm,error);
    else {
      /* The reader has now acknowledged the armed removal detector. Keep
       * NEEDED set throughout image acquisition so clients do not cue an early
       * lift; PRESENT alone now means the contact can be removed. */
      FpiSsm *ssm=self->ssm;
      fpi_device_report_finger_status(device,FP_FINGER_STATUS_PRESENT);
      (void)acquisition_continue(self,ssm);
    }
  } else g_clear_error(&error);
  up_owner_free(owner);
}

static void
contact_run (FpiSsm *ssm, FpDevice *device)
{
  GoodixAcquisition *self = fpi_ssm_get_data (ssm);
  if (!acquisition_continue(self,ssm)) return;
  /* This is the v3 settled contact recipe. Its static mode table is retained;
   * image requests use all four factory-decoded DAC words. */
  static const guint8 mode[27] = {
    0x0d,0x01,0x27,0x01,0x21,0x01,0x27,0x01,0x23,0x01,
    0x8d,0x8d,0x86,0x86,0x97,0x97,0x8f,0x8f,0x9b,0x9b,
    0x92,0x92,0x96,0x96,0x8c,0x8c,0x01 };
  GxFields fields;
  guint8 request[10] = {0x45,0x03};
  if (gx_fields (self->otp,64,&fields) != GX_OK)
    { fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO)); return; }
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CONTACT_DOWN0:
    case CONTACT_DOWN1:
      self->down[26] = fpi_ssm_get_cur_state (ssm) == CONTACT_DOWN1;
      goodix_send_mcu_switch_to_fdt_down (device,self->down,sizeof self->down,
         self->down[26],NULL,contact_command,ssm);
      break;
    case CONTACT_MODE:
      goodix_send_mcu_switch_to_fdt_mode (device,(guint8 *)mode,sizeof mode,FALSE,NULL,contact_command,ssm);
      break;
    case CONTACT_REGISTER:
      goodix_send_write_sensor_register (device,0x022c,(self->otp[23]&0x80)?0x0305:0x030a,contact_register,ssm);
      break;
    case CONTACT_RESTORE:
      /* The GF5298 Windows image-success wrapper restores this register before
       * returning to FDT arming (RVA 5a2bc/5a2f7). Preserve the established burst,
       * then require this volatile write's ACK before either UP phase. */
      goodix_send_write_sensor_register(device,0x022c,0x020a,contact_register,ssm);
      break;
    case CONTACT_UP0:
    case CONTACT_UP1: {
      guint phase=fpi_ssm_get_cur_state(ssm)==CONTACT_UP1;
      guint8 payload[40];
      if (gx_up_request(self->otp,self->last_down,20,phase,payload)!=GX_OK) {
        fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
      }
      if (!phase) {
        goodix_send_protocol(device,0x34,payload,40,NULL,TRUE,GOODIX_TIMEOUT,FALSE,contact_command,ssm);
      } else {
        UpOwner *event_owner=up_owner_new(self);
        guint64 generation=self->generation;
        goodix_wait_fdt_up(device,30000,contact_removed,event_owner,up_owner_free);
        if (self->ssm && self->generation==generation)
          goodix_send_protocol(device,0x34,payload,40,NULL,TRUE,GOODIX_TIMEOUT,FALSE,contact_up_ack,up_owner_new(self));
      }
      break;
    }
    case CONTACT_IMAGE:
      for (guint i=0;i<4;i++) { request[2+i*2]=fields.image[i]&255; request[3+i*2]=fields.image[i]>>8; }
      goodix_tls_read_image (device,request,sizeof request,contact_image,ssm);
      break;
    }
}

GoodixAcquisition *
goodix_acquisition_new (FpDevice *device, const guint8 factory[64])
{
  static const guint8 down[39] = {
    0x9c,0x01,0x27,0x01,0x21,0x01,0x21,0x01,0x23,0x01,
    0x8d,0x8d,0x86,0x86,0x97,0x97,0x8f,0x8f,0x9b,0x9b,
    0x92,0x92,0x96,0x96,0x8c,0x8c,0x00,0x00,0x05,0x03,
    0xa7,0x00,0xa1,0x00,0xa7,0x00,0xa3,0x00,0x00 };
  GoodixAcquisition *self = g_new0 (GoodixAcquisition,1);
  self->device=device;self->refs=1;
  memcpy (self->otp,factory,64);
  memcpy (self->down,down,sizeof down);
  self->down[2]=factory[33]; self->down[4]=factory[41];
  self->down[6]=factory[42]; self->down[8]=factory[43];
  self->down[32]=factory[26]; self->down[36]=factory[45];
  self->down[30]=factory[26]+6; self->down[34]=factory[45]+6;
  return self;
}

void
goodix_acquisition_free (GoodixAcquisition *self)
{
  if (!self) return;
  g_return_if_fail (self->refs>0);
  if (--self->refs) return;
  g_return_if_fail (!self->ssm);
  memset (self,0,sizeof *self);
  g_free (self);
}

void
goodix_acquisition_start (GoodixAcquisition *self, gboolean reference, GoodixAcquired callback, gpointer user_data)
{
  g_return_if_fail (self && !self->ssm && callback);
  self->reference=reference; self->frames=0;self->generation++;self->cancelled=FALSE;
  memset(self->last_down,0,sizeof self->last_down);
  self->callback=callback; self->user_data=user_data;
  self->paired_retry_count=0;self->paired_f1_valid=FALSE;
  memset (self->paired_seed,0,sizeof self->paired_seed);
  memset (self->paired_fdt,0,sizeof self->paired_fdt);
  memset (self->paired_images,0,sizeof self->paired_images);
  self->ssm=fpi_ssm_new (self->device,reference?paired_reference_run:contact_run,
                        reference?PAIRED_REFERENCE_NUM_STATES:CONTACT_STATES);
  fpi_ssm_set_data (self->ssm,self,NULL);
  fpi_ssm_start (self->ssm,acquisition_complete);
}

void
goodix_acquisition_cancel (GoodixAcquisition *self)
{
  if (self) self->cancelled=TRUE;
}
