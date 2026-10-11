/* Source-executed, read-only health checks; no reader or TLS socket is used. */
#include <gio/gio.h>
#include <stdio.h>
#include "goodixtls.h"
typedef GObject FpDevice;
typedef struct {guint64 read_loop,tls,configuration;} GoodixTransportHealth;
typedef struct {
 gboolean terminal_error,inited,read_pending,cancelling,configuration_missing,configuration_conflict;
 gboolean ack,reply;gpointer callback,fdt_up_callback,tls_ssm,tls_ready_callback,timeout;
 guint32 length;GCancellable *transfer_cancel_tkn;GoodixTlsServer *tls_hop;
 guint64 read_loop_generation,tls_generation,configuration_generation;
} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate state;
#define FPI_DEVICE_GOODIXTLS(d) (d)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
gboolean goodix_transport_health(FpDevice*,GoodixTransportHealth*,GError**) __attribute__((weak));
typedef void (*GoodixResetCallback)(void);
void goodix_send_reset(FpDevice *,gboolean,guint8,GoodixResetCallback,gpointer) __attribute__((weak));
typedef struct {GCallback callback;gpointer user_data;} GoodixCallbackInfo;
typedef struct {gboolean soft_reset_mcu,reset_sensor;guint8 sleep_time;} GoodixReset;
#define GOODIX_CMD_RESET 0xa2
#define GOODIX_TIMEOUT 1000
#define goodix_receive_reset NULL
static guint reset_sends;
static void goodix_send_protocol(FpDevice *d,guint8 cmd,guint8 *p,gsize n,gpointer x,gboolean a,guint t,gboolean r,gpointer cb,gpointer u){(void)d;(void)p;(void)n;(void)x;(void)a;(void)t;(void)r;(void)cb;g_assert_cmpuint(cmd,==,GOODIX_CMD_RESET);reset_sends++;g_free(u);}
#include "health.inc"
static void denied(FpDevice *d){GoodixTransportHealth h={9,9,9};GError *e=NULL;g_assert_false(goodix_transport_health(d,&h,&e));g_assert_nonnull(e);g_clear_error(&e);g_assert_cmpuint(h.read_loop,==,0);g_assert_cmpuint(h.tls,==,0);g_assert_cmpuint(h.configuration,==,0);}
int main(void){
 if(!goodix_transport_health){fputs("FAIL: transport health API missing\n",stderr);return 1;}
 FpDevice *d=g_object_new(G_TYPE_OBJECT,NULL);GoodixTlsServer tls={0};GoodixTransportHealth a,b;GError *e=NULL;
 state.transfer_cancel_tkn=g_cancellable_new();state.inited=state.read_pending=TRUE;
 state.read_loop_generation=3;state.tls_generation=5;state.configuration_generation=7;state.tls_hop=&tls;
 denied(d);g_mutex_init(&tls.mutex);tls.primitives_initialized=TRUE;tls.initialized=tls.handshake_done=tls.authenticated=TRUE;
 g_assert_true(goodix_transport_health(d,&a,&e));g_assert_null(e);g_assert_true(goodix_transport_health(d,&b,&e));g_assert_cmpmem(&a,sizeof a,&b,sizeof b);g_assert_cmpuint(a.read_loop,==,3);g_assert_cmpuint(a.tls,==,5);g_assert_cmpuint(a.configuration,==,7);
#define BAD(field) state.field=TRUE;denied(d);state.field=FALSE
 BAD(terminal_error);BAD(cancelling);BAD(configuration_missing);BAD(configuration_conflict);BAD(ack);BAD(reply);
 state.timeout=d;denied(d);state.timeout=NULL;
 state.length=1;denied(d);state.length=0;state.fdt_up_callback=d;denied(d);state.fdt_up_callback=NULL;
 /* A current RX callback runs before rearming; inited still owns that loop. */
 state.read_pending=FALSE;g_assert_true(goodix_transport_health(d,&b,&e));state.read_pending=TRUE;state.inited=FALSE;denied(d);state.inited=TRUE;
 g_cancellable_cancel(state.transfer_cancel_tkn);denied(d);g_clear_object(&state.transfer_cancel_tkn);state.transfer_cancel_tkn=g_cancellable_new();
 tls.serve_thread_started=TRUE;denied(d);tls.serve_thread_started=FALSE;tls.closing=TRUE;denied(d);tls.closing=FALSE;
 tls.authenticated=FALSE;denied(d);tls.authenticated=TRUE;tls.first_error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"synthetic");denied(d);g_clear_error(&tls.first_error);
 g_mutex_lock(&tls.mutex);denied(d);g_mutex_unlock(&tls.mutex);
 state.read_loop_generation++;state.tls_generation++;state.configuration_generation++;g_assert_true(goodix_transport_health(d,&b,&e));g_assert_cmpuint(b.read_loop,==,a.read_loop+1);g_assert_cmpuint(b.tls,==,a.tls+1);g_assert_cmpuint(b.configuration,==,a.configuration+1);
 goodix_send_reset(d,TRUE,10,NULL,NULL);g_assert_cmpuint(reset_sends,==,1);g_assert_true(state.configuration_missing);g_assert_cmpuint(state.configuration_generation,==,b.configuration+1);denied(d);
 g_mutex_clear(&tls.mutex);g_clear_object(&state.transfer_cancel_tkn);g_object_unref(d);puts("PASS: transport health is fail-closed, nonblocking, generation-bound and read-only");return 0;
}
