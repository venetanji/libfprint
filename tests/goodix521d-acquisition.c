/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real acquisition and FpiSsm, synthetic transport. No USB is opened.
 * Substitute only the external boundaries while compiling the exact production
 * source. Linker --wrap is too late for LTO: GCC can bind/integrate definitions
 * from libfprint-private before the linker gets an undefined symbol to wrap. */
#include <glib.h>
#include <stdio.h>
static gint64 timing_clock_us = 1000;
static gint64 timing_clock_read(void) { return timing_clock_us; }
#include "../libfprint/drivers/goodix521d/goodix-exposure.h"
/* Count calls without replacing the real controller or its results. */
static guint adc_reset_calls, adc_update_calls;
#define gx_exposure_reset(...) (++adc_reset_calls, gx_exposure_reset(__VA_ARGS__))
#define gx_exposure_update(...) (++adc_update_calls, gx_exposure_update(__VA_ARGS__))
#define g_get_monotonic_time timing_clock_read
#define fpi_device_get_cancellable mock_fpi_device_get_cancellable
#define fpi_device_report_finger_status mock_fpi_device_report_finger_status
#define goodix_send_write_sensor_register mock_goodix_send_write_sensor_register
#define goodix_tls_read_image mock_goodix_tls_read_image
#define goodix_wait_fdt_up mock_goodix_wait_fdt_up
#define goodix_wait_fdt_down mock_goodix_wait_fdt_down
#define goodix_send_protocol mock_goodix_send_protocol
#include "../libfprint/drivers/goodix521d/goodix-acquire.c"
#undef g_get_monotonic_time
#undef gx_exposure_reset
#undef gx_exposure_update
#include "test-device-fake.h"
#include <string.h>
typedef struct {
 guint64 action_id;
 guint reference,contact;
 char phase[32],status[16];
 gint64 elapsed_us;
} AcquisitionTiming;
static GArray *timing_records;
static void capture_timing(const gchar *domain,GLogLevelFlags level,const gchar *message,gpointer data) {
 (void)domain;(void)level;(void)data;
 if (!g_str_has_prefix(message,"goodix-timing ")) return;
 AcquisitionTiming record={0};int end=0;
 g_assert_cmpint(sscanf(message,"goodix-timing action_id=%" G_GUINT64_FORMAT
   " reference=%u contact=%u phase=%31s status=%15s elapsed_us=%" G_GINT64_FORMAT "%n",
   &record.action_id,&record.reference,&record.contact,record.phase,record.status,&record.elapsed_us,&end),==,6);
 g_assert_cmpint(message[end],==,0);g_assert_cmpint(record.elapsed_us,>=,0);
 g_array_append_val(timing_records,record);
}
static void reset_timing(void) {g_array_set_size(timing_records,0);timing_clock_us=1000;}
static const AcquisitionTiming *timing_at(guint index,const char *phase,const char *status) {
 g_assert_cmpuint(index,<,timing_records->len);
 const AcquisitionTiming *record=&g_array_index(timing_records,AcquisitionTiming,index);
 g_assert_cmpstr(record->phase,==,phase);g_assert_cmpstr(record->status,==,status);
 return record;
}
/* A source-executed protocol trace. Synthetic completions replace only the
 * transport boundary: the real acquisition and FpiSsm own every transition. */
typedef struct {
  GoodixDefaultCallback callback;
  gpointer data;
  GDestroyNotify destroy;
} EventWait;

typedef enum {
  FAIL_NONE, FAIL_DOWN0, FAIL_DOWN1, FAIL_MANUAL0, FAIL_MANUAL1,
  FAIL_CAPTURE_REGISTER, FAIL_IMAGE, FAIL_RESTORE, FAIL_UP0, FAIL_UP1
} ContactFailure;

static struct {
  ContactFailure failure, cancel_command;
  guint failure_kind, frame, corrupt_frame;
  gboolean hold_down1, hold_down_event, hold_manual1, hold_up1, hold_up_event;
  guint hold_restore;
  gboolean cancel_present, cancel_removal, cancel_none, free_on_complete, pattern_image;
  guint down_length, manual_length, up_length;
  gboolean use_flat_pixel, hold_image;
  guint16 flat_pixel;
  gboolean check_feedback;
  guint updates_before_image, expected_high_count, expected_steps;
  guint16 expected_next_dacs[4];
} options;

static struct {
  guint frames, registers, restores, restore_acks, manual_sends;
  guint down_arms, up_arms, down_ack_mask, up_ack_mask;
  guint done, present_reports, removal_reports, none_reports;
  FpFingerStatusFlags status;
  GoodixAcquiredOutcome outcome;
  GxImage image;
  guint8 expected_pixels[GX_PIXELS*2];
  EventWait down_wait, up_wait;
  GoodixCmdCallback held_down1, held_manual1, held_up1;
  gpointer held_down1_data, held_manual1_data, held_up1_data;
  GoodixImageCallback held_image;
  gpointer held_image_data;
  guint8 held_image_blob[GX_BLOB_SIZE];
  guint16 request_dacs[4];
  GoodixNoneCallback held_restore;
  gpointer held_restore_data;
  guint8 down_reply[20], manual_reply[20], up_reply[20], expected_seed[16];
  GString *trace;
} contact;

static GError *result_error;
static FpDevice *test_device;
static GoodixAcquisition *capture;
static gboolean capture_mode, reference_mode;
static gboolean adc_fixture, adc_minimum_fixture;
static void reference_protocol(FpDevice *, guint8, guint8 *, guint16,
                               GDestroyNotify, gboolean, guint, gboolean,
                               GoodixCmdCallback, gpointer);
static void reference_register(FpDevice *, guint16, guint16, GoodixNoneCallback, gpointer);
static void reference_image(FpDevice *, guint8 *, guint16, GoodixImageCallback, gpointer);
static void reference_blob(guint16, guint8 [GX_BLOB_SIZE]);
static const guint8 reference_seed[16];
static void assert_adc_context_cleared(void);

static void
trace(const char *step)
{
  timing_clock_us+=100;
  g_assert_nonnull(contact.trace);
  g_string_append_printf(contact.trace,"%s ",step);
}

static GError *
contact_error(ContactFailure failure)
{
  if (options.failure!=failure) return NULL;
  if ((failure==FAIL_CAPTURE_REGISTER || failure==FAIL_IMAGE || failure==FAIL_RESTORE) &&
      options.frame && options.frame!=(failure==FAIL_CAPTURE_REGISTER?contact.registers:contact.frames))
    return NULL;
  if (options.failure_kind==1)
    return g_error_new_literal(FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"synthetic negative ACK");
  return g_error_new_literal(G_IO_ERROR,
      options.failure_kind==2?G_IO_ERROR_FAILED:G_IO_ERROR_TIMED_OUT,"synthetic transport failure");
}

static void
emit_event(EventWait *wait, const guint8 reply[20], guint length, GError *error)
{
  EventWait owned=*wait;
  g_assert_nonnull(owned.callback);
  memset(wait,0,sizeof *wait);
  trace(wait==&contact.down_wait?"down-event":"up-event");
  owned.callback(test_device,(guint8 *)reply,length,owned.data,error);
  owned.destroy(owned.data);
}

static void emit_down(void) { emit_event(&contact.down_wait,contact.down_reply,options.down_length,NULL); }
static void emit_up(void) { emit_event(&contact.up_wait,contact.up_reply,options.up_length,NULL); }

GCancellable *mock_fpi_device_get_cancellable(FpDevice *dev) {(void)dev;return NULL;}

gboolean
mock_fpi_device_report_finger_status(FpDevice *dev,FpFingerStatusFlags status)
{
  g_assert_true(dev==test_device);
  if (status==(FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT)) {
    g_assert_cmpuint(contact.down_ack_mask,==,3);
    g_assert_cmpuint(contact.manual_sends,==,0);
    contact.present_reports++;
  }
  if (status==FP_FINGER_STATUS_PRESENT) {
    g_assert_cmpuint(contact.up_ack_mask,==,3);
    g_assert_cmpuint(contact.restore_acks,==,contact.frames);
    contact.removal_reports++;
  }
  if (status==FP_FINGER_STATUS_NONE) {
    g_assert_cmpuint(contact.up_ack_mask,==,3);
    g_assert_null(contact.up_wait.callback);
    contact.none_reports++;
  }
  contact.status=status;
  if ((options.cancel_present && status==(FP_FINGER_STATUS_PRESENT|FP_FINGER_STATUS_NEEDED)) ||
      (options.cancel_removal && status==FP_FINGER_STATUS_PRESENT) ||
      (options.cancel_none && status==FP_FINGER_STATUS_NONE))
    goodix_acquisition_cancel(capture);
  return TRUE;
}

void
mock_goodix_wait_fdt_down(FpDevice *dev,guint timeout,GoodixDefaultCallback cb,
                        gpointer data,GDestroyNotify destroy)
{
  g_assert_true(dev==test_device);g_assert_cmpuint(timeout,==,30000);
  g_assert_cmpuint(contact.down_arms,==,0);
  g_assert_null(contact.down_wait.callback);g_assert_null(contact.up_wait.callback);
  contact.down_wait=(EventWait){cb,data,destroy};
}

void
mock_goodix_wait_fdt_up(FpDevice *dev,guint timeout,GoodixDefaultCallback cb,
                      gpointer data,GDestroyNotify destroy)
{
  g_assert_true(dev==test_device);g_assert_cmpuint(timeout,==,30000);
  g_assert_cmpuint(contact.up_arms,==,0);
  g_assert_cmpuint(contact.restore_acks,==,contact.frames);
  g_assert_null(contact.up_wait.callback);g_assert_null(contact.down_wait.callback);
  contact.up_wait=(EventWait){cb,data,destroy};
}

