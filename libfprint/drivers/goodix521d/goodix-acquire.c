/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Capture sequencing derived from the working Goodix TLS driver, copyright
 * 2021 Alexander Meiler, Matthieu CHARETTE and Michael Teuscher. */
#define FP_COMPONENT "goodix521d"
#include "goodix-acquire.h"
#include "goodix-timing.h"
#include "goodix-exposure.h"
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
  guint8 reference_seed[16], down_seed[16];
  gboolean context_valid, reference_seed_valid, refresh_required, reference_pending;
  guint8 pending_reference_seed[16];
  GxImage pending_reference_image, reference_image;
  GxExposureState exposure;
  guint16 live_image_dacs[4];
  gboolean exposure_valid;
  GoodixAcquired callback;
  gpointer user_data;
  gboolean reference, cancelled;
  GxTiming timing;
  guint timing_contact;
  const char *timing_phase;
  gboolean timing_active;
};

/* Diagnostic-only monotonic wall times. Neither these timestamps nor the
 * context identifiers participate in ownership, cancellation or deadlines. */
static void
acquisition_timing_log (GoodixAcquisition *self, const char *phase,
                        const char *status, gint64 elapsed_us)
{
  g_message ("goodix-timing action_id=%" G_GUINT64_FORMAT
             " reference=%u contact=%u phase=%s status=%s elapsed_us=%" G_GINT64_FORMAT,
             self->timing.action_id, (guint) self->reference, self->timing_contact,
             phase, status, elapsed_us);
}

static void
acquisition_timing_transition (GoodixAcquisition *self, const char *next_phase)
{
  if (!self->timing_active) return;
  gint64 now = g_get_monotonic_time ();
  acquisition_timing_log (self, self->timing_phase, "ok",
                          gx_timing_mark (&self->timing, now));
  self->timing_phase = next_phase;
}

static void
acquisition_timing_complete (GoodixAcquisition *self, const GError *error)
{
  if (!self->timing_active) return;
  self->timing_active = FALSE;
  gint64 now = g_get_monotonic_time ();
  const char *status = !error ? "ok" :
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "cancelled" : "error";
  acquisition_timing_log (self, self->timing_phase, status,
                          gx_timing_mark (&self->timing, now));
  acquisition_timing_log (self, "acquisition-total", status,
                          gx_timing_total (&self->timing, now));
}

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

enum { CONTACT_DOWN0, CONTACT_DOWN1, CONTACT_MANUAL0, CONTACT_MANUAL1, CONTACT_REGISTER,
       CONTACT_IMAGE, CONTACT_RESTORE, CONTACT_UP0, CONTACT_UP1, CONTACT_STATES };

static void
acquisition_complete (FpiSsm *ssm, FpDevice *device, GError *error)
{
  GoodixAcquisition *self = fpi_ssm_get_data (ssm);
  GoodixAcquired callback = self->callback;
  gpointer user_data = self->user_data;
  GxImage image = self->reference ? self->paired_images[0] : self->contact;
  GoodixAcquiredOutcome outcome = self->refresh_required ? GOODIX_ACQUIRED_REFRESH_REQUIRED : GOODIX_ACQUIRED_IMAGE;
  if (!error && self->reference) {
    if (!self->paired_f1_valid || gx_fdt_seed(self->paired_fdt[0],16,self->pending_reference_seed)!=GX_OK)
      error=fpi_device_error_new(FP_DEVICE_ERROR_PROTO);
    else {
      self->pending_reference_image=image;
      self->reference_pending=TRUE;
      self->context_valid=FALSE;
    }
  }
  if (error || outcome==GOODIX_ACQUIRED_REFRESH_REQUIRED) goodix_acquisition_invalidate_context(self);
  acquisition_timing_complete (self, error);
  self->ssm = NULL;
  self->callback = NULL;
  memset (self->paired_images, 0, sizeof self->paired_images);
  memset (&self->contact, 0, sizeof self->contact);
  callback (device, error || outcome==GOODIX_ACQUIRED_REFRESH_REQUIRED ? NULL : &image, outcome, user_data, error);
  memset (&image, 0, sizeof image);
}

