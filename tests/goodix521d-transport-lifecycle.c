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
 guint read_outstanding;
 gint rx_endpoint;
 gssize rx_requested,rx_actual;
} FpiDeviceGoodixTlsPrivate;
typedef struct {
 FpDevice *device;guint8 endpoint;gssize length,actual_length;
 gboolean short_is_error;guint8 *buffer;
} FpiUsbTransfer;
typedef void (*GoodixNoneCallback)(FpDevice *,gpointer,GError *);
typedef void (*TransferCallback)(FpiUsbTransfer *,FpDevice *,gpointer,GError *);
typedef struct {FpiUsbTransfer *transfer;TransferCallback callback;gpointer data;} Pending;
static FpiDeviceGoodixTlsPrivate state;
static FpiDeviceGoodixTlsClass cls={.ep_in=0x81};
static Pending queue[16];
static guint submitted, parsed, completed;
static gint completion_error;
static gboolean *device_dead;
static guint timeout_finalized;
static GMainContext *action_context;
/* Match the libfprint timeout's borrowed device and finalization ordering. */
typedef struct {GSource source;FpDevice *device;} DeviceTimeout;
static gboolean timeout_dispatch(GSource *source,GSourceFunc cb,gpointer data){
 ((void (*)(FpDevice *,gpointer))(void (*)(void))cb)(((DeviceTimeout *)source)->device,data);return G_SOURCE_REMOVE;
}
static void timeout_finalize(GSource *source){
 (void)source;g_assert_false(*device_dead);timeout_finalized++;
}
static GSourceFuncs timeout_funcs={NULL,NULL,timeout_dispatch,timeout_finalize,NULL,NULL};
static GSource *fpi_device_add_timeout(FpDevice *d,guint ms,void (*cb)(FpDevice *,gpointer),gpointer data,GDestroyNotify destroy){
 DeviceTimeout *t=(DeviceTimeout *)g_source_new(&timeout_funcs,sizeof *t);t->device=d;
 g_source_set_callback(&t->source,G_SOURCE_FUNC(cb),data,destroy);
 g_source_set_ready_time(&t->source,g_get_monotonic_time()+(gint64)ms*1000);
 g_source_attach(&t->source,action_context);g_source_unref(&t->source);return &t->source;
}
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
void goodix_wait_read_idle(FpDevice *,guint,GoodixNoneCallback,gpointer) __attribute__((weak));
#include "lifecycle.inc"
static void dispatch(guint index,GError *e){Pending p=queue[index];g_assert_nonnull(p.transfer);queue[index].transfer=NULL;p.callback(p.transfer,p.transfer->device,p.data,e);g_free(p.transfer);}
static void finalized(gpointer p,GObject *o){(void)o;*(gboolean*)p=TRUE;}
typedef struct {guint calls,closed;gint error;gboolean drop_ref,reenter;} DrainResult;
static void drained(FpDevice *dev,gpointer data,GError *error){
 DrainResult *r=data;r->calls++;r->error=error?error->code:0;
 if(!error){g_assert_cmpuint(state.read_outstanding,==,0);r->closed++;}
 g_clear_error(&error);
 if(r->reenter){r->reenter=FALSE;goodix_wait_read_idle(dev,100,drained,r);}
 if(r->drop_ref){r->drop_ref=FALSE;g_object_unref(dev);}
}
static void until_called(GMainContext *context,DrainResult *r,guint calls){
 gint64 deadline=g_get_monotonic_time()+G_TIME_SPAN_SECOND;
 while(r->calls<calls && g_get_monotonic_time()<deadline){g_main_context_iteration(context,FALSE);g_usleep(1000);}
 g_assert_cmpuint(r->calls,==,calls);
 while(g_main_context_iteration(context,FALSE));
}
int main(void){
 if(!goodix_wait_read_idle){fputs("FAIL: owned USB RX drainage API is missing\n",stderr);return 1;}
 FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);gboolean dead=FALSE;g_object_weak_ref(dev,finalized,&dead);device_dead=&dead;
 goodix_start_read_loop(dev);g_assert_cmpuint(state.read_outstanding,==,1);g_assert_cmpuint(submitted,==,1);g_assert_cmpuint(state.read_loop_generation,==,1);
 GCancellable *old=g_object_ref(state.transfer_cancel_tkn);goodix_start_read_loop(dev);g_assert_cmpuint(submitted,==,1);
 goodix_stop_read_loop(dev);g_assert_true(g_cancellable_is_cancelled(old));goodix_start_read_loop(dev);
 g_assert_cmpuint(submitted,==,2);g_assert_cmpuint(state.read_outstanding,==,2);g_assert_true(old!=state.transfer_cancel_tkn);
 dispatch(0,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"stale cancellation"));
 g_assert_cmpuint(state.read_outstanding,==,1);g_assert_cmpuint(parsed,==,0);g_assert_cmpuint(completed,==,0);g_assert_cmpuint(submitted,==,2);g_assert_true(state.read_pending);g_object_unref(old);
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
 puts("PASS: pending read retains device until stale completion releases ownership");
 g_assert_cmpuint(state.read_outstanding,==,0);
 dev=g_object_new(G_TYPE_OBJECT,NULL);dead=FALSE;g_object_weak_ref(dev,finalized,&dead);
 action_context=g_main_context_new();
 goodix_start_read_loop(dev);goodix_stop_read_loop(dev);goodix_start_read_loop(dev);goodix_stop_read_loop(dev);
 g_clear_object(&state.transfer_cancel_tkn);g_assert_false(state.read_pending);g_assert_cmpuint(state.read_outstanding,==,2);
 DrainResult result={0};goodix_wait_read_idle(dev,100,drained,&result);g_assert_cmpuint(result.calls,==,0);
 while(g_main_context_iteration(NULL,FALSE)) {}
 g_assert_cmpuint(result.calls,==,0);
 while(g_main_context_iteration(action_context,FALSE)) {}
 g_assert_cmpuint(result.calls,==,0);
 dispatch(5,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"first stale cancellation"));
 g_assert_cmpuint(state.read_outstanding,==,1);g_assert_cmpuint(result.closed,==,0);
 while(g_main_context_iteration(action_context,FALSE)) {}
 g_assert_cmpuint(result.calls,==,0);
 dispatch(6,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"second stale cancellation"));
 g_assert_cmpuint(state.read_outstanding,==,0);g_assert_cmpuint(result.calls,==,0);
 until_called(action_context,&result,1);g_assert_cmpuint(result.closed,==,1);
 puts("PASS: USB close waits for all stale RX completions on the device action context");
 goodix_start_read_loop(dev);goodix_stop_read_loop(dev);g_clear_object(&state.transfer_cancel_tkn);
 result=(DrainResult){0};goodix_wait_read_idle(dev,5,drained,&result);
 until_called(action_context,&result,1);g_assert_cmpint(result.error,==,G_IO_ERROR_TIMED_OUT);g_assert_cmpuint(result.closed,==,0);g_assert_cmpuint(state.read_outstanding,==,1);
 g_object_unref(dev);g_assert_false(dead);dispatch(7,NULL);g_assert_true(dead);g_assert_cmpuint(result.calls,==,1);
 puts("PASS: bounded drain timeout forbids close and preserves the pending RX owner");
 dev=g_object_new(G_TYPE_OBJECT,NULL);dead=FALSE;g_object_weak_ref(dev,finalized,&dead);
 result=(DrainResult){.reenter=TRUE,.drop_ref=TRUE};goodix_wait_read_idle(dev,100,drained,&result);
 g_assert_cmpuint(result.calls,==,0);until_called(action_context,&result,2);
 g_assert_cmpuint(result.closed,==,2);g_assert_true(dead);g_assert_cmpuint(timeout_finalized,==,4);
 puts("PASS: already-idle completion is deferred, reentrant and safe at final device unref");
 g_main_context_unref(action_context);return 0;}
