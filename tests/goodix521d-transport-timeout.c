/* The upstream timeout factory may use a stale cached main-context clock.
 * Execute transport timer creation against that behavior without sleeping. */
#include <glib.h>
typedef int FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef struct {gint64 ready;} GSource;
typedef void (*GoodixTlsCallback)(FpDevice *,guint8 *,guint16,gpointer,GError *);
typedef void (*TimeoutCallback)(FpDevice *,gpointer);
typedef struct {GoodixTlsCallback callback;gpointer user_data;gboolean reply;guint8 cmd;GSource *timeout;} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate state;
static GSource timer;
static unsigned scheduled;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define GOODIX_TIMEOUT 1000
#define fp_dbg(...) ((void)0)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static void goodix_receive_timeout_cb(FpDevice *d,gpointer p){(void)d;(void)p;}
static GSource *fpi_device_add_timeout(FpDevice *d,guint ms,TimeoutCallback cb,gpointer data,void *destroy){
 (void)d;assert(ms==GOODIX_TIMEOUT);assert(cb==goodix_receive_timeout_cb);assert(!data&&!destroy);
 timer.ready=1;scheduled++;return &timer;
}
static void g_source_set_ready_time(GSource *s,gint64 ready){s->ready=ready;}
#include "timeout.inc"
int main(void){FpDevice dev=0;gint64 before=g_get_monotonic_time();goodix_read_tls(&dev,NULL,&dev);
 assert(scheduled==1 && state.timeout==&timer && state.reply && state.user_data==&dev);
 assert(timer.ready>=before+1000000 && timer.ready<=g_get_monotonic_time()+1000000);
 puts("PASS: new transport timer uses fresh monotonic deadline despite stale main context");return 0;}