static void
contact_manual (FpDevice *device, guint8 *data, guint16 length, gpointer user_data, GError *error)
{
  FpiSsm *ssm=user_data;
  GoodixAcquisition *self=fpi_ssm_get_data(ssm);
  (void)device;
  if (error) { fpi_ssm_mark_failed(ssm,error);return; }
  if (!acquisition_continue(self,ssm)) return;
  if (fpi_ssm_get_cur_state(ssm)==CONTACT_MANUAL0) {
    fpi_ssm_next_state(ssm);return;
  }
  GxFields fields;gboolean equal;
  if (fpi_ssm_get_cur_state(ssm)!=CONTACT_MANUAL1 || !data || length!=20 ||
      gx_fields(self->otp,64,&fields)!=GX_OK ||
      gx_preburst_fdt_equal(self->last_down+4,data+4,fields.timing,&equal)!=GX_OK) {
    fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
  }
  if (equal) {
    /* Windows requests reference refresh here. Conservatively obtain a real
     * current-contact UP first; similarity alone never proves reader absence. */
    self->refresh_required=TRUE;self->context_valid=FALSE;
    acquisition_timing_transition(self,"contact-up");
    fpi_ssm_jump_to_state(ssm,CONTACT_UP0);
  } else {
    acquisition_timing_transition(self,"contact-burst");
    fpi_ssm_next_state(ssm);
  }
}

static void
contact_register (FpDevice *device, gpointer user_data, GError *error)
{
  FpiSsm *ssm=user_data;
  GoodixAcquisition *self=fpi_ssm_get_data(ssm);
  (void)device;
  if (error) { fpi_ssm_mark_failed(ssm,error);return; }
  if (!acquisition_continue(self,ssm)) return;
  if (fpi_ssm_get_cur_state(ssm)==CONTACT_RESTORE)
    acquisition_timing_transition(self,"contact-up");
  fpi_ssm_next_state(ssm);
}

typedef struct { GoodixAcquisition *self; FpDevice *device; guint64 generation; } UpOwner;
static UpOwner *up_owner_new(GoodixAcquisition *self) {
  UpOwner *owner=g_new0(UpOwner,1);owner->self=self;self->refs++;
  owner->device=g_object_ref(self->device);owner->generation=self->generation;return owner;
}
static void up_owner_free(gpointer data) {
  UpOwner *owner=data;goodix_acquisition_free(owner->self);g_object_unref(owner->device);g_free(owner);
}

