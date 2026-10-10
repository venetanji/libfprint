/* Production USB packet sender under nested event-loop cancellation. */
#include <gio/gio.h>
#include <stdio.h>
typedef GObject FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef struct {guint8 ep_out;} FpiDeviceGoodixTlsClass;
typedef struct {guint8 cmd;gboolean cancelling;guint64 operation_generation,read_loop_generation;} FpiDeviceGoodixTlsPrivate;
typedef struct {gboolean short_is_error;guint8 endpoint;gssize length,actual_length;} FpiUsbTransfer;
static FpiDeviceGoodixTlsPrivate state;
static FpiDeviceGoodixTlsClass cls={.ep_out=1};
static guint writes,frees;
static gboolean cancel_first,fail_first;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define FPI_DEVICE_GOODIXTLS_GET_CLASS(d) ((void)(d),&cls)
#define GOODIX_EP_OUT_MAX_BUF_SIZE 64
#define GOODIX_TIMEOUT 1000
#define GOODIX_CMD_NOP 0
#define fp_info(...) ((void)0)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static FpiUsbTransfer *fpi_usb_transfer_new(FpDevice *d){(void)d;return g_new0(FpiUsbTransfer,1);}
static void fpi_usb_transfer_fill_bulk_full(FpiUsbTransfer *t,guint8 ep,guint8 *data,gsize n,GDestroyNotify destroy){(void)data;g_assert_null(destroy);t->endpoint=ep;t->length=n;}
static void fpi_usb_transfer_unref(FpiUsbTransfer *t){g_free(t);}
static gboolean fpi_usb_transfer_submit_sync(FpiUsbTransfer *t,guint timeout,GError **e){g_assert_cmpuint(timeout,==,1000);writes++;t->actual_length=t->length;if(writes==1&&cancel_first){state.read_loop_generation++;state.operation_generation++;}if(writes==1&&fail_first){g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"synthetic timeout");return FALSE;}return TRUE;}
static void candidate_diagnostic(FpDevice *d,GError *e,const char *site,FpiUsbTransfer *t){(void)d;(void)e;(void)site;(void)t;}
#include "send.inc"
static void freed(gpointer p){frees++;g_free(p);}
int main(void){FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);GError *e=NULL;
 cancel_first=TRUE;g_assert_false(goodix_send_data(dev,g_malloc0(128),128,freed,&e));g_assert_error(e,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&e);g_assert_cmpuint(writes,==,1);g_assert_cmpuint(frees,==,1);
 puts("PASS: reentrant cancellation stops remaining USB fragments and frees the payload once");
 writes=frees=0;cancel_first=FALSE;state.cancelling=TRUE;g_assert_false(goodix_send_data(dev,g_malloc0(128),128,freed,&e));g_assert_error(e,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&e);g_assert_cmpuint(writes,==,0);g_assert_cmpuint(frees,==,1);
 puts("PASS: cancellation guard blocks a new direct packet send");
 writes=frees=0;state.cancelling=FALSE;fail_first=TRUE;g_assert_false(goodix_send_data(dev,g_malloc0(128),128,freed,&e));g_assert_error(e,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);g_clear_error(&e);g_assert_cmpuint(writes,==,1);g_assert_cmpuint(frees,==,1);
 puts("PASS: original USB error survives without a second fragment or duplicate free");
 writes=frees=0;fail_first=FALSE;g_assert_true(goodix_send_data(dev,g_malloc0(128),128,freed,&e));g_assert_null(e);g_assert_cmpuint(writes,==,2);g_assert_cmpuint(frees,==,1);
 puts("PASS: live packet sends all fragments exactly once");g_object_unref(dev);return 0;}
