/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GF5298 FpDevice integration. Transport startup derives from the Goodix TLS
 * driver by Alexander Meiler, Matthieu CHARETTE and Michael Teuscher (2021). */
#define FP_COMPONENT "goodix521d"
#include "drivers_api.h"
#include "transport/goodix.h"
#include "transport/goodixtls.h"
#include "goodix-acquire.h"
#include "goodix-input.h"
#include "goodix-timing.h"
#include "gx-print-envelope.h"
#include "chicago/openchicago.h"
#include <openssl/crypto.h>
#include <string.h>

typedef enum { ACTION_NONE, ACTION_OPEN, ACTION_ENROLL, ACTION_VERIFY, ACTION_IDENTIFY } Action;
typedef enum { TIMING_NONE, TIMING_BOOTSTRAP, TIMING_TLS, TIMING_REFERENCE,
               TIMING_CAPTURE, TIMING_CLEANUP } TimingPhase;

typedef struct _FpiDeviceGoodix521d {
  FpiDeviceGoodixTls parent;
  guint8 factory[64], configuration[256];
  gchar *input_dir, *key_path;
  GxPrintIdentity identity;
  GoodixAcquisition *acquisition;
  OcSession *session;
  GoodixTransportHealth reference_health;
  gboolean reference_valid, initializing_reference;
  gboolean cleanup_incomplete, close_pending;
  guint64 capture_generation;
  FpiSsm *boot;
  Action action;
  GxTiming timing, phase_timing;
  guint64 next_action_id;
  TimingPhase timing_phase;
  gboolean timing_active;
  gboolean transport_open, processing, cancelled, in_cancel, finishing;
  GError *deferred_error;
  guint contacts;
  GSource *deadline;
  gint64 deadline_us;
  GError *abort_error;
  GHashTable *seen_frames;
  GPtrArray *templates;
} FpiDeviceGoodix521d;

typedef FpiDeviceGoodixTlsClass FpiDeviceGoodix521dClass;
GType fpi_device_goodix521d_get_type (void);
G_DEFINE_TYPE (FpiDeviceGoodix521d, fpi_device_goodix521d, FPI_TYPE_DEVICE_GOODIXTLS)
#define SELF(dev) ((FpiDeviceGoodix521d *)(dev))

static void operation_error (FpDevice *device, GError *error);
static void start_contact (FpDevice *device);
static void start_hardware (FpDevice *device);
static void acquired (FpDevice *device, const GxImage *image, gpointer owner, GError *error);

/* Only fixed labels and monotonic scalar durations reach the journal. Error
 * text, biometric, identity, configuration, and template data are not logged. */
static const gchar *
timing_status (const GError *error)
{
  if (!error) return "ok";
  if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_CANCELLED)) return "cancelled";
  if (g_error_matches(error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT)) return "timeout";
  if (error->domain==FP_DEVICE_RETRY) return "retry";
  return "error";
}

static void
timing_log (FpiDeviceGoodix521d *self, const gchar *phase, gint64 elapsed, const gchar *status)
{
  static const gchar *const actions[]={"none","open","enroll","verify","identify"};
  if (!self->timing_active || self->action==ACTION_NONE) return;
  g_message("goodix-timing action_id=%" G_GUINT64_FORMAT " action=%s contact=%u phase=%s elapsed_us=%" G_GINT64_FORMAT " status=%s",
            self->timing.action_id,actions[self->action],self->contacts,phase,elapsed,status);
}

static void
timing_phase_begin (FpiDeviceGoodix521d *self, TimingPhase phase)
{
  self->timing_phase=phase;
  gx_timing_begin(&self->phase_timing,self->timing.action_id,g_get_monotonic_time());
}

static void
timing_phase_end (FpiDeviceGoodix521d *self, const gchar *status)
{
  static const gchar *const phases[]={"none","bootstrap","tls","reference","capture","cleanup"};
  if (self->timing_phase==TIMING_NONE) return;
  timing_log(self,phases[self->timing_phase],gx_timing_mark(&self->phase_timing,g_get_monotonic_time()),status);
  self->timing_phase=TIMING_NONE;
}

static void
timing_terminal (FpiDeviceGoodix521d *self, const gchar *status)
{
  if (!self->timing_active) return;
  timing_phase_end(self,status);
  timing_log(self,"overall",gx_timing_total(&self->timing,g_get_monotonic_time()),status);
  self->timing_active=FALSE;
}