static void
finish_down1(FpDevice *dev,GoodixCmdCallback cb,gpointer data)
{
  GError *error=contact_error(FAIL_DOWN1);
  if (!error) contact.down_ack_mask|=2;
  cb(dev,NULL,0,data,error);
  if (!error && !options.hold_down_event) emit_down();
}

static void
finish_up1(FpDevice *dev,GoodixCmdCallback cb,gpointer data)
{
  GError *error=contact_error(FAIL_UP1);
  if (!error) contact.up_ack_mask|=2;
  cb(dev,NULL,0,data,error);
  if (!error && !options.hold_up_event) emit_up();
}

void
mock_goodix_send_protocol(FpDevice *dev,guint8 opcode,guint8 *payload,guint16 length,
                         GDestroyNotify destroy,gboolean checksum,guint timeout,
                         gboolean reply,GoodixCmdCallback cb,gpointer data)
{
  if (reference_mode) {reference_protocol(dev,opcode,payload,length,destroy,checksum,timeout,reply,cb,data);return;}
  const guint8 fdt[8]={47,1,39,1,49,1,41,1};
  guint8 image[8];
  for(guint i=0;i<4;i++) {image[2*i]=contact.request_dacs[i];image[2*i+1]=contact.request_dacs[i]>>8;}
  guint phase=payload[26];
  g_assert_cmpuint(phase,<=,1);g_assert_null(destroy);g_assert_true(checksum);
  g_assert_cmpuint(timeout,==,GOODIX_TIMEOUT);g_assert_cmpuint(contact.done,==,0);
  if (opcode==0x32) {
    guint8 expected[40]={0x9c,1};
    trace(phase?"down1":"down0");
    g_assert_cmpuint(length,==,40);g_assert_false(reply);
    memcpy(expected+2,fdt,8);memcpy(expected+10,contact.expected_seed,16);
    expected[26]=phase;expected[28]=capture_mode?5:10;expected[29]=3;
    memcpy(expected+30,image,8);expected[38]=3;
    g_assert_cmpmem(payload,length,expected,sizeof expected);
    g_assert_nonnull(contact.down_wait.callback);
    g_assert_cmpuint(contact.manual_sends,==,0);g_assert_cmpuint(contact.frames,==,0);
    g_assert_cmpuint(contact.down_arms++,==,phase);
    if (!phase) {
      GError *error=contact_error(FAIL_DOWN0);
      if (options.cancel_command==FAIL_DOWN0) goodix_acquisition_cancel(capture);
      if (!error) contact.down_ack_mask|=1;
      cb(dev,NULL,0,data,error);
    } else {
      g_assert_cmpuint(contact.down_ack_mask,==,1);
      if (options.hold_down1) {contact.held_down1=cb;contact.held_down1_data=data;return;}
      finish_down1(dev,cb,data);
    }
  } else if (opcode==0x36) {
    guint8 expected[27]={0x0d,1};
    trace(phase?"manual1":"manual0");
    g_assert_cmpuint(length,==,27);g_assert_cmpuint(reply,==,phase);
    memcpy(expected+2,fdt,8);memcpy(expected+10,reference_seed,16);expected[26]=phase;
    g_assert_cmpmem(payload,length,expected,sizeof expected);
    g_assert_cmpuint(contact.manual_sends++,==,phase);
    g_assert_cmpuint(contact.down_ack_mask,==,3);g_assert_cmpuint(contact.frames,==,0);
    g_assert_cmpuint(contact.status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
    if (phase && options.hold_manual1) {contact.held_manual1=cb;contact.held_manual1_data=data;return;}
    cb(dev,phase?contact.manual_reply:NULL,phase?options.manual_length:0,data,
       contact_error(phase?FAIL_MANUAL1:FAIL_MANUAL0));
  } else {
    trace(phase?"up1":"up0");
    g_assert_cmpuint(opcode,==,0x34);g_assert_cmpuint(length,==,40);g_assert_false(reply);
    /* Independent golden for current DOWN raw1000+20*i, touch=ff, T=8.
     * This is the touch-adjusted OR packing, distinct from the next-DOWN ADD. */
    guint8 expected[40]={0x8e,1};
    const guint8 up_seed[16]={0xfc,0xfd,6,6,0x10,0x12,0x1a,0x1a,
                             0x24,0x26,0x2e,0x2e,0x38,0x3a,0x42,0x42};
    memcpy(expected+2,fdt,8);memcpy(expected+10,up_seed,16);expected[26]=phase;
    g_assert_cmpmem(payload,length,expected,sizeof expected);
    g_assert_nonnull(contact.up_wait.callback);g_assert_cmpuint(contact.manual_sends,==,2);
    g_assert_cmpuint(contact.restore_acks,==,contact.frames);
    g_assert_true(contact.frames==0 || contact.frames==1);
    g_assert_cmpuint(contact.none_reports,==,0);
    g_assert_cmpuint(contact.status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
    g_assert_cmpuint(contact.up_arms++,==,phase);
    if (!phase) {
      GError *error=contact_error(FAIL_UP0);
      if (options.cancel_command==FAIL_UP0) goodix_acquisition_cancel(capture);
      if (!error) contact.up_ack_mask|=1;
      cb(dev,NULL,0,data,error);
    } else {
      g_assert_cmpuint(contact.up_ack_mask,==,1);
      if (options.hold_up1) {contact.held_up1=cb;contact.held_up1_data=data;return;}
      finish_up1(dev,cb,data);
    }
  }
}

void
mock_goodix_send_write_sensor_register(FpDevice *dev,guint16 address,guint16 value,
                                      GoodixNoneCallback cb,gpointer data)
{
  if (reference_mode) {reference_register(dev,address,value,cb,data);return;}
  g_assert_cmpuint(address,==,0x022c);
  g_assert_cmpuint(contact.manual_sends,==,2);g_assert_cmpuint(contact.up_arms,==,0);
  g_assert_cmpuint(contact.status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
  if (value==0x020a) {
    if(options.check_feedback) {
      g_assert_cmpuint(adc_update_calls,==,options.updates_before_image+1);
      g_assert_cmpuint(capture->exposure.high_count,==,options.expected_high_count);
      g_assert_cmpuint(capture->exposure.steps,==,options.expected_steps);
      g_assert_cmpmem(capture->live_image_dacs,sizeof capture->live_image_dacs,
                      options.expected_next_dacs,sizeof options.expected_next_dacs);
      g_assert_cmpmem(capture->contact.pixels,sizeof capture->contact.pixels,
                      contact.expected_pixels,sizeof contact.expected_pixels);
    }
    trace("restore");
    g_assert_cmpuint(++contact.restores,==,contact.frames);
    g_assert_cmpuint(contact.restore_acks+1,==,contact.restores);
    if (options.hold_restore==contact.frames) {contact.held_restore=cb;contact.held_restore_data=data;return;}
    GError *error=contact_error(FAIL_RESTORE);
    if (!error) contact.restore_acks++;
    cb(dev,data,error);
  } else {
    trace("register");
    g_assert_cmpuint(value,==,capture_mode?0x0305:0x030a);
    g_assert_cmpuint(contact.restore_acks,==,contact.frames);
    g_assert_cmpuint(++contact.registers,==,contact.frames+1);
    /* One physical contact image: no settling burst or hidden fallback. */
    g_assert_cmpuint(contact.registers,==,1);
    cb(dev,data,contact_error(FAIL_CAPTURE_REGISTER));
  }
}

/* Authored synthetic raster and independent wire packing. Expectations never
 * call the production decoder, so byte preservation is checked end to end. */
static void
contact_image_blob(guint8 raw[GX_BLOB_SIZE])
{
  for(guint i=0;i<GX_PIXELS;i++) {
    guint16 pixel=options.use_flat_pixel ? options.flat_pixel :
        options.pattern_image ? (0x321+73*i+17*(i/80))&0xfff : 1501;
    contact.expected_pixels[2*i]=pixel;contact.expected_pixels[2*i+1]=pixel>>8;
  }
  for(guint group=0;group<GX_PIXELS/4;group++) {
    guint16 sample[4];
    for(guint k=0;k<4;k++) {
      guint wire=group*4+k;
      guint raster=(wire%64)*80+wire/64;
      sample[k]=contact.expected_pixels[2*raster]|((guint16)contact.expected_pixels[2*raster+1]<<8);
    }
    guint8 *b=raw+group*6;
    b[0]=(sample[0]>>8)|((sample[1]&15)<<4);b[1]=sample[0];b[2]=sample[2];
    b[3]=sample[1]>>4;b[4]=sample[3]>>4;b[5]=(sample[2]>>8)|((sample[3]&15)<<4);
  }
  guint32 crc=0xffffffff;
  for(guint i=0;i<GX_BLOB_SIZE-4;i++) {
    crc^=(guint32)raw[i]<<24;
    for(guint bit=0;bit<8;bit++) crc=(crc<<1)^((crc&0x80000000u)?0x04c11db7u:0);
  }
  raw[7680]=crc>>8;raw[7681]=crc;raw[7682]=crc>>24;raw[7683]=crc>>16;
}

void
mock_goodix_tls_read_image(FpDevice *dev,guint8 *payload,guint16 length,
                          GoodixImageCallback cb,gpointer data)
{
  if (reference_mode) {reference_image(dev,payload,length,cb,data);return;}
  guint8 expected[10]={0x45,3};
  for(guint i=0;i<4;i++) {expected[2+2*i]=contact.request_dacs[i];expected[3+2*i]=contact.request_dacs[i]>>8;}
  guint8 raw[GX_BLOB_SIZE];
  trace("image");
  if(options.check_feedback) {
    g_assert_cmpuint(adc_update_calls,==,options.updates_before_image);
    g_assert_cmpmem(capture->live_image_dacs,sizeof capture->live_image_dacs,
                    contact.request_dacs,sizeof contact.request_dacs);
  }
  g_assert_cmpmem(payload,length,expected,sizeof expected);
  g_assert_cmpuint(++contact.frames,==,contact.registers);
  g_assert_cmpuint(contact.restores+1,==,contact.frames);
  g_assert_cmpuint(contact.status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
  g_assert_cmpuint(contact.frames,==,1);
  contact_image_blob(raw);
  if (options.corrupt_frame==contact.frames) raw[7680]^=1;
  if(options.hold_image) {
    contact.held_image=cb;contact.held_image_data=data;
    memcpy(contact.held_image_blob,raw,sizeof raw);return;
  }
  cb(dev,raw,sizeof raw,data,contact_error(FAIL_IMAGE));
}

static void
acquired(FpDevice *dev,const GxImage *image,GoodixAcquiredOutcome outcome,
         gpointer data,GError *error)
{
  (void)data;g_assert_true(dev==test_device);g_assert_cmpuint(++contact.done,==,1);
  g_assert_null(result_error);result_error=error;contact.outcome=outcome;
  if (error || outcome==GOODIX_ACQUIRED_REFRESH_REQUIRED) g_assert_null(image);
  else {
    g_assert_nonnull(image);g_assert_true(image->valid);
    g_assert_cmpmem(image->pixels,sizeof image->pixels,contact.expected_pixels,sizeof contact.expected_pixels);
    contact.image=*image;
  }
  g_assert_null(capture->ssm);
  if (options.free_on_complete) {goodix_acquisition_free(capture);capture=NULL;}
}
static struct {
  guint attempts, commands, images, completions;
  gint reject_state, fail_state;
  gboolean persistent, cancel_gate, hold_final, stable_drift;
  guint failure_kind;
  FpiSsm *ssm;
  guint64 generation;
  GoodixCmdCallback held_callback;
  gpointer held_data;
  guint8 held_reply[20];
  GxImage accepted;
} reference;

static const guint16 reference_first[8] = {
  0x0123, 0x0285, 0x0457, 0x0809, 0x107b, 0x20cd, 0x40ef, 0xfffd
};
/* Independent expected LE u16((FIRST raw u16 >> 1) * 257), including overflow. */
static const guint8 reference_seed[16] = {
  0x91,0x91, 0x42,0x43, 0x2b,0x2d, 0x04,0x08,
  0x3d,0x45, 0x66,0x76, 0x77,0x97, 0xfe,0x7d
};

static guint
reference_note(gpointer data)
{
  FpiSsm *ssm=data;
  guint state=fpi_ssm_get_cur_state(ssm);
  g_assert_true(fpi_ssm_get_data(ssm)==capture);
  g_assert_true(capture->device==test_device);
  g_assert_false(goodix_acquisition_commit_reference(capture));
  g_assert_cmpuint(reference.completions,==,0);
  if (state==PAIRED_REFERENCE_F1_PHASE0) {
    const guint8 empty_fdt[3][16]={{0}};
    g_assert_cmpuint(capture->paired_retry_count,==,reference.attempts);
    g_assert_false(capture->paired_f1_valid);
    g_assert_cmpmem(capture->paired_fdt,sizeof capture->paired_fdt,empty_fdt,sizeof empty_fdt);
    g_assert_false(capture->paired_images[0].valid);
    g_assert_false(capture->paired_images[1].valid);
    if (!reference.attempts) {
      reference.ssm=ssm;
      reference.generation=capture->generation;
    } else {
      g_assert_true(reference.ssm==ssm);
      g_assert_cmpuint(capture->generation,==,reference.generation);
    }
    g_assert_cmpuint(++reference.attempts,<=,2);
  }
  g_assert_true(reference.ssm==ssm);
  reference.commands++;
  timing_clock_us+=100;
  return state;
}

static GError *
reference_error(guint state)
{
  if ((gint)state!=reference.fail_state) return NULL;
  g_test_expect_message("libfprint-goodix521d",G_LOG_LEVEL_WARNING,"paired-reference-error *");
  if (reference.failure_kind==1)
    return g_error_new_literal(FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"synthetic protocol error");
  if (reference.failure_kind==2)
    return g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"synthetic cancellation");
  return g_error_new_literal(G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"synthetic transport timeout");
}

static void
reference_expect_rejection(guint state)
{
  if ((gint)state!=reference.reject_state ||
      (reference.attempts!=1 && !reference.persistent)) return;
  if (state!=PAIRED_REFERENCE_IMAGE2_RESTORE)
    g_test_expect_message("libfprint-goodix521d",G_LOG_LEVEL_WARNING,"paired-reference-fdt *");
  g_test_expect_message("libfprint-goodix521d",G_LOG_LEVEL_WARNING,"paired-reference-error *");
  if (reference.attempts==1 && !reference.cancel_gate)
    g_test_expect_message("libfprint-goodix521d",G_LOG_LEVEL_WARNING,"paired-reference-retry *");
}

static void
reference_protocol(FpDevice *dev, guint8 opcode, guint8 *payload, guint16 length,
                   GDestroyNotify destroy, gboolean checksum, guint timeout,
                   gboolean reply, GoodixCmdCallback cb, gpointer data)
{
  guint state=reference_note(data);
  guint8 response[20]={2,0,255,0};
  const guint8 zero[16]={0};
  g_assert_cmpuint(opcode,==,0x36);
  g_assert_cmpuint(length,==,27);
  g_assert_null(destroy);
  g_assert_true(checksum);
  g_assert_cmpuint(timeout,==,GOODIX_TIMEOUT);
  g_assert_cmpuint(payload[26],==,reply);
  g_assert_cmpmem(payload+10,16,reference.attempts==1?zero:reference_seed,16);
  for (guint i=0;i<8;i++) {
    guint16 word=reference_first[i];
    /* The second attempt has fresh F1 data, but must keep the retry seed that
     * came from the FIRST attempt's F1 for all three new FDT commands. */
    if (reference.attempts==2) word-=2;
    if (reference.stable_drift) {
      if (state==PAIRED_REFERENCE_F2_PHASE1) word-=2;
      if (state==PAIRED_REFERENCE_F3_PHASE1) word-=4;
    }
    if ((gint)state==reference.reject_state &&
        (reference.attempts==1 || reference.persistent) && i==0) word+=100;
    response[4+2*i]=word&255;
    response[5+2*i]=word>>8;
  }
  if ((gint)state==reference.reject_state && reference.cancel_gate)
    goodix_acquisition_cancel(capture);
  if (state==PAIRED_REFERENCE_F3_PHASE1 && reference.hold_final &&
      reference.attempts==2) {
    reference.held_callback=cb;
    reference.held_data=data;
    memcpy(reference.held_reply,response,sizeof response);
    return;
  }
  reference_expect_rejection(state);
  cb(dev,reply?response:NULL,reply?20:0,data,reference_error(state));
  g_test_assert_expected_messages();
}

static void
reference_register(FpDevice *dev, guint16 address, guint16 value,
                   GoodixNoneCallback cb, gpointer data)
{
  guint state=reference_note(data);
  g_assert_cmpuint(address,==,0x022c);
  g_assert_cmpuint(value,==,(state==PAIRED_REFERENCE_IMAGE1_RESTORE ||
                           state==PAIRED_REFERENCE_IMAGE2_RESTORE)?0x020a:0x030a);
  if ((gint)state==reference.reject_state && reference.cancel_gate)
    goodix_acquisition_cancel(capture);
  reference_expect_rejection(state);
  cb(dev,data,reference_error(state));
  g_test_assert_expected_messages();
}

static void
reference_blob(guint16 pixel, guint8 raw[GX_BLOB_SIZE])
{
  for (guint i=0;i<7680;i+=6) {
    raw[i]=(pixel>>8)|((pixel&15)<<4);raw[i+1]=pixel;raw[i+2]=pixel;
    raw[i+3]=pixel>>4;raw[i+4]=pixel>>4;raw[i+5]=raw[i];
  }
  guint32 crc=0xffffffff;
  for (guint i=0;i<7680;i++) {
    crc^=(guint32)raw[i]<<24;
    for (guint bit=0;bit<8;bit++) crc=(crc<<1)^((crc&0x80000000u)?0x04c11db7u:0);
  }
  raw[7680]=crc>>8;raw[7681]=crc;raw[7682]=crc>>24;raw[7683]=crc>>16;
}

static void
reference_image(FpDevice *dev, guint8 *payload, guint16 length,
                GoodixImageCallback cb, gpointer data)
{
  guint state=reference_note(data);
  guint8 raw[GX_BLOB_SIZE];
  guint16 pixel=adc_fixture ? (state==PAIRED_REFERENCE_IMAGE1?1000:1050) : 1000+reference.attempts*100;
  g_assert_cmpuint(length,==,10);
  g_assert_cmpuint(payload[0],==,state==PAIRED_REFERENCE_IMAGE1?1:0x81);
  if (state==PAIRED_REFERENCE_IMAGE2 &&
      reference.reject_state==PAIRED_REFERENCE_IMAGE2_RESTORE &&
      (reference.attempts==1 || reference.persistent)) pixel+=200;
  reference_blob(pixel,raw);
  reference.images++;
  cb(dev,raw,sizeof raw,data,reference_error(state));
  g_test_assert_expected_messages();
}

static void
reference_acquired(FpDevice *dev, const GxImage *image, GoodixAcquiredOutcome outcome, gpointer data, GError *error)
{
  (void)data;
  g_assert_cmpuint(outcome,==,GOODIX_ACQUIRED_IMAGE);
  g_assert_true(dev==test_device);
  g_assert_cmpuint(++reference.completions,==,1);
  g_assert_null(result_error);
  result_error=error;
  if (error) g_assert_null(image);
  else {
    g_assert_nonnull(image);
    g_assert_true(image->valid);
    reference.accepted=*image;
  }
  g_assert_null(capture->ssm);
}

static void
reference_setup(void)
{
  reset_timing();
  guint8 otp[64]={0};
  otp[23]=0x6e|(capture_mode?0x80:0);otp[26]=37;otp[33]=47;otp[40]=7;
  otp[41]=39;otp[42]=49;otp[43]=41;otp[44]=97;otp[45]=101;otp[46]=103;
  otp[27]=50; /* Synthetic valid factory integration=100. */
  if(adc_fixture) {otp[26]=161;otp[44]=adc_minimum_fixture?150:164;otp[45]=adc_minimum_fixture?165:170;otp[46]=adc_minimum_fixture?170:175;}
  memset(&reference,0,sizeof reference);
  reference.reject_state=reference.fail_state=-1;
  reference_mode=TRUE;
  g_clear_error(&result_error);
  test_device=g_object_new(FPI_TYPE_DEVICE_FAKE,NULL);
  capture=goodix_acquisition_new(test_device,otp);
  goodix_acquisition_set_timing_context(capture,87654321,0);
}

static void
reference_teardown(void)
{
  g_assert_null(capture->ssm);
  goodix_acquisition_free(capture);capture=NULL;
  g_clear_object(&test_device);
  g_clear_error(&result_error);
  reference_mode=FALSE;
}

static void
reference_retry_success(void)
{
  const gint gates[]={PAIRED_REFERENCE_F2_PHASE1,PAIRED_REFERENCE_IMAGE2_RESTORE,
                      PAIRED_REFERENCE_F3_PHASE1};
  for (guint i=0;i<G_N_ELEMENTS(gates);i++) {
    reference_setup();reference.reject_state=gates[i];reference.hold_final=TRUE;
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    g_assert_cmpuint(reference.attempts,==,2);
    g_assert_cmpuint(reference.completions,==,0);
    g_assert_false(reference.accepted.valid);
    g_assert_nonnull(reference.held_callback);
    reference.held_callback(test_device,reference.held_reply,20,reference.held_data,NULL);
    g_assert_cmpuint(reference.completions,==,1);
    g_assert_no_error(result_error);
    g_assert_cmpuint(reference.accepted.pixels[0]|((guint16)reference.accepted.pixels[1]<<8),==,1200);
    reference_teardown();
  }
}

static void
reference_persistent_failure(void)
{
  const gint gates[]={PAIRED_REFERENCE_F2_PHASE1,PAIRED_REFERENCE_IMAGE2_RESTORE,
                      PAIRED_REFERENCE_F3_PHASE1};
  for (guint i=0;i<G_N_ELEMENTS(gates);i++) {
    reference_setup();reference.reject_state=gates[i];reference.persistent=TRUE;
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    g_assert_cmpuint(reference.attempts,==,2);
    g_assert_cmpuint(reference.commands,==,2*(gates[i]+1));
    g_assert_cmpuint(reference.completions,==,1);
    g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    g_assert_false(reference.accepted.valid);
    reference_teardown();
  }
}

static void
reference_cancel_prevents_retry(void)
{
  const gint gates[]={PAIRED_REFERENCE_F2_PHASE1,PAIRED_REFERENCE_IMAGE2_RESTORE,
                      PAIRED_REFERENCE_F3_PHASE1};
  for (guint i=0;i<G_N_ELEMENTS(gates);i++) {
    reference_setup();reference.reject_state=gates[i];reference.cancel_gate=TRUE;
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    g_assert_cmpuint(reference.attempts,==,1);
    g_assert_cmpuint(reference.commands,==,gates[i]+1);
    g_assert_cmpuint(reference.completions,==,1);
    g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    g_assert_false(reference.accepted.valid);
    reference_teardown();
  }
}

static void
reference_command_errors_do_not_retry(void)
{
  for (guint kind=0;kind<3;kind++) for (gint state=0;state<PAIRED_REFERENCE_NUM_STATES;state++) {
    reference_setup();reference.fail_state=state;reference.failure_kind=kind;
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    g_assert_cmpuint(reference.attempts,==,1);
    g_assert_cmpuint(reference.commands,==,state+1);
    g_assert_cmpuint(reference.completions,==,1);
    if (kind==1) g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    else g_assert_error(result_error,G_IO_ERROR,(kind==2?G_IO_ERROR_CANCELLED:G_IO_ERROR_TIMED_OUT));
    g_assert_false(reference.accepted.valid);
    reference_teardown();
  }
}

static void
reference_new_start_resets_retry(void)
{
  reference_setup();reference.reject_state=PAIRED_REFERENCE_F2_PHASE1;
  GoodixAcquisition *same=capture;
  for (guint i=0;i<2;i++) {
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    g_assert_true(capture==same);
    g_assert_cmpuint(reference.attempts,==,2);
    g_assert_cmpuint(reference.completions,==,1);
    g_assert_no_error(result_error);
    g_assert_true(reference.accepted.valid);
    /* The next start must zero the wire base AND restore the one retry budget. */
    memset(&reference,0,sizeof reference);
    reference.reject_state=PAIRED_REFERENCE_F2_PHASE1;
    reference.fail_state=-1;
  }
  reference_teardown();
}

static void
contact_reset(const guint8 seed[16])
{
  g_assert_null(contact.down_wait.callback);g_assert_null(contact.up_wait.callback);
  if (contact.trace) g_string_free(contact.trace,TRUE);
  memset(&contact,0,sizeof contact);memset(&options,0,sizeof options);
  contact.trace=g_string_new(NULL);
  contact.status=FP_FINGER_STATUS_NONE;
  const guint16 factory[4]={37,97,101,103};
  const guint16 adc_factory[4]={161,164,170,175};
  const guint16 adc_minimum[4]={161,150,165,170};
  memcpy(contact.request_dacs,adc_fixture?(adc_minimum_fixture?adc_minimum:adc_factory):factory,sizeof contact.request_dacs);
  options.down_length=options.manual_length=options.up_length=20;
  contact.down_reply[0]=2;contact.down_reply[2]=0xff;
  contact.manual_reply[0]=2;contact.manual_reply[2]=0xff;
  contact.up_reply[1]=2;contact.up_reply[2]=0xff;
  memcpy(contact.expected_seed,seed,16);
  for (guint i=0;i<8;i++) {
    guint16 raw=1000+i*20;
    contact.down_reply[4+2*i]=raw;contact.down_reply[5+2*i]=raw>>8;
    contact.manual_reply[4+2*i]=raw;contact.manual_reply[5+2*i]=raw>>8;
  }
  /* Factory timing is 8. One lane differs by 9 after LSB truncation. */
  contact.manual_reply[4]+=18;
  const guint8 up_raw[16]={0,0,1,0,2,0,0xfe,1,0,2,0xff,0xff,0x23,1,5,4};
  memcpy(contact.up_reply+4,up_raw,16);
  g_clear_error(&result_error);reset_timing();
  goodix_acquisition_set_timing_context(capture,1234567,4);
}

static void
contact_setup(void)
{
  reference_setup();reference.stable_drift=TRUE;
  goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
  g_assert_cmpuint(reference.completions,==,1);g_assert_no_error(result_error);
  g_assert_true(goodix_acquisition_commit_reference(capture));
  g_assert_false(goodix_acquisition_commit_reference(capture));
  reference_mode=FALSE;contact_reset(reference_seed);
}

static void
contact_start(void)
{
  goodix_acquisition_start(capture,FALSE,acquired,NULL);
}

static void
contact_teardown(void)
{
  /* A cancelled/failed arm may still own an event callback. Deliver it late,
   * exactly once, to prove it cannot revive the failed operation. */
  if (contact.down_wait.callback) emit_down();
  if (contact.up_wait.callback) emit_up();
  g_assert_cmpuint(contact.done,==,1);
  g_assert_null(contact.held_down1);g_assert_null(contact.held_up1);
  g_assert_null(contact.held_manual1);g_assert_null(contact.held_restore);g_assert_null(contact.held_image);
  if (capture) {g_assert_null(capture->ssm);goodix_acquisition_free(capture);capture=NULL;}
  g_clear_object(&test_device);g_clear_error(&result_error);
  if(contact.trace) g_string_free(contact.trace,TRUE);
  contact.trace=NULL;reference_mode=FALSE;adc_fixture=adc_minimum_fixture=FALSE;
}

static void
assert_image_success(void)
{
  g_assert_cmpuint(contact.done,==,1);g_assert_no_error(result_error);
  g_assert_cmpuint(contact.outcome,==,GOODIX_ACQUIRED_IMAGE);
  g_assert_true(contact.image.valid);
  g_assert_cmpmem(contact.image.pixels,sizeof contact.image.pixels,
                  contact.expected_pixels,sizeof contact.expected_pixels);
  g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restores,==,1);
  g_assert_cmpuint(contact.restore_acks,==,1);g_assert_cmpuint(contact.manual_sends,==,2);
  g_assert_cmpuint(contact.down_arms,==,2);g_assert_cmpuint(contact.up_arms,==,2);
  g_assert_cmpuint(contact.present_reports,==,1);g_assert_cmpuint(contact.removal_reports,==,1);
  g_assert_cmpuint(contact.none_reports,==,1);
  g_assert_cmpuint(contact.status,==,FP_FINGER_STATUS_NONE);
}

static void
contact_exact_sequence(void)
{
  for (guint mode=0;mode<2;mode++) {
    capture_mode=mode;contact_setup();contact_start();assert_image_success();
    GString *expected=g_string_new("down0 down1 down-event manual0 manual1 ");
    g_string_append(expected,"register image restore ");
    g_string_append(expected,"up0 up1 up-event ");
    g_assert_cmpstr(contact.trace->str,==,expected->str);g_string_free(expected,TRUE);
    contact_teardown();
  }
  capture_mode=FALSE;
}

static void
contact_first_image_bytes_delivered_once(void)
{
  contact_setup();options.pattern_image=TRUE;options.hold_up_event=TRUE;
  contact_start();
  g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.registers,==,1);
  g_assert_cmpuint(contact.restores,==,1);g_assert_cmpuint(contact.restore_acks,==,1);
  g_assert_cmpuint(contact.done,==,0);g_assert_false(contact.image.valid);
  g_assert_cmpuint(contact.up_ack_mask,==,3);g_assert_cmpuint(contact.none_reports,==,0);
  emit_up();assert_image_success();
  const guint8 zero_prefix[5]={0};
  g_assert_cmpmem(contact.image.prefix,5,zero_prefix,5);
  g_assert_cmpuint(contact.image.marker,==,0);g_assert_cmpuint(contact.image.generation,!=,0);
  g_assert_cmpuint(contact.done,==,1);
  g_assert_cmpstr(contact.trace->str,==,
      "down0 down1 down-event manual0 manual1 register image restore up0 up1 up-event ");
  contact_teardown();
}

static void
contact_requires_committed_context(void)
{
  for (guint kind=0;kind<3;kind++) {
    reference_setup();
    if (kind) {
      goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
      g_assert_no_error(result_error);
      if (kind==2) {
        g_assert_true(goodix_acquisition_commit_reference(capture));
        goodix_acquisition_invalidate_context(capture);
      }
    }
    reference_mode=FALSE;contact_reset(reference_seed);contact_start();
    g_assert_cmpuint(contact.done,==,1);g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    g_assert_cmpstr(contact.trace->str,==,"");g_assert_false(contact.image.valid);
    g_assert_false(goodix_acquisition_commit_reference(capture));
    contact_teardown();
  }
}

static void
assert_no_successor_contact(void)
{
  if (contact.down_wait.callback) emit_down();
  if (contact.up_wait.callback) emit_up();
  contact_reset(reference_seed);contact_start();
  g_assert_cmpuint(contact.done,==,1);g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
  g_assert_cmpstr(contact.trace->str,==,"");g_assert_false(contact.image.valid);
}

static void
contact_refresh_waits_for_up(void)
{
  contact_setup();
  /* Inclusive threshold, in every lane, must take the refresh branch. */
  for(guint i=0;i<8;i++) {
    guint16 raw=1000+i*20+16;
    contact.manual_reply[4+2*i]=raw;contact.manual_reply[5+2*i]=raw>>8;
  }
  options.hold_up1=options.hold_up_event=TRUE;
  contact_start();
  g_assert_cmpuint(contact.frames,==,0);g_assert_cmpuint(contact.restores,==,0);
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.up_ack_mask,==,1);
  g_assert_cmpuint(contact.removal_reports,==,0);g_assert_cmpuint(contact.none_reports,==,0);
  GoodixCmdCallback cb=contact.held_up1;gpointer data=contact.held_up1_data;
  contact.held_up1=NULL;finish_up1(test_device,cb,data);
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.removal_reports,==,1);
  g_assert_cmpuint(contact.none_reports,==,0);
  emit_up();
  g_assert_cmpuint(contact.done,==,1);g_assert_no_error(result_error);
  g_assert_cmpuint(contact.outcome,==,GOODIX_ACQUIRED_REFRESH_REQUIRED);
  g_assert_false(contact.image.valid);g_assert_cmpuint(contact.none_reports,==,1);
  g_assert_cmpstr(contact.trace->str,==,"down0 down1 down-event manual0 manual1 up0 up1 up-event ");
  /* All validated seeds become ineligible, including a pending reference. */
  const guint8 zero_seed[16]={0};
  g_assert_false(capture->context_valid);g_assert_false(capture->reference_seed_valid);
  g_assert_false(capture->reference_pending);
  g_assert_cmpmem(capture->reference_seed,16,zero_seed,16);
  g_assert_cmpmem(capture->down_seed,16,zero_seed,16);
  g_assert_cmpmem(capture->pending_reference_seed,16,zero_seed,16);
  g_assert_false(goodix_acquisition_commit_reference(capture));
  assert_no_successor_contact();contact_teardown();
}

