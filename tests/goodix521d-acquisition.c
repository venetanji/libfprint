/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real acquisition and FpiSsm, synthetic transport. No USB is opened.
 * Substitute only the external boundaries while compiling the exact production
 * source. Linker --wrap is too late for LTO: GCC can bind/integrate definitions
 * from libfprint-private before the linker gets an undefined symbol to wrap. */
#include <glib.h>
#include <stdio.h>
static gint64 timing_clock_us = 1000;
static gint64 timing_clock_read(void) { return timing_clock_us; }
#define g_get_monotonic_time timing_clock_read
#define fpi_device_get_cancellable mock_fpi_device_get_cancellable
#define fpi_device_report_finger_status mock_fpi_device_report_finger_status
#define goodix_send_mcu_switch_to_fdt_down mock_goodix_send_mcu_switch_to_fdt_down
#define goodix_send_mcu_switch_to_fdt_mode mock_goodix_send_mcu_switch_to_fdt_mode
#define goodix_send_write_sensor_register mock_goodix_send_write_sensor_register
#define goodix_tls_read_image mock_goodix_tls_read_image
#define goodix_wait_fdt_up mock_goodix_wait_fdt_up
#define goodix_send_protocol mock_goodix_send_protocol
#include "../libfprint/drivers/goodix521d/goodix-acquire.c"
#undef g_get_monotonic_time
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
static guint frames,done,up_arms;
static gboolean event_first,fail_up,cancel_present,cancel_removal;
static gboolean event_phase0, fail_up0, cancel_up0, hold_up1, up_event_pending;
static guint up_ack_mask, removal_reports, none_reports;
static GoodixCmdCallback pending_up1;
static gpointer pending_up1_data;
static FpFingerStatusFlags current_status;
static guint restores;
static guint restore_failure;
static gboolean restore_acknowledged, hold_restore, cancel_restore, corrupt_last_image, capture_mode;
static GoodixNoneCallback restore_callback;
static gpointer restore_data;
static GError *result_error;
static GoodixDefaultCallback event_callback;
static gpointer event_data;
static GDestroyNotify event_destroy;
static FpDevice *test_device;
static GoodixAcquisition *capture;

/* Reference tests run the production acquisition and real FpiSsm. Only wire
 * completion is synthetic; no reader, calibration, or algorithm is exercised. */
static gboolean reference_mode;
static void reference_protocol(FpDevice *, guint8, guint8 *, guint16,
                               GDestroyNotify, gboolean, guint, gboolean,
                               GoodixCmdCallback, gpointer);
static void reference_register(FpDevice *, guint16, guint16, GoodixNoneCallback, gpointer);
static void reference_image(FpDevice *, guint8 *, guint16, GoodixImageCallback, gpointer);