static gboolean
cancelled (FpiDeviceGoodix521d *self)
{
  if (self->deadline_us && g_get_monotonic_time() >= self->deadline_us) {
    if (!self->abort_error) self->abort_error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"Goodix operation deadline exceeded");
    return TRUE;
  }
  GCancellable *c = fpi_device_get_cancellable (FP_DEVICE (self));
  return self->cancelled || (c && g_cancellable_is_cancelled (c));
}

static void
clear_deadline (FpiDeviceGoodix521d *self)
{
  if (self->deadline) { g_source_destroy (self->deadline); self->deadline = NULL; }
  self->deadline_us=0;
}

static gboolean
stop_transport (FpiDeviceGoodix521d *self, GError **error)
{
  if (!self->transport_open) return TRUE;
  if (!goodix_dev_deinit (FP_DEVICE (self), error)) return FALSE;
  self->transport_open = FALSE;
  return TRUE;
}

static gboolean
reference_health_matches (FpiDeviceGoodix521d *self, GError **error)
{
  GoodixTransportHealth current;
  if (!self->transport_open) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_INITIALIZED,"Goodix transport is not initialized");
    return FALSE;
  }
  if (!goodix_transport_health(FP_DEVICE(self),&current,error)) return FALSE;
  if (current.read_loop!=self->reference_health.read_loop ||
      current.tls!=self->reference_health.tls ||
      current.configuration!=self->reference_health.configuration) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Goodix reference generation is no longer current");
    return FALSE;
  }
  return TRUE;
}

static void
start_capture (FpDevice *device, gboolean reference)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if (cancelled(self)) { operation_error(device,NULL);return; }
  timing_phase_begin(self,reference?TIMING_REFERENCE:TIMING_CAPTURE);
  goodix_acquisition_set_timing_context(self->acquisition,self->timing.action_id,self->contacts);
  /* Scalar ownership survives queued callbacks without owning a pointer into
   * an acquisition that may already have been cancelled or replaced. */
  self->capture_generation++;
  goodix_acquisition_start(self->acquisition,reference,acquired,(gpointer)(guintptr)self->capture_generation);
}

static void
operation_error_complete (FpDevice *device, GError *error)
{
  FpiDeviceGoodix521d *self = SELF (device);
  Action action = self->action;
  timing_terminal(self,timing_status(error));
  clear_deadline (self);
  self->action = ACTION_NONE;self->finishing=FALSE;
  /* References are valid only for their live transport generation. Any failed
   * action tears it down; a later match action may build a fresh reference. */
  self->reference_valid=FALSE;self->initializing_reference=FALSE;
  g_clear_pointer (&self->session,oc_session_free);
  g_clear_pointer (&self->templates,g_ptr_array_unref);
  fpi_device_report_finger_status (device,FP_FINGER_STATUS_NONE);
  switch (action) {
  case ACTION_OPEN: fpi_device_open_complete (device,error); break;
  case ACTION_ENROLL: fpi_device_enroll_complete (device,NULL,error); break;
  case ACTION_VERIFY: fpi_device_verify_complete (device,error); break;
  case ACTION_IDENTIFY: fpi_device_identify_complete (device,error); break;
  case ACTION_NONE: g_clear_error (&error); break;
  }
}

static void
failed_open_reads_idle (FpDevice *device, gpointer unused, GError *cleanup)
{
  FpiDeviceGoodix521d *self = SELF (device);
  GError *error = g_steal_pointer (&self->deferred_error);
  (void) unused;
  if (!error) error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);
  /* fp_device_open() owns the generic GUsb open, but its error completion does
   * not close it. The interface/TLS teardown and every owned RX completion must
   * precede this close; cancelling a libusb transfer alone is insufficient. */
  if (!cleanup)
    g_usb_device_close (fpi_device_get_usb_device (device), &cleanup);
  if (cleanup) {
    g_prefix_error (&error, "Goodix USB cleanup incomplete (%s): ", cleanup->message);
    g_clear_error (&cleanup);
  }
  operation_error_complete (device,error);
}