static void
contact_comparator_all_lanes_and_lsb(void)
{
  for(guint lane=0;lane<8;lane++) {
    contact_setup();memcpy(contact.manual_reply+4,contact.down_reply+4,16);
    guint16 raw=1000+lane*20+18;
    contact.manual_reply[4+2*lane]=raw;contact.manual_reply[5+2*lane]=raw>>8;
    contact_start();assert_image_success();contact_teardown();
  }
  contact_setup();memcpy(contact.manual_reply+4,contact.down_reply+4,16);
  for(guint lane=0;lane<8;lane++) contact.manual_reply[4+2*lane]|=1;
  contact_start();g_assert_no_error(result_error);
  g_assert_cmpuint(contact.outcome,==,GOODIX_ACQUIRED_REFRESH_REQUIRED);
  g_assert_cmpuint(contact.frames,==,0);g_assert_cmpuint(contact.done,==,1);
  contact_teardown();
}

static void
contact_dynamic_down_seed(void)
{
  const guint8 up_seed[16]={0,0,0,0,1,1,0xff,0xff,0,1,0xff,0x7e,0x91,0x91,2,4};
  contact_setup();GoodixAcquisition *same=capture;
  contact_start();assert_image_success();
  contact_reset(up_seed);goodix_acquisition_set_timing_context(capture,1234567,5);
  contact_start();assert_image_success();g_assert_true(capture==same);
  /* Every manual payload is independently checked against the FIRST FDT seed,
   * while every DOWN payload is checked against the last qualified UP seed. */
  contact_teardown();
}