GCancellable *mock_fpi_device_get_cancellable(FpDevice *dev) {(void)dev;return NULL;}
gboolean mock_fpi_device_report_finger_status(FpDevice *dev,FpFingerStatusFlags status) {
 (void)dev;
 if (status==FP_FINGER_STATUS_PRESENT) {
   g_assert_cmpuint(up_ack_mask,==,3);removal_reports++;
 }
 if (status==FP_FINGER_STATUS_NONE) {
   g_assert_cmpuint(up_ack_mask,==,3);none_reports++;
 }
 current_status=status;
 if((cancel_present && status==(FP_FINGER_STATUS_PRESENT|FP_FINGER_STATUS_NEEDED)) || (cancel_removal && status==FP_FINGER_STATUS_PRESENT)) goodix_acquisition_cancel(capture);
 return TRUE;
}
void mock_goodix_send_mcu_switch_to_fdt_down(FpDevice *dev,guint8 *data,guint16 length,gboolean reply,GDestroyNotify destroy,GoodixDefaultCallback cb,gpointer user) {
 timing_clock_us+=100;
 g_assert_cmpuint(length,==,39);g_assert_null(destroy);g_assert_cmpuint(data[26],==,reply);
 guint8 response[20]={2,0,255,0};cb(dev,reply?response:NULL,reply?20:0,user,NULL);
}
void mock_goodix_send_mcu_switch_to_fdt_mode(FpDevice *dev,guint8 *data,guint16 length,gboolean reply,GDestroyNotify destroy,GoodixDefaultCallback cb,gpointer user) {
 timing_clock_us+=100;
 (void)data;g_assert_cmpuint(length,==,27);g_assert_false(reply);g_assert_null(destroy);cb(dev,NULL,0,user,NULL);
}
void mock_goodix_send_write_sensor_register(FpDevice *dev,guint16 address,guint16 value,GoodixNoneCallback cb,gpointer user) {
 if (reference_mode) { reference_register(dev,address,value,cb,user); return; }
 timing_clock_us+=100;
 g_assert_cmpuint(address,==,0x022c);
 if (value==0x020a) {
   g_assert_cmpuint(frames,==,10);g_assert_cmpuint(up_arms,==,0);
   g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
   g_assert_cmpuint(++restores,==,1);
   if (hold_restore) {restore_callback=cb;restore_data=user;return;}
   if (restore_failure) {
     cb(dev,user,g_error_new_literal(restore_failure==1?FP_DEVICE_ERROR:G_IO_ERROR,
        restore_failure==1?FP_DEVICE_ERROR_PROTO:G_IO_ERROR_TIMED_OUT,"synthetic restore failure"));return;
   }
   restore_acknowledged=TRUE;
   if (cancel_restore) goodix_acquisition_cancel(capture);
 } else g_assert_cmpuint(value,==,(capture_mode?0x0305:0x030a));
 cb(dev,user,NULL);
}
void mock_goodix_tls_read_image(FpDevice *dev,guint8 *data,guint16 length,GoodixImageCallback cb,gpointer user) {
 if (reference_mode) { reference_image(dev,data,length,cb,user); return; }
 timing_clock_us+=100;
 g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
 guint8 *request=data;g_assert_cmpuint(length,==,10);g_assert_cmpuint(request[0],==,0x45);
 guint8 raw[7684]={0};memcpy(raw+7680,"\xb1\x49\xc0\xf0",4);frames++;if(corrupt_last_image && frames==10)raw[7680]^=1;cb(dev,raw,sizeof raw,user,NULL);
}
void mock_goodix_wait_fdt_up(FpDevice *dev,guint timeout,GoodixDefaultCallback cb,gpointer data,GDestroyNotify destroy) {
 g_assert_true(dev==test_device);g_assert_cmpuint(timeout,==,30000);g_assert_null(event_callback);
 /* Firmware can emit the UP event during phase 0. Subscribe before its write. */
 g_assert_cmpuint(up_arms,==,0);
 event_callback=cb;event_data=data;event_destroy=destroy;
}
static void release_event(GError *error) {
 timing_clock_us+=100;
 GoodixDefaultCallback cb=event_callback;gpointer data=event_data;GDestroyNotify destroy=event_destroy;
 event_callback=NULL;event_data=NULL;event_destroy=NULL;
 /* Successful delivery is transport-gated by both ACKs. A late callback
  * after cancellation is still delivered to exercise the real stale owner. */
 g_assert_true(error || done || up_ack_mask==3);
 guint8 reply[20]={0,2,255,0};cb(test_device,reply,20,data,error);destroy(data);
}
static void finish_up1(FpDevice *dev, GoodixCmdCallback cb, gpointer user) {
 if (fail_up) {
   cb(dev,NULL,0,user,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"phase-1 failure"));
   release_event(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"phase-1 failure"));
   return;
 }
 up_ack_mask|=2;
 cb(dev,NULL,0,user,NULL);
 if(up_event_pending) {up_event_pending=FALSE;release_event(NULL);}
}
void mock_goodix_send_protocol(FpDevice *dev,guint8 opcode,guint8 *data,guint16 length,GDestroyNotify destroy,gboolean checksum,guint timeout,gboolean reply,GoodixCmdCallback cb,gpointer user) {
 if (reference_mode) { reference_protocol(dev,opcode,data,length,destroy,checksum,timeout,reply,cb,user); return; }
 timing_clock_us+=100;
 g_assert_true(restore_acknowledged);g_assert_cmpuint(restores,==,1);
 g_assert_cmpuint(opcode,==,0x34);g_assert_cmpuint(length,==,40);g_assert_null(destroy);g_assert_true(checksum);g_assert_cmpuint(timeout,==,1000);g_assert_false(reply);
 g_assert_cmpuint(data[0],==,0x8e);for(guint i=27;i<40;i++)g_assert_cmpuint(data[i],==,0);
 g_assert_nonnull(event_callback);
 g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
 g_assert_cmpuint(done,==,0);g_assert_cmpuint(none_reports,==,0);
 up_arms++;
 if(!data[26]) {
   if(event_first && event_phase0) up_event_pending=TRUE;
   g_assert_cmpuint(up_ack_mask,==,0);
   if(fail_up0) {
     cb(dev,NULL,0,user,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"phase-0 failure"));
     return;
   }
   if(cancel_up0) goodix_acquisition_cancel(capture);
   up_ack_mask|=1;
   cb(dev,NULL,0,user,NULL);
 } else {
   g_assert_cmpuint(up_ack_mask,==,1);
   if(event_first && !event_phase0) up_event_pending=TRUE;
   if(hold_up1) {pending_up1=cb;pending_up1_data=user;return;}
   finish_up1(dev,cb,user);
 }
}
static void acquired(FpDevice *dev,const GxImage *image,gpointer user,GError *error) {
 (void)dev;(void)user;done++;result_error=error;
 if(!error)g_assert_true(image && image->valid);
 goodix_acquisition_free(capture);capture=NULL;
}
static void run(gboolean early,gboolean error) {
 reset_timing();
 restores=0;restore_acknowledged=FALSE;restore_callback=NULL;restore_data=NULL;
 up_ack_mask=removal_reports=none_reports=0;up_event_pending=FALSE;
 pending_up1=NULL;pending_up1_data=NULL;
 current_status=FP_FINGER_STATUS_NONE;frames=done=up_arms=0;event_first=early;fail_up=error;g_clear_error(&result_error);
 test_device=g_object_new(FPI_TYPE_DEVICE_FAKE,NULL);
 guint8 otp[64]={0};otp[23]=0x6e;otp[26]=40;otp[33]=41;otp[40]=7;otp[41]=42;otp[42]=43;otp[43]=44;otp[44]=45;otp[45]=46;otp[46]=47;if(capture_mode)otp[23]|=0x80;
 capture=goodix_acquisition_new(test_device,otp);
 goodix_acquisition_set_timing_context(capture,1234567,4);
 goodix_acquisition_start(capture,FALSE,acquired,NULL);
 if(cancel_present) {
   g_assert_cmpuint(frames,==,0);g_assert_cmpuint(up_arms,==,0);g_assert_cmpuint(done,==,1);
   g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&result_error);g_object_unref(test_device);return;
 }
 if (corrupt_last_image) {
   g_assert_cmpuint(frames,==,10);g_assert_cmpuint(restores,==,0);g_assert_cmpuint(up_arms,==,0);
   g_assert_cmpuint(done,==,1);g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
   g_assert_null(event_callback);g_assert_cmpuint(current_status,!=,FP_FINGER_STATUS_PRESENT);
   g_clear_error(&result_error);g_object_unref(test_device);return;
 }
 if (hold_restore) {
   g_assert_cmpuint(frames,==,10);g_assert_cmpuint(up_arms,==,0);g_assert_cmpuint(done,==,0);
   g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
   g_assert_nonnull(restore_callback);restore_acknowledged=TRUE;
   g_assert_cmpuint(timing_records->len,==,2); /* No restore success before ACK. */
   timing_clock_us+=700;
   restore_callback(test_device,restore_data,NULL);restore_callback=NULL;restore_data=NULL;
 }
 if (restore_failure || cancel_restore) {
   g_assert_cmpuint(frames,==,10);g_assert_cmpuint(restores,==,1);
   g_assert_cmpuint(up_arms,==,0);g_assert_cmpuint(done,==,1);g_assert_null(event_callback);
   if (restore_failure==1)g_assert_error(result_error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
   else g_assert_error(result_error,G_IO_ERROR,(cancel_restore?G_IO_ERROR_CANCELLED:G_IO_ERROR_TIMED_OUT));
   g_assert_cmpuint(current_status,!=,FP_FINGER_STATUS_PRESENT);
   g_clear_error(&result_error);g_object_unref(test_device);return;
 }
 if(fail_up0 || cancel_up0) {
   g_assert_cmpuint(frames,==,10);g_assert_cmpuint(up_arms,==,1);
   g_assert_cmpuint(done,==,1);g_assert_cmpuint(removal_reports,==,0);
   g_assert_cmpuint(none_reports,==,0);g_assert_null(pending_up1);
   g_assert_error(result_error,G_IO_ERROR,(cancel_up0?G_IO_ERROR_CANCELLED:G_IO_ERROR_TIMED_OUT));
   /* A held early event must not revive a failed acquisition or complete twice. */
   release_event(NULL);
   g_assert_cmpuint(done,==,1);g_assert_cmpuint(removal_reports,==,0);
   g_assert_cmpuint(none_reports,==,0);g_assert_null(capture);
   g_clear_error(&result_error);g_object_unref(test_device);return;
 }
 g_assert_cmpuint(frames,==,10);g_assert_cmpuint(up_arms,==,2);
 if(hold_up1) {
   g_assert_nonnull(pending_up1);g_assert_cmpuint(up_ack_mask,==,1);
   g_assert_cmpuint(done,==,0);g_assert_cmpuint(removal_reports,==,0);
   g_assert_cmpuint(none_reports,==,0);
   g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
   g_assert_cmpuint(timing_records->len,==,3); /* No UP completion before event. */
   timing_clock_us+=500;
   finish_up1(test_device,pending_up1,pending_up1_data);
   pending_up1=NULL;pending_up1_data=NULL;
 }
 if(cancel_removal) {
   g_assert_cmpuint(done,==,1);g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
   release_event(NULL);g_clear_error(&result_error);g_object_unref(test_device);return;
 }
 if(!early) {g_assert_cmpuint(done,==,0);g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_PRESENT);release_event(NULL);}
 if(!error)g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NONE);
 g_assert_cmpuint(done,==,1);g_assert_null(capture);g_assert_null(event_callback);
 if(error)g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);else g_assert_no_error(result_error);
 g_clear_error(&result_error);g_object_unref(test_device);
}
static void contact_cases(void) {run(FALSE,FALSE);run(TRUE,FALSE);run(TRUE,TRUE);cancel_present=TRUE;run(FALSE,FALSE);cancel_present=FALSE;cancel_removal=TRUE;run(FALSE,FALSE);cancel_removal=FALSE;
 hold_restore=TRUE;run(FALSE,FALSE);hold_restore=FALSE;
 restore_failure=1;run(FALSE,FALSE);restore_failure=2;run(FALSE,FALSE);restore_failure=0;
 cancel_restore=TRUE;run(FALSE,FALSE);cancel_restore=FALSE;
 corrupt_last_image=TRUE;run(FALSE,FALSE);corrupt_last_image=FALSE;
 capture_mode=TRUE;run(FALSE,FALSE);capture_mode=FALSE;}