static void
contact_image (FpDevice *device, guint8 *data, guint16 length, gpointer user_data, GError *error)
{
  UpOwner *owner=user_data;
  GoodixAcquisition *self=owner->self;
  if (!self->ssm || self->generation!=owner->generation) {
    g_clear_error(&error);up_owner_free(owner);return;
  }
  FpiSsm *ssm=self->ssm;
  const guint8 prefix[5] = {0};
  guint64 generation = owner->generation;
  (void)device;
  if (error) { fpi_ssm_mark_failed (ssm, error); goto out; }
  if (!acquisition_continue(self,ssm)) goto out;
  if (fpi_ssm_get_cur_state(ssm)!=CONTACT_IMAGE || !self->exposure_valid || !self->reference_image.valid) {
    fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));goto out;
  }
  if (gx_image_blob (data, length, prefix, generation, generation, &self->contact) != GX_OK)
    { fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO)); goto out; }
  /* Feedback uses the unmodified native ADC planes. Its result applies only
   * to the next request; this capture keeps its original exposure and pixels. */
  guint16 background[GX_PIXELS],frame[GX_PIXELS];
  GxExposureMeasurement measurement;
  for (guint i=0;i<GX_PIXELS;i++) {
    background[i]=self->reference_image.pixels[2*i]|((guint16)self->reference_image.pixels[2*i+1]<<8);
    frame[i]=self->contact.pixels[2*i]|((guint16)self->contact.pixels[2*i+1]<<8);
  }
  GxExposureResult feedback=gx_exposure_update(&self->exposure,background,GX_PIXELS,
                                              frame,GX_PIXELS,self->live_image_dacs,&measurement);
  memset(background,0,sizeof background);memset(frame,0,sizeof frame);
  if (feedback!=GX_EXPOSURE_OK) {
    fpi_ssm_mark_failed(ssm,fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Exposure feedback input or context is invalid"));goto out;
  }
  self->frames++;
  acquisition_timing_transition(self,"contact-restore");
  fpi_ssm_jump_to_state(ssm,CONTACT_RESTORE);
out:
  up_owner_free(owner);
}
static void contact_removed(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  if (!self->ssm || self->generation!=owner->generation) { g_clear_error(&error);return; }
  if (!error && (!data || length!=20 || data[0]!=0 || data[1]!=2)) error=fpi_device_error_new(FP_DEVICE_ERROR_PROTO);
  if (error) fpi_ssm_mark_failed(self->ssm,error);
  else {
    FpiSsm *ssm=self->ssm;
    if (!acquisition_continue(self,ssm)) return;
    if (fpi_ssm_get_cur_state(ssm)!=CONTACT_UP1 || gx_fdt_seed(data+4,16,self->down_seed)!=GX_OK) {
      fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
    }
    fpi_device_report_finger_status(device,FP_FINGER_STATUS_NONE);
    if (acquisition_continue(self,ssm)) fpi_ssm_mark_completed(ssm);
  }
}
static void contact_down(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  if (!self->ssm || self->generation!=owner->generation) {g_clear_error(&error);return;}
  FpiSsm *ssm=self->ssm;
  if (error) {fpi_ssm_mark_failed(ssm,error);return;}
  if (!acquisition_continue(self,ssm)) return;
  if(fpi_ssm_get_cur_state(ssm)!=CONTACT_DOWN1 || !data || length!=20 || data[0]!=2 || data[1]!=0) {
    fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
  }
  memcpy(self->last_down,data,20);
  acquisition_timing_transition(self,"contact-preburst");
  fpi_device_report_finger_status(device,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
  if(acquisition_continue(self,ssm)) fpi_ssm_next_state(ssm);
}
static void contact_down_ack(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  (void)device;(void)data;(void)length;
  if(self->ssm && self->generation==owner->generation) {
    FpiSsm *ssm=self->ssm;
    if(error) fpi_ssm_mark_failed(ssm,error);
    else if(acquisition_continue(self,ssm) && fpi_ssm_get_cur_state(ssm)==CONTACT_DOWN0)
      fpi_ssm_next_state(ssm);
  } else g_clear_error(&error);
  up_owner_free(owner);
}
static void contact_up_phase0_ack(FpDevice *device,guint8 *data,guint16 length,gpointer user_data,GError *error) {
  UpOwner *owner=user_data;GoodixAcquisition *self=owner->self;
  (void)device;(void)data;(void)length;
  if (self->ssm && self->generation==owner->generation) {
    FpiSsm *ssm=self->ssm;
    if (error) fpi_ssm_mark_failed(ssm,error);
    else if (acquisition_continue(self,ssm)) fpi_ssm_next_state(ssm);
  } else g_clear_error(&error);
  up_owner_free(owner);
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
  GxFields fields;
  guint8 request[10] = {0x45,0x03};
  if (gx_fields (self->otp,64,&fields) != GX_OK)
    { fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO)); return; }
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CONTACT_DOWN0:
    case CONTACT_DOWN1: {
      guint phase=fpi_ssm_get_cur_state(ssm)==CONTACT_DOWN1;
      guint8 payload[40];
      if(!self->context_valid || !self->exposure_valid ||
         gx_down_request_with_dacs(self->otp,self->down_seed,16,phase,self->live_image_dacs,payload)!=GX_OK) {
        fpi_ssm_mark_failed(ssm,fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Detector context is not ready"));return;
      }
      if(!phase) {
        guint64 generation=self->generation;
        goodix_wait_fdt_down(device,30000,contact_down,up_owner_new(self),up_owner_free);
        if(!self->ssm || self->generation!=generation || !acquisition_continue(self,ssm)) return;
      }
      goodix_send_protocol(device,0x32,payload,40,NULL,TRUE,GOODIX_TIMEOUT,FALSE,contact_down_ack,up_owner_new(self));
      break;
    }
    case CONTACT_MANUAL0:
    case CONTACT_MANUAL1: {
      guint phase=fpi_ssm_get_cur_state(ssm)==CONTACT_MANUAL1;
      guint8 payload[27];
      if(gx_contact_manual_request(self->otp,self->reference_seed,16,phase,payload)!=GX_OK) {
        fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
      }
      goodix_send_protocol(device,0x36,payload,27,NULL,TRUE,GOODIX_TIMEOUT,phase,contact_manual,ssm);
      break;
    }
    case CONTACT_REGISTER:
      goodix_send_write_sensor_register (device,0x022c,(self->otp[23]&0x80)?0x0305:0x030a,contact_register,ssm);
      break;
    case CONTACT_RESTORE:
      /* The successful Windows image wrapper restores before its caller
       * proceeds to removal arming (RVA5a2bc/5a2f7). */
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
        /* APP10034 may report a new UP IRQ after phase0. Own that entire
         * window, but the transport requires both ordered phase ACKs before
         * publishing removal. No pre-arm packet is accepted. */
        UpOwner *event_owner=up_owner_new(self);
        guint64 generation=self->generation;
        goodix_wait_fdt_up(device,30000,contact_removed,event_owner,up_owner_free);
        if (self->ssm && self->generation==generation && acquisition_continue(self,ssm))
          goodix_send_protocol(device,0x34,payload,40,NULL,TRUE,GOODIX_TIMEOUT,FALSE,contact_up_phase0_ack,up_owner_new(self));
      } else {
        goodix_send_protocol(device,0x34,payload,40,NULL,TRUE,GOODIX_TIMEOUT,FALSE,contact_up_ack,up_owner_new(self));
      }
      break;
    }
    case CONTACT_IMAGE:
      if (!self->exposure_valid || gx_contact_image_request(self->live_image_dacs,request)!=GX_OK) {
        fpi_ssm_mark_failed(ssm,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return;
      }
      goodix_tls_read_image (device,request,sizeof request,contact_image,up_owner_new(self));
      break;
    }
}