static void
contact_waits_for_boundaries(void)
{
  contact_setup();options.hold_down1=options.hold_down_event=TRUE;
  options.hold_manual1=options.hold_up1=options.hold_up_event=TRUE;options.hold_restore=1;
  contact_start();
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.manual_sends,==,0);
  g_assert_cmpuint(contact.down_ack_mask,==,1);g_assert_cmpuint(contact.present_reports,==,0);
  GoodixCmdCallback cb=contact.held_down1;gpointer data=contact.held_down1_data;
  contact.held_down1=NULL;finish_down1(test_device,cb,data);
  g_assert_cmpuint(contact.manual_sends,==,0);g_assert_cmpuint(contact.present_reports,==,0);
  emit_down();
  g_assert_cmpuint(contact.frames,==,0);g_assert_cmpuint(contact.done,==,0);
  cb=contact.held_manual1;data=contact.held_manual1_data;contact.held_manual1=NULL;
  cb(test_device,contact.manual_reply,20,data,NULL);
  g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restores,==,1);
  g_assert_cmpuint(contact.restore_acks,==,0);g_assert_cmpuint(contact.up_arms,==,0);
  g_assert_cmpuint(contact.registers,==,1);g_assert_cmpuint(contact.done,==,0);
  GoodixNoneCallback restore=contact.held_restore;data=contact.held_restore_data;
  contact.held_restore=NULL;contact.restore_acks++;restore(test_device,data,NULL);
  g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restore_acks,==,1);
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.up_ack_mask,==,1);
  g_assert_cmpuint(contact.removal_reports,==,0);g_assert_cmpuint(contact.none_reports,==,0);
  cb=contact.held_up1;data=contact.held_up1_data;contact.held_up1=NULL;
  finish_up1(test_device,cb,data);
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.none_reports,==,0);
  emit_up();assert_image_success();contact_teardown();
}

