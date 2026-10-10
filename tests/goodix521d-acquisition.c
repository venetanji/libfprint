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
static GError *result_error;
static GoodixDefaultCallback event_callback;
static gpointer event_data;
static GDestroyNotify event_destroy;
static FpDevice *test_device;
static GoodixAcquisition *capture;

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
 g_assert_cmpuint(address,==,0x022c);g_assert_cmpuint(value,==,0x030a);cb(dev,user,NULL);
}
void mock_goodix_tls_read_image(FpDevice *dev,guint8 *data,guint16 length,GoodixImageCallback cb,gpointer user) {
 g_assert_cmpuint(current_status,==,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
 guint8 *request=data;g_assert_cmpuint(length,==,10);g_assert_cmpuint(request[0],==,0x45);
 guint8 raw[7684]={0};memcpy(raw+7680,"\xb1\x49\xc0\xf0",4);frames++;cb(dev,raw,sizeof raw,user,NULL);
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
 current_status=FP_FINGER_STATUS_NONE;frames=done=up_arms=0;event_first=early;fail_up=error;g_clear_error(&result_error);
 test_device=g_object_new(FPI_TYPE_DEVICE_FAKE,NULL);
 guint8 otp[64]={0};otp[23]=0x6e;otp[26]=40;otp[33]=41;otp[40]=7;otp[41]=42;otp[42]=43;otp[43]=44;otp[44]=45;otp[45]=46;otp[46]=47;
 capture=goodix_acquisition_new(test_device,otp);goodix_acquisition_start(capture,FALSE,acquired,NULL);
 if(cancel_present) {
   g_assert_cmpuint(frames,==,0);g_assert_cmpuint(up_arms,==,0);g_assert_cmpuint(done,==,1);
   g_assert_error(result_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&result_error);g_object_unref(test_device);return;
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
int main(int argc,char **argv) {g_test_init(&argc,&argv,NULL);run(FALSE,FALSE);run(TRUE,FALSE);run(TRUE,TRUE);cancel_present=TRUE;run(FALSE,FALSE);cancel_present=FALSE;cancel_removal=TRUE;run(FALSE,FALSE);return 0;}
