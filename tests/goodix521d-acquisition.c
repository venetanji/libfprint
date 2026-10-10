/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real acquisition and FpiSsm, synthetic transport. No USB is opened.
 * Substitute only the external boundaries while compiling the exact production
 * source. Linker --wrap is too late for LTO: GCC can bind/integrate definitions
 * from libfprint-private before the linker gets an undefined symbol to wrap. */
#define fpi_device_get_cancellable mock_fpi_device_get_cancellable
#define fpi_device_report_finger_status mock_fpi_device_report_finger_status
#define goodix_send_mcu_switch_to_fdt_down mock_goodix_send_mcu_switch_to_fdt_down
#define goodix_send_mcu_switch_to_fdt_mode mock_goodix_send_mcu_switch_to_fdt_mode
#define goodix_send_write_sensor_register mock_goodix_send_write_sensor_register
#define goodix_tls_read_image mock_goodix_tls_read_image
#define goodix_wait_fdt_up mock_goodix_wait_fdt_up
#define goodix_send_protocol mock_goodix_send_protocol
#include "../libfprint/drivers/goodix521d/goodix-acquire.c"
#include "test-device-fake.h"
#include <string.h>
static guint frames,done,up_arms;
static gboolean event_first,fail_up,cancel_present,cancel_removal;
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
 (void)dev; current_status=status;
 if((cancel_present && status==(FP_FINGER_STATUS_PRESENT|FP_FINGER_STATUS_NEEDED)) || (cancel_removal && status==FP_FINGER_STATUS_PRESENT)) goodix_acquisition_cancel(capture);
 return TRUE;
}
void mock_goodix_send_mcu_switch_to_fdt_down(FpDevice *dev,guint8 *data,guint16 length,gboolean reply,GDestroyNotify destroy,GoodixDefaultCallback cb,gpointer user) {
 g_assert_cmpuint(length,==,39);g_assert_null(destroy);g_assert_cmpuint(data[26],==,reply);
 guint8 response[20]={2,0,255,0};cb(dev,reply?response:NULL,reply?20:0,user,NULL);
}
void mock_goodix_send_mcu_switch_to_fdt_mode(FpDevice *dev,guint8 *data,guint16 length,gboolean reply,GDestroyNotify destroy,GoodixDefaultCallback cb,gpointer user) {
 (void)data;g_assert_cmpuint(length,==,27);g_assert_false(reply);g_assert_null(destroy);cb(dev,NULL,0,user,NULL);
}
void mock_goodix_send_write_sensor_register(FpDevice *dev,guint16 address,guint16 value,GoodixNoneCallback cb,gpointer user) {
 if (reference_mode) { reference_register(dev,address,value,cb,user); return; }
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
 g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
 guint8 *request=data;g_assert_cmpuint(length,==,10);g_assert_cmpuint(request[0],==,0x45);
 guint8 raw[7684]={0};memcpy(raw+7680,"\xb1\x49\xc0\xf0",4);frames++;if(corrupt_last_image && frames==10)raw[7680]^=1;cb(dev,raw,sizeof raw,user,NULL);
}
void mock_goodix_wait_fdt_up(FpDevice *dev,guint timeout,GoodixDefaultCallback cb,gpointer data,GDestroyNotify destroy) {
 g_assert_true(dev==test_device);g_assert_cmpuint(timeout,==,30000);g_assert_null(event_callback);
 event_callback=cb;event_data=data;event_destroy=destroy;
}
static void release_event(GError *error) {
 GoodixDefaultCallback cb=event_callback;gpointer data=event_data;GDestroyNotify destroy=event_destroy;
 event_callback=NULL;event_data=NULL;event_destroy=NULL;
 guint8 reply[20]={0,2,255,0};cb(test_device,reply,20,data,error);destroy(data);
}
void mock_goodix_send_protocol(FpDevice *dev,guint8 opcode,guint8 *data,guint16 length,GDestroyNotify destroy,gboolean checksum,guint timeout,gboolean reply,GoodixCmdCallback cb,gpointer user) {
 if (reference_mode) { reference_protocol(dev,opcode,data,length,destroy,checksum,timeout,reply,cb,user); return; }
 g_assert_true(restore_acknowledged);g_assert_cmpuint(restores,==,1);
 g_assert_cmpuint(opcode,==,0x34);g_assert_cmpuint(length,==,40);g_assert_null(destroy);g_assert_true(checksum);g_assert_cmpuint(timeout,==,1000);g_assert_false(reply);
 g_assert_cmpuint(data[0],==,0x8e);for(guint i=27;i<40;i++)g_assert_cmpuint(data[i],==,0);
 up_arms++;
 if(data[26] && event_first) release_event(fail_up?g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"cancel"):NULL);
 cb(dev,NULL,0,user,fail_up&&data[26]?g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"cancel"):NULL);
}
static void acquired(FpDevice *dev,const GxImage *image,gpointer user,GError *error) {
 (void)dev;(void)user;done++;result_error=error;
 if(!error)g_assert_true(image && image->valid);
 goodix_acquisition_free(capture);capture=NULL;
}
static void run(gboolean early,gboolean error) {
 restores=0;restore_acknowledged=FALSE;restore_callback=NULL;restore_data=NULL;
 current_status=FP_FINGER_STATUS_NONE;frames=done=up_arms=0;event_first=early;fail_up=error;g_clear_error(&result_error);
 test_device=g_object_new(FPI_TYPE_DEVICE_FAKE,NULL);
 guint8 otp[64]={0};otp[23]=0x6e;otp[26]=40;otp[33]=41;otp[40]=7;otp[41]=42;otp[42]=43;otp[43]=44;otp[44]=45;otp[45]=46;otp[46]=47;if(capture_mode)otp[23]|=0x80;
 capture=goodix_acquisition_new(test_device,otp);goodix_acquisition_start(capture,FALSE,acquired,NULL);
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
 g_assert_cmpuint(frames,==,10);g_assert_cmpuint(up_arms,==,2);
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
  guint8 otp[64]={0};
  otp[23]=0x40;otp[26]=37;otp[33]=47;otp[40]=7;
  otp[41]=39;otp[42]=49;otp[43]=41;otp[44]=97;otp[45]=101;otp[46]=103;
  memset(&reference,0,sizeof reference);
  reference.reject_state=reference.fail_state=-1;
  reference_mode=TRUE;
  g_clear_error(&result_error);
  test_device=g_object_new(FPI_TYPE_DEVICE_FAKE,NULL);
  capture=goodix_acquisition_new(test_device,otp);
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

int main(int argc, char **argv)
{
  g_test_init(&argc,&argv,NULL);
  g_test_add_func("/goodix521d/acquisition/contact",contact_cases);
  g_test_add_func("/goodix521d/acquisition/reference-retry-success",reference_retry_success);
  g_test_add_func("/goodix521d/acquisition/reference-persistent-failure",reference_persistent_failure);
  g_test_add_func("/goodix521d/acquisition/reference-cancel-no-retry",reference_cancel_prevents_retry);
  g_test_add_func("/goodix521d/acquisition/reference-command-errors-no-retry",reference_command_errors_do_not_retry);
  g_test_add_func("/goodix521d/acquisition/reference-new-start-resets-retry",reference_new_start_resets_retry);
  return g_test_run();
}