static void
contact_bad_images_never_restore_or_publish(void)
{
  for(guint pattern=0;pattern<2;pattern++) {
    contact_setup();options.pattern_image=pattern;options.corrupt_frame=1;contact_start();
    g_assert_cmpuint(contact.done,==,1);g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restores,==,0);
    g_assert_cmpuint(contact.up_arms,==,0);g_assert_cmpuint(contact.none_reports,==,0);
    g_assert_cmpuint(contact.removal_reports,==,0);g_assert_false(contact.image.valid);
    assert_no_successor_contact();contact_teardown();
  }
}

static void
contact_command_errors_stop(void)
{
  for(guint failure=FAIL_DOWN0;failure<=FAIL_UP1;failure++) for(guint kind=0;kind<3;kind++) {
    contact_setup();options.failure=failure;options.failure_kind=kind;options.frame=1;
    contact_start();g_assert_cmpuint(contact.done,==,1);g_assert_false(contact.image.valid);
    if(kind==1) g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    else g_assert_error(result_error,G_IO_ERROR,(kind==2?G_IO_ERROR_FAILED:G_IO_ERROR_TIMED_OUT));
    g_assert_cmpuint(contact.none_reports,==,0);g_assert_cmpuint(contact.removal_reports,==,0);
    if(failure==FAIL_DOWN0) g_assert_cmpuint(contact.down_arms,==,1);
    if(failure<=FAIL_DOWN1) g_assert_cmpuint(contact.manual_sends,==,0);
    if(failure==FAIL_MANUAL0) g_assert_cmpuint(contact.manual_sends,==,1);
    if(failure<=FAIL_MANUAL1) g_assert_cmpuint(contact.frames,==,0);
    if(failure==FAIL_CAPTURE_REGISTER) g_assert_cmpuint(contact.frames,==,0);
    if(failure==FAIL_IMAGE) {g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restores,==,0);}
    if(failure==FAIL_RESTORE) {g_assert_cmpuint(contact.frames,==,1);g_assert_cmpuint(contact.restores,==,1);}
    if(failure<FAIL_UP0) g_assert_cmpuint(contact.up_arms,==,0);
    if(failure==FAIL_UP0) g_assert_cmpuint(contact.up_arms,==,1);
    assert_no_successor_contact();contact_teardown();
  }
}