static void contact_phase0_event_waits_for_both_acks(void) {
 event_phase0=TRUE;hold_up1=TRUE;run(TRUE,FALSE);
 g_assert_cmpuint(up_ack_mask,==,3);g_assert_cmpuint(removal_reports,==,1);
 g_assert_cmpuint(none_reports,==,1);hold_up1=FALSE;event_phase0=FALSE;
}
static void contact_phase0_error_stops_phase1(void) {
 event_phase0=TRUE;fail_up0=TRUE;run(TRUE,FALSE);fail_up0=FALSE;event_phase0=FALSE;
}
static void contact_phase0_cancel_stops_phase1(void) {
 event_phase0=TRUE;cancel_up0=TRUE;run(TRUE,FALSE);cancel_up0=FALSE;event_phase0=FALSE;
}

static struct {
  guint attempts, commands, images, completions;
  gint reject_state, fail_state;
  gboolean persistent, cancel_gate, hold_final;
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
  guint16 pixel=1000+reference.attempts*100;
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
reference_acquired(FpDevice *dev, const GxImage *image, gpointer data, GError *error)
{
  (void)data;
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
  otp[23]=0x40;otp[26]=37;otp[33]=47;otp[40]=7;
  otp[41]=39;otp[42]=49;otp[43]=41;otp[44]=97;otp[45]=101;otp[46]=103;
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
static void timing_contact_success(void)
{
 run(FALSE,FALSE);
 g_assert_cmpuint(timing_records->len,==,5);
 const char *phases[]={"contact-down","contact-burst","contact-restore","contact-up","acquisition-total"};
 const gint64 elapsed[]={200,3000,100,300,3600};
 for(guint i=0;i<5;i++) {
   const AcquisitionTiming *record=timing_at(i,phases[i],"ok");
   g_assert_cmpuint(record->reference,==,0);g_assert_cmpuint(record->action_id,==,1234567);
   g_assert_cmpuint(record->contact,==,4);g_assert_cmpint(record->elapsed_us,==,elapsed[i]);
 }
 g_assert_cmpuint(frames,==,10);g_assert_cmpuint(restores,==,1);g_assert_cmpuint(up_arms,==,2);
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
static void timing_contact_failure_and_cancel(void)
{
 restore_failure=2;run(FALSE,FALSE);restore_failure=0;
 g_assert_cmpuint(timing_records->len,==,4);
 g_assert_cmpint(timing_at(0,"contact-down","ok")->elapsed_us,==,200);
 g_assert_cmpint(timing_at(1,"contact-burst","ok")->elapsed_us,==,3000);
 g_assert_cmpint(timing_at(2,"contact-restore","error")->elapsed_us,==,100);
 g_assert_cmpint(timing_at(3,"acquisition-total","error")->elapsed_us,==,3300);
 corrupt_last_image=TRUE;run(FALSE,FALSE);corrupt_last_image=FALSE;
 g_assert_cmpuint(timing_records->len,==,3);
 timing_at(1,"contact-burst","error");timing_at(2,"acquisition-total","error");
 cancel_present=TRUE;run(FALSE,FALSE);cancel_present=FALSE;
 g_assert_cmpuint(timing_records->len,==,3);
 timing_at(0,"contact-down","ok");timing_at(1,"contact-burst","cancelled");
 g_assert_cmpint(timing_at(2,"acquisition-total","cancelled")->elapsed_us,==,200);
 cancel_removal=TRUE;run(FALSE,FALSE);cancel_removal=FALSE;
 /* run() delivers a late event after cancellation. It cannot duplicate logs. */
 g_assert_cmpuint(timing_records->len,==,5);
 timing_at(3,"contact-up","cancelled");timing_at(4,"acquisition-total","cancelled");
}
static void timing_waits_for_verified_boundaries(void)
{
 hold_restore=TRUE;run(FALSE,FALSE);hold_restore=FALSE;
 g_assert_cmpuint(timing_records->len,==,5);
 g_assert_cmpint(timing_at(2,"contact-restore","ok")->elapsed_us,==,800);
 g_assert_cmpint(timing_at(4,"acquisition-total","ok")->elapsed_us,==,4300);
 event_phase0=TRUE;hold_up1=TRUE;run(TRUE,FALSE);hold_up1=FALSE;event_phase0=FALSE;
 g_assert_cmpuint(timing_records->len,==,5);
 g_assert_cmpint(timing_at(3,"contact-up","ok")->elapsed_us,==,800);
 g_assert_cmpint(timing_at(4,"acquisition-total","ok")->elapsed_us,==,4100);
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

int main(int argc, char **argv)
{
  g_test_init(&argc,&argv,NULL);
  timing_records=g_array_new(FALSE,FALSE,sizeof(AcquisitionTiming));
  guint log_handler=g_log_set_handler("libfprint-goodix521d",G_LOG_LEVEL_MESSAGE,capture_timing,NULL);
  g_test_add_func("/goodix521d/acquisition/contact",contact_cases);
  g_test_add_func("/goodix521d/acquisition/contact-phase0-event-waits-both-acks",contact_phase0_event_waits_for_both_acks);
  g_test_add_func("/goodix521d/acquisition/contact-phase0-error-stops-phase1",contact_phase0_error_stops_phase1);
  g_test_add_func("/goodix521d/acquisition/contact-phase0-cancel-stops-phase1",contact_phase0_cancel_stops_phase1);
  g_test_add_func("/goodix521d/acquisition/reference-retry-success",reference_retry_success);
  g_test_add_func("/goodix521d/acquisition/reference-persistent-failure",reference_persistent_failure);
  g_test_add_func("/goodix521d/acquisition/reference-cancel-no-retry",reference_cancel_prevents_retry);
  g_test_add_func("/goodix521d/acquisition/reference-command-errors-no-retry",reference_command_errors_do_not_retry);
  g_test_add_func("/goodix521d/acquisition/reference-new-start-resets-retry",reference_new_start_resets_retry);
  g_test_add_func("/goodix521d/acquisition/timing-reference",timing_reference_success);
  g_test_add_func("/goodix521d/acquisition/timing-contact",timing_contact_success);
  g_test_add_func("/goodix521d/acquisition/timing-reference-retry-error",timing_reference_retry_and_error);
  g_test_add_func("/goodix521d/acquisition/timing-contact-error-cancel",timing_contact_failure_and_cancel);
  g_test_add_func("/goodix521d/acquisition/timing-context-reuse-clamp",timing_context_reuse_and_clamp);
  g_test_add_func("/goodix521d/acquisition/timing-verified-boundaries",timing_waits_for_verified_boundaries);
  int result=g_test_run();
  g_log_remove_handler("libfprint-goodix521d",log_handler);g_array_unref(timing_records);
  return result;
}
