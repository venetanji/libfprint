/* Real GLib references/cancellation around the exact production read loop.
 * Only USB submission, parser dispatch, and private device storage are fakes. */
#include <gio/gio.h>
#include <stdio.h>
typedef GObject FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef struct {guint8 ep_in;} FpiDeviceGoodixTlsClass;
typedef struct {
 GCancellable *transfer_cancel_tkn;
 guint64 read_loop_generation;
 gboolean read_pending, inited,cancelling;
 gint rx_endpoint;
 gssize rx_requested,rx_actual;
} FpiDeviceGoodixTlsPrivate;
typedef struct {
 FpDevice *device;guint8 endpoint;gssize length,actual_length;
 gboolean short_is_error;guint8 *buffer;
} FpiUsbTransfer;
typedef void (*TransferCallback)(FpiUsbTransfer *,FpDevice *,gpointer,GError *);
typedef struct {FpiUsbTransfer *transfer;TransferCallback callback;gpointer data;} Pending;
static FpiDeviceGoodixTlsPrivate state;
static FpiDeviceGoodixTlsClass cls={.ep_in=0x81};
static Pending queue[16];
static guint submitted, parsed, completed;
static gint completion_error;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define FPI_DEVICE_GOODIXTLS_GET_CLASS(d) ((void)(d),&cls)
#define GOODIX_EP_IN_MAX_BUF_SIZE 0x10000
#define fp_dbg(...) ((void)0)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static FpiUsbTransfer *fpi_usb_transfer_new(FpDevice *d){FpiUsbTransfer *t=g_new0(FpiUsbTransfer,1);t->device=d;return t;}
static void fpi_usb_transfer_fill_bulk(FpiUsbTransfer *t,guint8 ep,gsize n){t->endpoint=ep;t->length=n;}
static void fpi_usb_transfer_submit(FpiUsbTransfer *t,guint timeout,GCancellable *cancel,TransferCallback cb,gpointer data){
 g_assert_cmpuint(timeout,==,0);g_assert_true(cancel==state.transfer_cancel_tkn);g_assert_false(g_cancellable_is_cancelled(cancel));
 g_assert_cmpuint(submitted,<,G_N_ELEMENTS(queue));queue[submitted++]=(Pending){t,cb,data};
}
static void goodix_receive_pack(FpDevice *d,guint8 *bytes,guint32 n){(void)d;(void)bytes;(void)n;parsed++;}
static void goodix_receive_done(FpDevice *d,guint8 *bytes,guint16 n,GError *e){(void)d;(void)bytes;(void)n;completed++;completion_error=e?e->code:0;g_clear_error(&e);}
void goodix_receive_data(FpDevice *dev);
#include "lifecycle.inc"
static void dispatch(guint index,GError *e){Pending p=queue[index];g_assert_nonnull(p.transfer);queue[index].transfer=NULL;p.callback(p.transfer,p.transfer->device,p.data,e);g_free(p.transfer);}
static void finalized(gpointer p,GObject *o){(void)o;*(gboolean*)p=TRUE;}
int main(void){FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);gboolean dead=FALSE;g_object_weak_ref(dev,finalized,&dead);
 goodix_start_read_loop(dev);g_assert_cmpuint(submitted,==,1);g_assert_cmpuint(state.read_loop_generation,==,1);
 GCancellable *old=g_object_ref(state.transfer_cancel_tkn);goodix_start_read_loop(dev);g_assert_cmpuint(submitted,==,1);
 goodix_stop_read_loop(dev);g_assert_true(g_cancellable_is_cancelled(old));goodix_start_read_loop(dev);
 g_assert_cmpuint(submitted,==,2);g_assert_true(old!=state.transfer_cancel_tkn);
 dispatch(0,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"stale cancellation"));
 g_assert_cmpuint(parsed,==,0);g_assert_cmpuint(completed,==,0);g_assert_cmpuint(submitted,==,2);g_assert_true(state.read_pending);g_object_unref(old);
 puts("PASS: restart owns a fresh cancellation token; stale completion cannot parse or rearm");
 dispatch(1,NULL);g_assert_cmpuint(parsed,==,1);g_assert_cmpuint(submitted,==,3);g_assert_true(state.read_pending);
 goodix_receive_data(dev);g_assert_cmpuint(submitted,==,3);
 puts("PASS: current completion rearms once and repeated receive cannot duplicate submission");
 dispatch(2,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"synthetic transport failure"));
 g_assert_cmpuint(completed,==,1);g_assert_cmpint(completion_error,==,G_IO_ERROR_FAILED);g_assert_false(state.inited);g_assert_false(state.read_pending);g_assert_cmpuint(submitted,==,3);
 goodix_start_read_loop(dev);g_assert_cmpuint(submitted,==,4);g_cancellable_cancel(state.transfer_cancel_tkn);dispatch(3,NULL);
 g_assert_cmpuint(completed,==,2);g_assert_cmpint(completion_error,==,G_IO_ERROR_CANCELLED);g_assert_cmpuint(submitted,==,4);g_assert_false(state.inited);
 puts("PASS: current error/cancellation closes the loop and reports the owned command once");
 goodix_start_read_loop(dev);g_assert_cmpuint(submitted,==,5);goodix_stop_read_loop(dev);g_clear_object(&state.transfer_cancel_tkn);g_object_unref(dev);g_assert_false(dead);
 dispatch(4,NULL);g_assert_true(dead);g_assert_cmpuint(parsed,==,1);g_assert_cmpuint(submitted,==,5);
 puts("PASS: pending read retains device until stale completion releases ownership");return 0;}