static void
operation_error_now (FpDevice *device, GError *error)
{
  FpiDeviceGoodix521d *self = SELF (device);
  g_autoptr(GError) cleanup = NULL;
  Action action = self->action;
  if (action == ACTION_NONE) { g_clear_error (&error); return; }
  gboolean was_cancelled=cancelled(self);
  if (self->abort_error) { g_clear_error (&error); error = g_steal_pointer (&self->abort_error); }
  else if (was_cancelled) { g_clear_error (&error); error = g_error_new_literal (G_IO_ERROR,G_IO_ERROR_CANCELLED,"Goodix operation cancelled"); }
  timing_phase_end(self,timing_status(error));
  if (self->transport_open) timing_phase_begin(self,TIMING_CLEANUP);
  stop_transport (self, &cleanup);
  timing_phase_end(self,timing_status(cleanup));
  if (!error) error = g_steal_pointer (&cleanup);
  else if (cleanup) g_prefix_error (&error,"Goodix transport cleanup incomplete (%s): ",cleanup->message);
  if (!error) error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);
  if (action == ACTION_OPEN && !self->transport_open) {
    self->deferred_error=error;
    /* On drain timeout retain the USB handle: an eventual stale completion
     * still owns it, and this failed open must not permit unsafe reuse. */
    goodix_wait_read_idle(device,2000,failed_open_reads_idle,NULL);
    return;
  }
  operation_error_complete (device,error);
}

static gboolean
error_idle (gpointer data)
{
  FpDevice *device=data;
  FpiDeviceGoodix521d *self=SELF(device);
  operation_error_now(device,g_steal_pointer(&self->deferred_error));
  return G_SOURCE_REMOVE;
}
static void
operation_error (FpDevice *device, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if (self->action==ACTION_NONE || self->finishing) { g_clear_error(&error);return; }
  if (!self->deferred_error) self->deferred_error=error;
  else g_clear_error(&error);
  if (self->in_cancel) return;
  self->finishing=TRUE;clear_deadline(self);
  /* Unwind transport/parser/SSM callbacks before joining TLS or releasing USB. */
  g_idle_add_full(G_PRIORITY_DEFAULT,error_idle,g_object_ref(device),g_object_unref);
}

static void
cancel_operation (FpDevice *device)
{
  FpiDeviceGoodix521d *self = SELF (device);
  self->cancelled = TRUE;
  goodix_acquisition_cancel(self->acquisition);
  /* Synchronous unwind is owned by the transport. A running CPU worker owns
   * its session until its completion callback, which discards its result. */
  self->in_cancel=TRUE;
  goodix_cancel (device);
  self->in_cancel=FALSE;
  if (self->action != ACTION_NONE && !self->processing && !self->boot && !self->finishing)
    operation_error (device,g_steal_pointer(&self->deferred_error));
}

static void
operation_timeout (FpDevice *device, gpointer unused)
{
  FpiDeviceGoodix521d *self = SELF (device);
  (void)unused;
  self->deadline = NULL;
  self->abort_error = g_error_new_literal (G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"Goodix operation deadline exceeded");
  cancel_operation (device);
}

static void
begin_action (FpDevice *device, Action action, guint milliseconds)
{
  FpiDeviceGoodix521d *self = SELF (device);
  self->action=action; self->cancelled=FALSE; self->contacts=0;self->finishing=FALSE;
  self->initializing_reference=FALSE;self->capture_generation++;
  gx_timing_begin(&self->timing,++self->next_action_id,g_get_monotonic_time());
  self->timing_phase=TIMING_NONE;self->timing_active=TRUE;
  g_clear_error (&self->abort_error);
  self->deadline=fpi_device_add_timeout (device,milliseconds,operation_timeout,NULL,NULL);
  self->deadline_us=g_get_monotonic_time ()+(gint64)milliseconds*1000;
  g_source_set_ready_time (self->deadline,self->deadline_us);
}

/* Open and failed-context recovery create an authenticated generation. The
 * accepted reference and host session may serve later contacts only while its
 * read-loop, TLS and configuration generations remain healthy and unchanged. */
enum { BOOT_NOP, BOOT_ENABLE, BOOT_NOP2, BOOT_FIRMWARE, BOOT_KEY,
       BOOT_RESET, BOOT_IDLE, BOOT_CONFIGURATION, BOOT_STATES };