static void
contact_rejects_malformed_replies(void)
{
  for(guint which=0;which<3;which++) for(guint length=19;length<=21;length+=2) {
    contact_setup();
    if(which==0)options.down_length=length;
    if(which==1)options.manual_length=length;
    if(which==2)options.up_length=length;
    contact_start();g_assert_cmpuint(contact.done,==,1);
    g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    g_assert_false(contact.image.valid);g_assert_cmpuint(contact.none_reports,==,0);
    if(which<2)g_assert_cmpuint(contact.frames,==,0);
    assert_no_successor_contact();contact_teardown();
  }
  const guint irqs[]={0x80,0x82,0x202,0};
  for(guint which=0;which<2;which++) for(guint i=0;i<G_N_ELEMENTS(irqs);i++) {
    contact_setup();guint8 *reply=which?contact.up_reply:contact.down_reply;
    reply[0]=irqs[i];reply[1]=irqs[i]>>8;
    contact_start();g_assert_cmpuint(contact.done,==,1);
    g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    g_assert_cmpuint(contact.none_reports,==,0);g_assert_false(contact.image.valid);
    if(!which)g_assert_cmpuint(contact.frames,==,0);
    assert_no_successor_contact();contact_teardown();
  }
}

static void
contact_cancellation_stops_successors(void)
{
  for(guint point=0;point<9;point++) {
    contact_setup();
    if(point==0)options.cancel_present=TRUE;
    if(point==1)options.cancel_removal=TRUE;
    if(point==2)options.cancel_none=TRUE;
    if(point==3)options.hold_restore=1;
    if(point==4)options.hold_manual1=TRUE;
    if(point==5)options.hold_down1=TRUE;
    if(point==6)options.hold_up1=TRUE;
    if(point==7)options.cancel_command=FAIL_DOWN0;
    if(point==8)options.cancel_command=FAIL_UP0;
    contact_start();
    if(point>=3 && point<=6) {
      g_assert_cmpuint(contact.done,==,0);goodix_acquisition_cancel(capture);
      if(point==3) {
        GoodixNoneCallback cb=contact.held_restore;gpointer data=contact.held_restore_data;
        contact.held_restore=NULL;contact.restore_acks++;cb(test_device,data,NULL);
      } else if(point==4) {
        GoodixCmdCallback cb=contact.held_manual1;gpointer data=contact.held_manual1_data;
        contact.held_manual1=NULL;cb(test_device,contact.manual_reply,20,data,NULL);
      } else {
        GoodixCmdCallback cb=point==5?contact.held_down1:contact.held_up1;
        gpointer data=point==5?contact.held_down1_data:contact.held_up1_data;
        if(point==5){contact.held_down1=NULL;finish_down1(test_device,cb,data);}
        else {contact.held_up1=NULL;finish_up1(test_device,cb,data);}
      }
    }
    g_assert_cmpuint(contact.done,==,1);g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    g_assert_false(contact.image.valid);
    if(point==0 || point==4 || point==5 || point==7)g_assert_cmpuint(contact.frames,==,0);
    if(point==7)g_assert_cmpuint(contact.down_arms,==,1);
    if(point==8)g_assert_cmpuint(contact.up_arms,==,1);
    if(point==3)g_assert_cmpuint(contact.frames,==,1);
    if(point==0 || point==3 || point==4 || point==5 || point==7)g_assert_cmpuint(contact.up_arms,==,0);
    assert_no_successor_contact();contact_teardown();
  }
}

static void
contact_stale_up_owner_cannot_advance_new_contact(void)
{
  contact_setup();options.failure=FAIL_UP0;contact_start();
  g_assert_cmpuint(contact.done,==,1);g_assert_nonnull(result_error);
  EventWait old=contact.up_wait;memset(&contact.up_wait,0,sizeof contact.up_wait);
  /* A fresh qualified reference starts a new owner generation on the same object. */
  memset(&reference,0,sizeof reference);reference.fail_state=reference.reject_state=-1;
  reference_mode=TRUE;g_clear_error(&result_error);
  goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
  g_assert_no_error(result_error);g_assert_true(goodix_acquisition_commit_reference(capture));
  reference_mode=FALSE;contact_reset(reference_seed);options.hold_down_event=TRUE;
  contact_start();g_assert_cmpuint(contact.done,==,0);
  old.callback(test_device,contact.up_reply,20,old.data,NULL);old.destroy(old.data);
  g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.frames,==,0);
  g_assert_cmpuint(contact.none_reports,==,0);g_assert_nonnull(contact.down_wait.callback);
  emit_down();assert_image_success();contact_teardown();
}

static void
contact_callback_can_release_object(void)
{
  contact_setup();options.free_on_complete=TRUE;contact_start();
  assert_image_success();g_assert_null(capture);contact_teardown();
}

static void
timing_contact_success(void)
{
  contact_setup();contact_start();assert_image_success();
  const char *phases[]={"contact-down","contact-preburst","contact-burst","contact-restore","contact-up","acquisition-total"};
  const gint64 elapsed[]={300,200,200,100,300,1100};
  g_assert_cmpuint(timing_records->len,==,G_N_ELEMENTS(phases));
  for(guint i=0;i<G_N_ELEMENTS(phases);i++) {
    const AcquisitionTiming *record=timing_at(i,phases[i],"ok");
    g_assert_cmpuint(record->reference,==,0);g_assert_cmpuint(record->action_id,==,1234567);
    g_assert_cmpuint(record->contact,==,4);g_assert_cmpint(record->elapsed_us,==,elapsed[i]);
  }
  contact_teardown();
}

static void
timing_contact_failure_and_cancel(void)
{
  contact_setup();options.failure=FAIL_RESTORE;options.frame=1;contact_start();
  g_assert_cmpuint(timing_records->len,==,5);
  timing_at(0,"contact-down","ok");timing_at(1,"contact-preburst","ok");
  timing_at(2,"contact-burst","ok");timing_at(3,"contact-restore","error");
  g_assert_cmpint(timing_at(4,"acquisition-total","error")->elapsed_us,==,800);
  contact_teardown();
  contact_setup();options.cancel_present=TRUE;contact_start();
  g_assert_cmpuint(timing_records->len,==,3);timing_at(0,"contact-down","ok");
  timing_at(1,"contact-preburst","cancelled");
  g_assert_cmpint(timing_at(2,"acquisition-total","cancelled")->elapsed_us,==,300);
  contact_teardown();
}
static void timing_reference_success(void)
{
 reference_setup();
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(reference.commands,==,12);g_assert_cmpuint(reference.completions,==,1);
 g_assert_cmpuint(timing_records->len,==,2);
 const AcquisitionTiming *phase=timing_at(0,"reference-acquire","ok");
 g_assert_cmpuint(phase->reference,==,1);g_assert_cmpuint(phase->contact,==,0);
 g_assert_cmpuint(phase->action_id,==,87654321);g_assert_cmpint(phase->elapsed_us,==,1200);
 g_assert_cmpint(timing_at(1,"acquisition-total","ok")->elapsed_us,==,1200);
 reference_teardown();
}
static void timing_reference_retry_and_error(void)
{
 reference_setup();reference.reject_state=PAIRED_REFERENCE_F2_PHASE1;
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(reference.attempts,==,2);g_assert_cmpuint(reference.commands,==,19);
 g_assert_cmpuint(timing_records->len,==,2);
 g_assert_cmpint(timing_at(0,"reference-acquire","ok")->elapsed_us,==,1900);
 g_assert_cmpint(timing_at(1,"acquisition-total","ok")->elapsed_us,==,1900);
 reference_teardown();
 reference_setup();reference.fail_state=PAIRED_REFERENCE_F2_PHASE1;
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(reference.commands,==,7);g_assert_cmpuint(timing_records->len,==,2);
 g_assert_cmpint(timing_at(0,"reference-acquire","error")->elapsed_us,==,700);
 g_assert_cmpint(timing_at(1,"acquisition-total","error")->elapsed_us,==,700);
 reference_teardown();
 reference_setup();reference.fail_state=PAIRED_REFERENCE_IMAGE1;reference.failure_kind=2;
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(reference.commands,==,4);g_assert_cmpuint(timing_records->len,==,2);
 g_assert_cmpint(timing_at(0,"reference-acquire","cancelled")->elapsed_us,==,400);
 timing_at(1,"acquisition-total","cancelled");
 reference_teardown();
}
static void timing_context_reuse_and_clamp(void)
{
 reference_setup();
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(timing_records->len,==,2);
 memset(&reference,0,sizeof reference);reference.fail_state=reference.reject_state=-1;
 goodix_acquisition_set_timing_context(capture,87654322,9);
 timing_clock_us=5000;g_array_set_size(timing_records,0);
 goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
 g_assert_cmpuint(timing_records->len,==,2);
 const AcquisitionTiming *record=timing_at(1,"acquisition-total","ok");
 g_assert_cmpuint(record->action_id,==,87654322);g_assert_cmpuint(record->contact,==,9);
 g_assert_cmpint(record->elapsed_us,==,1200);
 /* Test only the timing layer under a synthetic clock regression. It must
  * never produce negative elapsed values or affect acquisition generation. */
 guint64 generation=capture->generation;
 g_array_set_size(timing_records,0);capture->timing_active=TRUE;
 capture->timing_phase="reference-acquire";gx_timing_begin(&capture->timing,99,7000);
 timing_clock_us=6999;acquisition_timing_complete(capture,NULL);
 g_assert_cmpuint(timing_records->len,==,2);
 g_assert_cmpint(timing_at(0,"reference-acquire","ok")->elapsed_us,==,0);
 g_assert_cmpint(timing_at(1,"acquisition-total","ok")->elapsed_us,==,0);
 acquisition_timing_complete(capture,NULL);g_assert_cmpuint(timing_records->len,==,2);
 g_assert_cmpuint(capture->generation,==,generation);
 reference_teardown();
}