GoodixAcquisition *
goodix_acquisition_new (FpDevice *device, const guint8 factory[64])
{
  GoodixAcquisition *self = g_new0 (GoodixAcquisition,1);
  self->device=device;self->refs=1;
  memcpy (self->otp,factory,64);
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
goodix_acquisition_set_timing_context (GoodixAcquisition *self, guint64 action_id, guint contact)
{
  g_return_if_fail (self && !self->ssm);
  self->timing.action_id = action_id;
  self->timing_contact = contact;
}

void
goodix_acquisition_start (GoodixAcquisition *self, gboolean reference, GoodixAcquired callback, gpointer user_data)
{
  g_return_if_fail (self && !self->ssm && callback);
  self->reference=reference; self->frames=0;self->generation++;self->cancelled=FALSE;self->refresh_required=FALSE;
  gx_timing_begin (&self->timing, self->timing.action_id, g_get_monotonic_time ());
  self->timing_phase = reference ? "reference-acquire" : "contact-down";
  self->timing_active = TRUE;
  memset(self->last_down,0,sizeof self->last_down);
  self->callback=callback; self->user_data=user_data;
  self->paired_retry_count=0;self->paired_f1_valid=FALSE;
  if(reference && self->reference_seed_valid) memcpy(self->paired_seed,self->reference_seed,16);
  else memset(self->paired_seed,0,16);
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
  if (self) {self->cancelled=TRUE;goodix_acquisition_invalidate_context(self);}
}

void
goodix_acquisition_invalidate_context (GoodixAcquisition *self)
{
  if (!self) return;
  self->context_valid=FALSE;self->reference_seed_valid=FALSE;self->reference_pending=FALSE;
  memset(self->pending_reference_seed,0,16);
  memset(self->reference_seed,0,16);memset(self->down_seed,0,16);
  memset(&self->pending_reference_image,0,sizeof self->pending_reference_image);
  memset(&self->reference_image,0,sizeof self->reference_image);
  memset(self->live_image_dacs,0,sizeof self->live_image_dacs);
  gx_exposure_clear(&self->exposure);self->exposure_valid=FALSE;
}

gboolean
goodix_acquisition_commit_reference (GoodixAcquisition *self)
{
  if (!self || self->ssm || !self->reference_pending || acquisition_cancelled(self)) return FALSE;
  GxFields fields;GxExposureState exposure;
  if (!self->pending_reference_image.valid || gx_fields(self->otp,64,&fields)!=GX_OK ||
      gx_exposure_reset(&exposure,fields.integration,fields.initial_image_dac)!=GX_EXPOSURE_OK) {
    goodix_acquisition_invalidate_context(self);return FALSE;
  }
  for (guint i=0;i<4;i++) if(fields.image[i]>511) {
    goodix_acquisition_invalidate_context(self);return FALSE;
  }
  self->reference_image=self->pending_reference_image;
  memset(&self->pending_reference_image,0,sizeof self->pending_reference_image);
  self->exposure=exposure;memcpy(self->live_image_dacs,fields.image,sizeof self->live_image_dacs);
  self->exposure_valid=TRUE;
  memcpy(self->reference_seed,self->pending_reference_seed,16);
  memcpy(self->down_seed,self->pending_reference_seed,16);
  memset(self->pending_reference_seed,0,16);
  self->reference_pending=FALSE;self->reference_seed_valid=TRUE;self->context_valid=TRUE;
  return TRUE;
}