static void
boot_next (FpDevice *device, gpointer data, GError *error)
{
  if (!error && cancelled (SELF (device))) error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Goodix operation cancelled");
  if (error) fpi_ssm_mark_failed (data,error); else fpi_ssm_next_state (data);
}
static void
boot_firmware (FpDevice *device, gchar *firmware, gpointer data, GError *error)
{
  if (!error && (!firmware || strcmp(firmware,"GFUSB_GM168SEC_APP_10034")))
    error=fpi_device_error_new_msg(FP_DEVICE_ERROR_NOT_SUPPORTED,"Goodix521d requires firmware GFUSB_GM168SEC_APP_10034");
  boot_next(device,data,error);
}
static void
boot_key (FpDevice *device, gboolean success, guint32 flags, guint8 *digest, guint16 length, gpointer data, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);
  guint8 key[32]={0}, actual[32]; gsize actual_length=32;
  if (!error && (!success || flags!=0xbb020001 || !digest || length!=32))
    error=fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Invalid Goodix reader key digest response");
  if (!error && goodix_tls_load_psk_file(self->key_path,key,&error)) {
    g_autoptr(GChecksum) hash=g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(hash,key,sizeof key);g_checksum_get_digest(hash,actual,&actual_length);
    if (CRYPTO_memcmp(actual,digest,32)) error=fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Provisioned key does not match this reader");
    else goodix_tls_set_psk(device,key);
  }
  OPENSSL_cleanse(key,sizeof key);
  boot_next(device,data,error);
}
static void
boot_reset (FpDevice *device, gboolean success, guint16 number, gpointer data, GError *error)
{
  if (!error && (!success || number!=2048)) error=fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Unexpected Goodix reset response");
  boot_next(device,data,error);
}
static void
boot_configuration (FpDevice *device, gboolean success, gpointer data, GError *error)
{
  if (!error && !success) error=fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Goodix rejected volatile configuration");
  boot_next(device,data,error);
}
static void
boot_run (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if (cancelled(self)) { fpi_ssm_mark_failed(ssm,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Goodix operation cancelled"));return; }
  switch(fpi_ssm_get_cur_state(ssm)) {
  case BOOT_NOP: goodix_start_read_loop(device);goodix_send_nop(device,boot_next,ssm);break;
  case BOOT_ENABLE: goodix_send_enable_chip(device,TRUE,boot_next,ssm);break;
  case BOOT_NOP2: goodix_send_nop(device,boot_next,ssm);break;
  case BOOT_FIRMWARE: goodix_send_firmware_version(device,boot_firmware,ssm);break;
  case BOOT_KEY: goodix_send_preset_psk_read(device,0xbb020001,32,boot_key,ssm);break;
  case BOOT_RESET: goodix_send_reset(device,TRUE,20,boot_reset,ssm);break;
  case BOOT_IDLE: goodix_send_mcu_switch_to_idle_mode(device,20,boot_next,ssm);break;
  case BOOT_CONFIGURATION: goodix_send_upload_config_mcu(device,self->configuration,256,NULL,boot_configuration,ssm);break;
  }
}

typedef struct {
  guint16 frame[OC_FRAME_PIXELS];
  OcEnrollResult enrolled;
  OcVerifyResult verified;
  gint match_index;
  GBytes *completed_template;
  OcSession *new_session;
  /* Sequential GTask ownership: main initializes, worker writes, completion
   * reads only after return. These measure elapsed time, not CPU consumption. */
  GxTiming worker_timing;
  gint64 queue_us, algorithm_us;
  gboolean reference;
} ProcessResult;
static void
process_free (ProcessResult *result)
{
  g_clear_pointer(&result->completed_template,g_bytes_unref);
  g_clear_pointer(&result->new_session,oc_session_free);
  memset(result,0,sizeof *result);g_free(result);
}
static void
process_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  FpiDeviceGoodix521d *self=SELF(source_object);
  ProcessResult *result=task_data;
  GError *error=NULL;
  (void)cancellable;
  result->queue_us=gx_timing_mark(&result->worker_timing,g_get_monotonic_time());
  if (result->reference) {
    result->new_session=oc_session_new_for_profile(result->frame,GOODIX_CHICAGO_PROFILE_GF5298,&error);
  } else if (self->action==ACTION_ENROLL) {
    if (oc_enroll_add(self->session,result->frame,&result->enrolled,&error) && result->enrolled.complete)
      result->completed_template=oc_enroll_finish_complete(self->session,&error);
  } else {
    oc_identify(self->session,(GBytes *const *)self->templates->pdata,self->templates->len,
                result->frame,&result->verified,&result->match_index,NULL,&error);
  }
  result->algorithm_us=gx_timing_mark(&result->worker_timing,g_get_monotonic_time());
  if (error) g_task_return_error(task,error);
  else g_task_return_boolean(task,TRUE);
}

static void
process_done (GObject *source_object, GAsyncResult *async_result, gpointer unused)
{
  FpDevice *device=FP_DEVICE(source_object);
  FpiDeviceGoodix521d *self=SELF(device);
  GTask *task=G_TASK(async_result);
  ProcessResult *result=g_task_get_task_data(task);
  g_autoptr(GError) error=NULL;
  gint64 delivered_us=g_get_monotonic_time();
  (void)unused;
  self->processing=FALSE;
  gboolean success=g_task_propagate_boolean(task,&error);
  gboolean interrupted=success && cancelled(self);
  const gchar *status=self->abort_error ? timing_status(self->abort_error) :
                      interrupted ? "cancelled" : timing_status(error);
  if (success && !interrupted &&
      ((self->action==ACTION_ENROLL && !result->enrolled.accepted) ||
       ((self->action==ACTION_VERIFY || self->action==ACTION_IDENTIFY) && result->verified.reject!=OC_REJECT_NONE)))
    status="retry";
  timing_log(self,"queue",result->queue_us,status);
  timing_log(self,"algorithm",result->algorithm_us,status);
  timing_log(self,"callback",gx_timing_mark(&result->worker_timing,delivered_us),status);
  if (!success || interrupted) {
    operation_error(device,g_steal_pointer(&error));return;
  }
  if (!reference_health_matches(self,&error)) {
    operation_error(device,g_steal_pointer(&error));return;
  }
  if (result->reference) {
    g_clear_pointer(&self->session,oc_session_free);
    self->session=g_steal_pointer(&result->new_session);
    self->reference_valid=TRUE;self->initializing_reference=FALSE;
    if (self->action==ACTION_OPEN) {
      timing_terminal(self,"ok");
      clear_deadline(self);self->action=ACTION_NONE;
      fpi_device_open_complete(device,NULL);
    } else start_contact(device);
    return;
  }
  if (self->action==ACTION_ENROLL) {
    if (result->enrolled.complete) {
      FpPrint *print=NULL;
      g_autoptr(GVariant) envelope=gx_print_envelope_export(&self->identity,TRUE,result->completed_template,&error);
      if (!envelope) { operation_error(device,g_steal_pointer(&error));return; }
      if (cancelled(self)) { operation_error(device,NULL);return; }
      if (!reference_health_matches(self,&error)) { operation_error(device,g_steal_pointer(&error));return; }
      fpi_device_get_enroll_data(device,&print);
      fpi_print_set_type(print,FPI_PRINT_RAW);
      g_object_set(print,"fpi-data",envelope,NULL);
      timing_terminal(self,"ok");
      clear_deadline(self);self->action=ACTION_NONE;
      fpi_device_enroll_complete(device,g_object_ref(print),NULL);return;
    }
    GError *retry=NULL;
    if (!result->enrolled.accepted) retry=fpi_device_retry_new_msg(FP_DEVICE_RETRY_GENERAL,"%s",oc_reject_to_string(result->enrolled.reject));
    if (cancelled(self)) { g_clear_error(&retry);operation_error(device,NULL);return; }
    fpi_device_enroll_progress(device,result->enrolled.stage,NULL,retry);
    /* Progress callbacks may cancel synchronously. */
    if (cancelled(self)) { operation_error(device,NULL);return; }
    start_contact(device);return;
  }
  Action action=self->action;
  if (cancelled(self)) { operation_error(device,NULL);return; }
  if (result->verified.reject!=OC_REJECT_NONE) {
    GError *retry=fpi_device_retry_new_msg(FP_DEVICE_RETRY_GENERAL,"%s",oc_reject_to_string(result->verified.reject));
    if (action==ACTION_VERIFY) fpi_device_verify_report(device,FPI_MATCH_ERROR,NULL,retry);
    else fpi_device_identify_report(device,NULL,NULL,retry);
  } else if (action==ACTION_VERIFY) {
    fpi_device_verify_report(device,result->verified.match?FPI_MATCH_SUCCESS:FPI_MATCH_FAIL,NULL,NULL);
  } else {
    GPtrArray *prints=NULL;fpi_device_get_identify_data(device,&prints);
    FpPrint *match=result->match_index>=0 ? g_ptr_array_index(prints,result->match_index):NULL;
    fpi_device_identify_report(device,match,NULL,NULL);
  }
  /* Report callbacks may cancel; completion must still happen exactly once. */
  if (cancelled(self)) { operation_error(device,NULL);return; }
  timing_terminal(self,result->verified.reject!=OC_REJECT_NONE?"retry":"ok");
  clear_deadline(self);self->action=ACTION_NONE;
  g_clear_pointer(&self->templates,g_ptr_array_unref);
  if (action==ACTION_VERIFY) fpi_device_verify_complete(device,NULL);
  else fpi_device_identify_complete(device,NULL);
}

static void
acquired_process (FpDevice *device, const GxImage *image, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if (self->action==ACTION_NONE || self->finishing) {g_clear_error(&error);return;}
  if (error || cancelled(self)) { operation_error(device,error);return; }
  if (!reference_health_matches(self,&error)) { operation_error(device,error);return; }
  if (!image || !image->valid) { operation_error(device,fpi_device_error_new(FP_DEVICE_ERROR_PROTO));return; }
  if (!self->initializing_reference) {
    g_autofree char *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,image->pixels,sizeof image->pixels);
    if (g_hash_table_contains(self->seen_frames,digest)) {
      operation_error(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,"Repeated sensor frame rejected"));return;
    }
    g_hash_table_add(self->seen_frames,g_steal_pointer(&digest));
  }
  ProcessResult *result=g_new0(ProcessResult,1);result->match_index=-1;
  result->reference=self->initializing_reference;
  for(guint i=0;i<OC_FRAME_PIXELS;i++) result->frame[i]=image->pixels[2*i]|((guint16)image->pixels[2*i+1]<<8);
  g_autoptr(GTask) task=g_task_new(device,fpi_device_get_cancellable(device),process_done,NULL);
  g_task_set_task_data(task,result,(GDestroyNotify)process_free);
  /* Wait for bounded computation before destroying algorithm state, even if
   * cancelled. No result is committed after cancellation. */
  g_task_set_return_on_cancel(task,FALSE);
  self->processing=TRUE;
  gx_timing_begin(&result->worker_timing,self->timing.action_id,g_get_monotonic_time());
  g_task_run_in_thread(task,process_thread);
}