/* Synthetic oracle: integration100, initialDAC161, reference1000, frame3500.
 * Calls1/2 retain the factory DAC; call3 raises its target to170. All request
 * checks use explicit oracle words, never the production controller output. */
static void
adc_history_updates_only_future_requests(void)
{
  const guint8 up_seed[16]={0,0,0,0,1,1,0xff,0xff,0,1,0xff,0x7e,0x91,0x91,2,4};
  const guint16 next[4]={170,173,179,184};
  const guint16 after[4][4]={{161,164,170,175},{161,164,170,175},
                            {170,173,179,184},{179,182,188,193}};
  adc_fixture=TRUE;contact_setup();
  guint updates=adc_update_calls, resets=adc_reset_calls;
  for(guint index=0;index<4;index++) {
    if(index) contact_reset(up_seed);
    if(index==3) memcpy(contact.request_dacs,next,sizeof next);
    options.use_flat_pixel=TRUE;options.flat_pixel=3500;
    options.check_feedback=TRUE;options.updates_before_image=updates+index;
    options.expected_high_count=MIN(index+1,3);options.expected_steps=index>1?index-1:0;
    memcpy(options.expected_next_dacs,after[index],sizeof after[index]);
    contact_start();assert_image_success();
    g_assert_cmpuint(adc_update_calls,==,updates+index+1);
    g_assert_cmpuint(adc_reset_calls,==,resets);
    g_assert_cmpuint(capture->exposure.integration,==,100);
    g_assert_cmpuint(capture->exposure.initial_dac,==,161);
  }
  contact_teardown();
}

static void
adc_distribution_anchors_at_minimum_word(void)
{
  const guint8 up_seed[16]={0,0,0,0,1,1,0xff,0xff,0,1,0xff,0x7e,0x91,0x91,2,4};
  const guint16 next[4]={172,161,176,181};
  adc_fixture=adc_minimum_fixture=TRUE;contact_setup();
  guint updates=adc_update_calls;
  options.use_flat_pixel=TRUE;options.flat_pixel=3500;
  options.check_feedback=TRUE;options.updates_before_image=updates;
  options.expected_high_count=1;memcpy(options.expected_next_dacs,next,sizeof next);
  contact_start();assert_image_success();
  contact_reset(up_seed);memcpy(contact.request_dacs,next,sizeof next);
  options.use_flat_pixel=TRUE;options.flat_pixel=3500;
  options.check_feedback=TRUE;options.updates_before_image=updates+1;
  options.expected_high_count=2;memcpy(options.expected_next_dacs,next,sizeof next);
  contact_start();assert_image_success();contact_teardown();
}

static void
adc_saturated_frame_preserves_raw_without_advancing_history(void)
{
  const guint8 up_seed[16]={0,0,0,0,1,1,0xff,0xff,0,1,0xff,0x7e,0x91,0x91,2,4};
  const guint16 next[4]={172,161,176,181};
  adc_fixture=adc_minimum_fixture=TRUE;contact_setup();guint updates=adc_update_calls;
  options.use_flat_pixel=TRUE;options.flat_pixel=4095;
  options.check_feedback=TRUE;options.updates_before_image=updates;
  memcpy(options.expected_next_dacs,next,sizeof next);
  contact_start();assert_image_success();
  g_assert_cmpuint(adc_update_calls,==,updates+1);
  g_assert_cmpuint(capture->exposure.mean,==,0);
  g_assert_cmpuint(capture->exposure.high_count,==,0);g_assert_cmpuint(capture->exposure.low_count,==,0);
  /* An ineligible measurement retains history, but the wrapper still anchors
   * all four words at its current target. It neither edits nor recaptures4095. */
  contact_reset(up_seed);memcpy(contact.request_dacs,next,sizeof next);
  options.use_flat_pixel=TRUE;options.flat_pixel=3500;
  options.check_feedback=TRUE;options.updates_before_image=updates+1;
  options.expected_high_count=1;memcpy(options.expected_next_dacs,next,sizeof next);
  contact_start();assert_image_success();contact_teardown();
}

static void
adc_cancelled_held_image_never_reaches_restore(void)
{
  adc_fixture=TRUE;contact_setup();options.hold_image=TRUE;
  options.use_flat_pixel=TRUE;options.flat_pixel=3500;
  guint updates=adc_update_calls;
  contact_start();g_assert_cmpuint(contact.done,==,0);g_assert_cmpuint(contact.frames,==,1);
  g_assert_nonnull(contact.held_image);goodix_acquisition_cancel(capture);
  assert_adc_context_cleared();
  GoodixImageCallback cb=contact.held_image;gpointer data=contact.held_image_data;
  contact.held_image=NULL;cb(test_device,contact.held_image_blob,GX_BLOB_SIZE,data,NULL);
  g_assert_cmpuint(contact.done,==,1);g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(contact.restores,==,0);g_assert_cmpuint(contact.up_arms,==,0);
  g_assert_false(contact.image.valid);g_assert_cmpuint(adc_update_calls,==,updates);
  assert_adc_context_cleared();
  assert_no_successor_contact();contact_teardown();
}

static void
assert_adc_context_cleared(void)
{
  const GxImage zero_image={0};
  const GxExposureState zero_exposure={0};
  const guint16 zero_dacs[4]={0};
  g_assert_false(capture->exposure_valid);g_assert_false(capture->context_valid);
  g_assert_false(capture->reference_pending);
  g_assert_cmpmem(&capture->pending_reference_image,sizeof capture->pending_reference_image,
                  &zero_image,sizeof zero_image);
  g_assert_cmpmem(&capture->reference_image,sizeof capture->reference_image,&zero_image,sizeof zero_image);
  g_assert_cmpmem(&capture->exposure,sizeof capture->exposure,&zero_exposure,sizeof zero_exposure);
  g_assert_cmpmem(capture->live_image_dacs,sizeof capture->live_image_dacs,zero_dacs,sizeof zero_dacs);
}

static void
adc_pending_first_reference_is_owned_and_commit_only(void)
{
  const guint16 initial[4]={161,164,170,175};
  const GxImage zero_image={0};
  adc_fixture=TRUE;reference_setup();
  guint resets=adc_reset_calls, updates=adc_update_calls;
  goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
  g_assert_no_error(result_error);g_assert_cmpuint(reference.completions,==,1);
  g_assert_cmpuint(adc_reset_calls,==,resets);g_assert_cmpuint(adc_update_calls,==,updates);
  g_assert_false(capture->exposure_valid);g_assert_true(capture->reference_pending);
  g_assert_true(capture->pending_reference_image.valid);
  g_assert_cmpmem(capture->pending_reference_image.pixels,sizeof capture->pending_reference_image.pixels,
                  reference.accepted.pixels,sizeof reference.accepted.pixels);
  /* FIRST is1000; the distinct but accepted SECOND is1050. All callback-owned
   * paired staging has already been wiped. Reusing that staging cannot alter it. */
  for(guint i=0;i<GX_PIXELS;i++)
    g_assert_cmpuint(capture->pending_reference_image.pixels[2*i]|
                     ((guint16)capture->pending_reference_image.pixels[2*i+1]<<8),==,1000);
  g_assert_false(capture->paired_images[0].valid);g_assert_false(capture->paired_images[1].valid);
  memset(capture->paired_images,0xa5,sizeof capture->paired_images);
  g_assert_true(goodix_acquisition_commit_reference(capture));
  g_assert_cmpuint(adc_reset_calls,==,resets+1);g_assert_cmpuint(adc_update_calls,==,updates);
  g_assert_true(capture->exposure_valid);g_assert_true(capture->reference_image.valid);
  g_assert_cmpuint(capture->exposure.initial_dac,==,161);
  g_assert_cmpuint(capture->exposure.integration,==,100);
  g_assert_cmpuint(capture->exposure.high_count,==,0);g_assert_cmpuint(capture->exposure.low_count,==,0);
  g_assert_cmpmem(capture->live_image_dacs,sizeof capture->live_image_dacs,initial,sizeof initial);
  g_assert_cmpmem(capture->reference_image.pixels,sizeof capture->reference_image.pixels,
                  reference.accepted.pixels,sizeof reference.accepted.pixels);
  g_assert_cmpmem(&capture->pending_reference_image,sizeof capture->pending_reference_image,
                  &zero_image,sizeof zero_image);
  g_assert_false(goodix_acquisition_commit_reference(capture));
  goodix_acquisition_invalidate_context(capture);assert_adc_context_cleared();
  reference_teardown();adc_fixture=FALSE;
}

static void
adc_pending_rejection_and_reference_failure_never_initialize(void)
{
  for(guint failed=0;failed<2;failed++) {
    adc_fixture=TRUE;reference_setup();
    if(failed) {reference.fail_state=PAIRED_REFERENCE_F3_PHASE1;reference.failure_kind=1;}
    guint resets=adc_reset_calls, updates=adc_update_calls;
    goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
    if(failed) g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    else {g_assert_no_error(result_error);goodix_acquisition_invalidate_context(capture);}
    assert_adc_context_cleared();g_assert_false(goodix_acquisition_commit_reference(capture));
    g_assert_cmpuint(adc_reset_calls,==,resets);g_assert_cmpuint(adc_update_calls,==,updates);
    reference_teardown();adc_fixture=FALSE;
  }
}