typedef struct { FpDevice *device; GxImage image; GError *error; guint64 action_id, generation; } Captured;
static gboolean
acquired_idle (gpointer data)
{
  Captured *captured=data;
  FpiDeviceGoodix521d *self=SELF(captured->device);
  if (self->timing.action_id!=captured->action_id || self->capture_generation!=captured->generation)
    return G_SOURCE_REMOVE;
  acquired_process(captured->device,&captured->image,g_steal_pointer(&captured->error));
  return G_SOURCE_REMOVE;
}
static void
captured_free (gpointer data)
{
  Captured *captured=data;
  g_clear_error(&captured->error);g_object_unref(captured->device);
  memset(captured,0,sizeof *captured);g_free(captured);
}
static void
acquired (FpDevice *device, const GxImage *image, gpointer owner, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if ((guint64)(guintptr)owner!=self->capture_generation) { g_clear_error(&error);return; }
  Captured *captured=g_new0(Captured,1);
  captured->action_id=self->timing.action_id;captured->generation=self->capture_generation;
  if (self->action!=ACTION_NONE && !self->finishing) {
    timing_phase_end(self,timing_status(error));
  }
  captured->device=g_object_ref(device);captured->error=error;
  if(image)captured->image=*image;
  g_idle_add_full(G_PRIORITY_DEFAULT,acquired_idle,captured,captured_free);
}