static void
adc_crc_and_transport_errors_invalidate_without_extra_update(void)
{
  for(guint point=0;point<5;point++) {
    adc_fixture=TRUE;contact_setup();guint updates=adc_update_calls;
    options.use_flat_pixel=TRUE;options.flat_pixel=3500;
    if(point==0) options.corrupt_frame=1;
    if(point==1) options.failure=FAIL_IMAGE;
    if(point==2) options.failure=FAIL_RESTORE;
    if(point==3) options.failure=FAIL_UP0;
    if(point==4) options.up_length=19;
    contact_start();g_assert_cmpuint(contact.done,==,1);g_assert_nonnull(result_error);
    g_assert_false(contact.image.valid);g_assert_cmpuint(contact.none_reports,==,0);
    g_assert_cmpuint(adc_update_calls,==,updates+(point>=2));
    if(point<2) {g_assert_cmpuint(contact.restores,==,0);g_assert_cmpuint(contact.up_arms,==,0);}
    assert_adc_context_cleared();assert_no_successor_contact();contact_teardown();
  }
}

static void
adc_refresh_and_explicit_reset_clear_history(void)
{
  const guint8 up_seed[16]={0,0,0,0,1,1,0xff,0xff,0,1,0xff,0x7e,0x91,0x91,2,4};
  for(guint refresh=0;refresh<2;refresh++) {
    adc_fixture=TRUE;contact_setup();options.use_flat_pixel=TRUE;options.flat_pixel=3500;
    contact_start();assert_image_success();g_assert_cmpuint(capture->exposure.high_count,==,1);
    guint updates=adc_update_calls;
    if(refresh) {
      contact_reset(up_seed);memcpy(contact.manual_reply+4,contact.down_reply+4,16);
      contact_start();g_assert_cmpuint(contact.done,==,1);g_assert_no_error(result_error);
      g_assert_cmpuint(contact.outcome,==,GOODIX_ACQUIRED_REFRESH_REQUIRED);
      g_assert_cmpuint(contact.frames,==,0);
    } else goodix_acquisition_invalidate_context(capture);
    assert_adc_context_cleared();g_assert_cmpuint(adc_update_calls,==,updates);
    assert_no_successor_contact();contact_teardown();
  }
}

static void
adc_stale_image_owner_cannot_mutate_new_reference(void)
{
  adc_fixture=TRUE;contact_setup();options.hold_image=TRUE;
  options.use_flat_pixel=TRUE;options.flat_pixel=300;
  contact_start();g_assert_cmpuint(contact.done,==,0);
  GoodixImageCallback old_cb=contact.held_image;gpointer old_data=contact.held_image_data;
  guint8 old_raw[GX_BLOB_SIZE];memcpy(old_raw,contact.held_image_blob,sizeof old_raw);
  contact.held_image=NULL;
  guint updates=adc_update_calls;
  /* Model terminal failure before a late transport image callback. Its owner
   * token stays alive, but its old SSM is finished before a fresh acquisition. */
  fpi_ssm_mark_failed(capture->ssm,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"synthetic lost epoch"));
  g_assert_cmpuint(contact.done,==,1);assert_adc_context_cleared();
  memset(&reference,0,sizeof reference);reference.fail_state=reference.reject_state=-1;
  reference_mode=TRUE;g_clear_error(&result_error);
  goodix_acquisition_start(capture,TRUE,reference_acquired,NULL);
  g_assert_no_error(result_error);g_assert_true(goodix_acquisition_commit_reference(capture));
  reference_mode=FALSE;contact_reset(reference_seed);
  options.hold_image=TRUE;options.use_flat_pixel=TRUE;options.flat_pixel=3500;
  contact_start();g_assert_cmpuint(contact.done,==,0);g_assert_nonnull(contact.held_image);
  GxExposureState fresh=capture->exposure;GxImage background=capture->reference_image;
  old_cb(test_device,old_raw,sizeof old_raw,old_data,NULL);
  g_assert_cmpuint(adc_update_calls,==,updates);g_assert_cmpuint(contact.done,==,0);
  g_assert_cmpmem(&capture->exposure,sizeof capture->exposure,&fresh,sizeof fresh);
  g_assert_cmpmem(capture->reference_image.pixels,sizeof capture->reference_image.pixels,
                  background.pixels,sizeof background.pixels);
  g_assert_cmpuint(contact.restores,==,0);g_assert_cmpuint(contact.up_arms,==,0);
  GoodixImageCallback cb=contact.held_image;gpointer data=contact.held_image_data;
  contact.held_image=NULL;cb(test_device,contact.held_image_blob,GX_BLOB_SIZE,data,NULL);
  assert_image_success();g_assert_cmpuint(adc_update_calls,==,updates+1);
  g_assert_cmpuint(capture->exposure.high_count,==,1);g_assert_cmpuint(capture->exposure.low_count,==,0);
  contact_teardown();
}

int main(int argc, char **argv)
{
  g_test_init(&argc,&argv,NULL);
  timing_records=g_array_new(FALSE,FALSE,sizeof(AcquisitionTiming));
  guint log_handler=g_log_set_handler("libfprint-goodix521d",G_LOG_LEVEL_MESSAGE,capture_timing,NULL);
  g_test_add_func("/goodix521d/acquisition/contact-exact-sequence",contact_exact_sequence);
  g_test_add_func("/goodix521d/acquisition/contact-first-image-bytes-delivered-once",contact_first_image_bytes_delivered_once);
  g_test_add_func("/goodix521d/acquisition/contact-requires-committed-context",contact_requires_committed_context);
  g_test_add_func("/goodix521d/acquisition/contact-refresh-waits-for-up",contact_refresh_waits_for_up);
  g_test_add_func("/goodix521d/acquisition/contact-comparator-all-lanes-and-lsb",contact_comparator_all_lanes_and_lsb);
  g_test_add_func("/goodix521d/acquisition/contact-dynamic-down-seed",contact_dynamic_down_seed);
  g_test_add_func("/goodix521d/acquisition/contact-waits-for-boundaries",contact_waits_for_boundaries);
  g_test_add_func("/goodix521d/acquisition/contact-bad-images-never-restore-or-publish",contact_bad_images_never_restore_or_publish);
  g_test_add_func("/goodix521d/acquisition/contact-command-errors-stop",contact_command_errors_stop);
  g_test_add_func("/goodix521d/acquisition/contact-rejects-malformed-replies",contact_rejects_malformed_replies);
  g_test_add_func("/goodix521d/acquisition/contact-cancellation-stops-successors",contact_cancellation_stops_successors);
  g_test_add_func("/goodix521d/acquisition/contact-stale-up-owner-cannot-advance-new-contact",contact_stale_up_owner_cannot_advance_new_contact);
  g_test_add_func("/goodix521d/acquisition/contact-callback-can-release-object",contact_callback_can_release_object);
  g_test_add_func("/goodix521d/acquisition/reference-retry-success",reference_retry_success);
  g_test_add_func("/goodix521d/acquisition/reference-persistent-failure",reference_persistent_failure);
  g_test_add_func("/goodix521d/acquisition/reference-cancel-prevents-retry",reference_cancel_prevents_retry);
  g_test_add_func("/goodix521d/acquisition/reference-command-errors-do-not-retry",reference_command_errors_do_not_retry);
  g_test_add_func("/goodix521d/acquisition/reference-new-start-resets-retry",reference_new_start_resets_retry);
  g_test_add_func("/goodix521d/acquisition/timing-reference-success",timing_reference_success);
  g_test_add_func("/goodix521d/acquisition/timing-contact-success",timing_contact_success);
  g_test_add_func("/goodix521d/acquisition/timing-reference-retry-and-error",timing_reference_retry_and_error);
  g_test_add_func("/goodix521d/acquisition/timing-contact-failure-and-cancel",timing_contact_failure_and_cancel);
  g_test_add_func("/goodix521d/acquisition/timing-context-reuse-and-clamp",timing_context_reuse_and_clamp);
  g_test_add_func("/goodix521d/acquisition/adc-history-updates-only-future-requests",adc_history_updates_only_future_requests);
  g_test_add_func("/goodix521d/acquisition/adc-distribution-anchors-at-minimum-word",adc_distribution_anchors_at_minimum_word);
  g_test_add_func("/goodix521d/acquisition/adc-cancelled-held-image-never-reaches-restore",adc_cancelled_held_image_never_reaches_restore);
  g_test_add_func("/goodix521d/acquisition/adc-pending-first-reference-is-owned-and-commit-only",adc_pending_first_reference_is_owned_and_commit_only);
  g_test_add_func("/goodix521d/acquisition/adc-pending-rejection-and-reference-failure-never-initialize",adc_pending_rejection_and_reference_failure_never_initialize);
  g_test_add_func("/goodix521d/acquisition/adc-crc-and-transport-errors-invalidate-without-extra-update",adc_crc_and_transport_errors_invalidate_without_extra_update);
  g_test_add_func("/goodix521d/acquisition/adc-refresh-and-explicit-reset-clear-history",adc_refresh_and_explicit_reset_clear_history);
  g_test_add_func("/goodix521d/acquisition/adc-stale-image-owner-cannot-mutate-new-reference",adc_stale_image_owner_cannot_mutate_new_reference);
  g_test_add_func("/goodix521d/acquisition/adc-saturated-frame-preserves-raw-without-advancing-history",adc_saturated_frame_preserves_raw_without_advancing_history);
  int result=g_test_run();
  g_log_remove_handler("libfprint-goodix521d",log_handler);g_array_unref(timing_records);
  return result;
}