static void
tls_ready (FpDevice *device, gpointer unused, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);(void)unused;
  timing_phase_end(self,timing_status(error));
  if (error || cancelled(self)) { operation_error(device,error);return; }
  if (!goodix_transport_health(device,&self->reference_health,&error)) { operation_error(device,error);return; }
  start_capture(device,TRUE);
}
static void
boot_complete (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);(void)ssm;self->boot=NULL;
  timing_phase_end(self,timing_status(error));
  if (error || cancelled(self)) { operation_error(device,error);return; }
  timing_phase_begin(self,TIMING_TLS);
  goodix_tls(device,tls_ready,NULL);
}
static void
start_hardware (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);GError *error=NULL;
  timing_phase_begin(self,TIMING_BOOTSTRAP);
  if (cancelled(self)) { operation_error(device,NULL);return; }
  if (self->transport_open) { operation_error(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_BUSY,"Prior Goodix transport cleanup incomplete"));return; }
  if (!goodix_dev_init(device,&error)) { operation_error(device,error);return; }
  self->transport_open=TRUE;
  self->boot=fpi_ssm_new(device,boot_run,BOOT_STATES);
  fpi_ssm_start(self->boot,boot_complete);
}
static void
start_contact (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);
  GError *error=NULL;
  if (!self->reference_valid || !self->session) { operation_error(device,fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));return; }
  if (!reference_health_matches(self,&error)) { operation_error(device,error);return; }
  if (++self->contacts>64) { operation_error(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,"Enrollment contact limit exceeded"));return; }
  fpi_device_report_finger_status(device,FP_FINGER_STATUS_NEEDED);
  start_capture(device,FALSE);
}

static void
reference_reads_idle (FpDevice *device, gpointer owner, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);
  if (self->action==ACTION_NONE || self->finishing ||
      self->timing.action_id!=(guint64)(guintptr)owner) { g_clear_error(&error);return; }
  if (error || cancelled(self)) { operation_error(device,error);return; }
  start_hardware(device);
}

static void
rebuild_reference (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);
  GError *error=NULL;
  self->reference_valid=FALSE;self->initializing_reference=TRUE;
  g_clear_pointer(&self->session,oc_session_free);
  fpi_device_report_finger_status(device,FP_FINGER_STATUS_NONE);
  if (self->transport_open) {
    timing_phase_begin(self,TIMING_CLEANUP);
    gboolean stopped=stop_transport(self,&error);
    timing_phase_end(self,timing_status(error));
    if (!stopped) { operation_error(device,error);return; }
  }
  /* A cancelled old read must finish before a new generation can submit NOP:
   * generation rejection alone cannot stop that read consuming a new reply.
   * Do not begin a new action or reset its cancellation/deadline here. */
  goodix_wait_read_idle(device,2000,reference_reads_idle,(gpointer)(guintptr)self->timing.action_id);
}

static void
clear_open_resources (FpiDeviceGoodix521d *self)
{
  self->reference_valid=FALSE;self->initializing_reference=FALSE;
  self->capture_generation++;
  memset(&self->reference_health,0,sizeof self->reference_health);
  g_clear_pointer(&self->session,oc_session_free);
  g_clear_pointer(&self->acquisition,goodix_acquisition_free);
  g_clear_pointer(&self->seen_frames,g_hash_table_unref);
  g_clear_pointer(&self->templates,g_ptr_array_unref);
  g_clear_pointer(&self->input_dir,g_free);
  g_clear_pointer(&self->key_path,g_free);
  g_clear_error(&self->deferred_error);
  g_clear_error(&self->abort_error);
  memset(self->factory,0,sizeof self->factory);
  memset(self->configuration,0,sizeof self->configuration);
}

static void
finalize (GObject *object)
{
  FpiDeviceGoodix521d *self=SELF(object);
  /* Active GTasks/USB transfers retain the device; finalization is idle. */
  clear_deadline(self);
  clear_open_resources(self);
  G_OBJECT_CLASS(fpi_device_goodix521d_parent_class)->finalize(object);
}

static void
dev_open (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);GError *error=NULL;guint8 key[32];
  const gchar *override=g_getenv("LIBFPRINT_GOODIX521D_DATA_DIR");
  if (self->transport_open || self->cleanup_incomplete) { fpi_device_open_complete(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_BUSY,"Prior Goodix cleanup incomplete"));return; }
  clear_open_resources(self);
  begin_action(device,ACTION_OPEN,45000);
  self->initializing_reference=TRUE;
  self->input_dir=g_strdup(override?override:"/var/lib/libfprint/goodix521d");
  self->key_path=g_build_filename(self->input_dir,"psk.bin",NULL);
  g_autofree gchar *factory_path=g_build_filename(self->input_dir,"factory.bin",NULL);
  if (!goodix521d_read_private(factory_path,self->factory,64,&error) ||
      !goodix521d_configure(self->factory,self->configuration,&error) ||
      !goodix_tls_load_psk_file(self->key_path,key,&error)) {
    OPENSSL_cleanse(key,sizeof key);operation_error(device,error);return;
  }
  OPENSSL_cleanse(key,sizeof key);
  self->identity.backend="openchicago";self->identity.backend_version=1;
  self->identity.profile=17;self->identity.preprocess_version=1;
  g_autoptr(GChecksum) hash=g_checksum_new(G_CHECKSUM_SHA256);guint8 digest[32];gsize length=32;
  g_checksum_update(hash,self->factory,64);g_checksum_get_digest(hash,digest,&length);
  memcpy(self->identity.sensor_id,digest,16);g_checksum_reset(hash);
  g_checksum_update(hash,self->configuration,256);length=32;g_checksum_get_digest(hash,self->identity.config_digest,&length);
  self->acquisition=goodix_acquisition_new(device,self->factory);
  self->seen_frames=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
  start_hardware(device);
}
static void
close_reads_idle (FpDevice *device, gpointer unused, GError *error)
{
  FpiDeviceGoodix521d *self=SELF(device);(void)unused;
  if (!self->close_pending) { g_clear_error(&error);return; }
  self->close_pending=FALSE;
  if (error) {
    /* Ordinary close_complete closes GUsb even with an error. Keep both the
     * handle and remaining owners until a later, successful close retry. */
    fpi_device_close_abort(device,error);
    return;
  }
  clear_open_resources(self);
  self->cleanup_incomplete=FALSE;
  fpi_device_close_complete(device,NULL);
}

static void
dev_close (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);GError *error=NULL;
  self->cleanup_incomplete=TRUE;self->reference_valid=FALSE;self->close_pending=TRUE;
  clear_deadline(self);
  if (!stop_transport(self,&error)) {
    self->close_pending=FALSE;
    if (!error) error=fpi_device_error_new(FP_DEVICE_ERROR_GENERAL);
    fpi_device_close_abort(device,error);
    return;
  }
  goodix_wait_read_idle(device,2000,close_reads_idle,NULL);
}
static void
dev_enroll (FpDevice *device)
{
  FpiDeviceGoodix521d *self=SELF(device);GError *error=NULL;
  begin_action(device,ACTION_ENROLL,900000);
  if (self->cleanup_incomplete) { operation_error(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_BUSY,"Goodix close cleanup must be retried"));return; }
  if (!self->session || !oc_enroll_begin(self->session,OC_ENROLL_ENGINE,OC_ENGINE_STAGES,&error)) {
    operation_error(device,error);return;
  }
  start_contact(device);
}
static gboolean
load_print (FpiDeviceGoodix521d *self, FpPrint *print, GError **error)
{
  g_autoptr(GVariant) data=NULL;g_object_get(print,"fpi-data",&data,NULL);
  GBytes *blob=gx_print_envelope_import(data,&self->identity,error);
  if (!blob) return FALSE;
  g_ptr_array_add(self->templates,blob);return TRUE;
}
static void
start_match (FpDevice *device, Action action)
{
  FpiDeviceGoodix521d *self=SELF(device);GError *error=NULL;
  begin_action(device,action,60000);
  if (self->cleanup_incomplete) { operation_error(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_BUSY,"Goodix close cleanup must be retried"));return; }
  self->templates=g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
  if (action==ACTION_VERIFY) {
    FpPrint *print=NULL;fpi_device_get_verify_data(device,&print);
    if (!load_print(self,print,&error)) { operation_error(device,error);return; }
  } else {
    GPtrArray *prints=NULL;fpi_device_get_identify_data(device,&prints);
    /* fprintd checks for duplicates even before the first enrollment. An
     * empty gallery is valid: preserve capture/quality checks and let the
     * matcher report no match. Nonempty templates still require validation. */
    if (prints->len>100) { operation_error(device,fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID));return; }
    for(guint i=0;i<prints->len;i++) if (!load_print(self,g_ptr_array_index(prints,i),&error)) { operation_error(device,error);return; }
  }
  if (self->reference_valid && self->session && reference_health_matches(self,&error))
    start_contact(device);
  else {
    g_clear_error(&error);
    rebuild_reference(device);
  }
}
static void dev_verify(FpDevice *device) { start_match(device,ACTION_VERIFY); }
static void dev_identify(FpDevice *device) { start_match(device,ACTION_IDENTIFY); }
static void fpi_device_goodix521d_init(FpiDeviceGoodix521d *self) { (void)self; }
static void
fpi_device_goodix521d_class_init (FpiDeviceGoodix521dClass *klass)
{
  static const FpIdEntry ids[]={{.vid=0x27c6,.pid=0x521d},{.vid=0,.pid=0}};
  G_OBJECT_CLASS(klass)->finalize=finalize;
  FpDeviceClass *device_class=FP_DEVICE_CLASS(klass);
  FpiDeviceGoodixTlsClass *transport_class=FPI_DEVICE_GOODIXTLS_CLASS(klass);
  device_class->id="goodix521d";device_class->full_name="Goodix GF5298 (experimental free host matching)";
  device_class->type=FP_DEVICE_TYPE_USB;device_class->scan_type=FP_SCAN_TYPE_PRESS;
  device_class->id_table=ids;device_class->nr_enroll_stages=OC_ENGINE_STAGES;
  device_class->open=dev_open;device_class->close=dev_close;device_class->enroll=dev_enroll;
  device_class->verify=dev_verify;device_class->identify=dev_identify;device_class->cancel=cancel_operation;
  transport_class->interface=0;transport_class->ep_in=0x83;transport_class->ep_out=0x01;
  fpi_device_class_auto_initialize_features(device_class);
}
