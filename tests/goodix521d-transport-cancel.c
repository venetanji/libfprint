/* Source-executed cancellation and reentrant USB/TLS lifetime checks. */
#include <gio/gio.h>
#include <stdio.h>
typedef GObject FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef struct {FpDevice *dev;gint stage;} FpiSsm;
typedef struct {guint8 unused;} GoodixTlsServer;
typedef void (*GoodixNoneCallback)(FpDevice *,gpointer,GError *);
typedef void (*GoodixCmdCallback)(FpDevice *,guint8 *,guint16,gpointer,GError *);
typedef GoodixCmdCallback GoodixDefaultCallback;
typedef struct {GCallback callback;gpointer user_data;} GoodixCallbackInfo;
typedef struct {
 GoodixTlsServer *tls_hop;GoodixCallbackInfo *tls_ready_callback;FpiSsm *tls_ssm;
 guint64 tls_generation,read_loop_generation,operation_generation;
 gboolean cancelling,inited,read_pending,ack,reply,configuration_missing;
 GoodixCmdCallback fdt_up_callback;gpointer fdt_up_user_data;GDestroyNotify fdt_up_destroy;
 GSource *fdt_up_timeout;guint64 fdt_up_generation,fdt_up_operation,fdt_up_read_loop;
 gboolean fdt_up_acknowledged,fdt_up_received,fdt_up_started;guint fdt_up_phases_sent,fdt_up_acks;guint8 fdt_up_event[20];
 GCancellable *transfer_cancel_tkn;
 guint8 cmd,*data;guint32 length;GSource *timeout;
 GoodixCmdCallback callback;gpointer user_data;
 gint64 fdt_event_deadline;
 guint8 fdt_event_opcode;
 gboolean terminal_error;
} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate state;
static guint cancel_log_count;
static gchar *cancel_log;
static void capture_cancel_log(const gchar *domain,GLogLevelFlags level,const gchar *message,gpointer data)
{
 (void)domain;(void)level;(void)data;
 if(!g_str_has_prefix(message,"goodix-up-cancel ") && !g_str_has_prefix(message,"goodix-down-cancel "))return;
 cancel_log_count++;g_free(cancel_log);cancel_log=g_strdup(message);
}

static guint commands,tls_callbacks,transitions,failed_ssms,send_calls,event_calls,event_destroys;
static gint command_error,tls_error;
static gboolean cancel_in_send, fail_send, reenter_cancel;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define GOODIX_FLAGS_TLS 0xb0
#define fp_dbg(...) ((void)0)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static void candidate_diagnostic(FpDevice *d,GError *e,const char *site,void *t){(void)d;(void)e;(void)site;(void)t;}
void goodix_cancel(FpDevice *dev) __attribute__((weak));
void goodix_reset_state(FpDevice *dev);
void goodix_receive_done(FpDevice *,guint8 *,guint16,GError *);
static void tls_handshake_done(FpiSsm *,FpDevice *,GError *);
static void fpi_ssm_mark_failed(FpiSsm *ssm,GError *e){g_assert_true(ssm==state.tls_ssm);failed_ssms++;tls_handshake_done(ssm,ssm->dev,e);g_free(ssm);}
static gint fpi_ssm_get_cur_state(FpiSsm *ssm){g_assert_true(ssm==state.tls_ssm);return ssm->stage;}
static void fpi_ssm_next_state(FpiSsm *ssm){g_assert_true(ssm==state.tls_ssm);transitions++;}
static void fpi_ssm_set_data(FpiSsm *s,gpointer p,GDestroyNotify f){(void)s;(void)p;(void)f;}
static gboolean goodix_tls_server_wait_authenticated(GoodixTlsServer *s,GError **e){(void)s;(void)e;g_assert_not_reached();}
static void goodix_send_tls_successfully_established(FpDevice *d,GoodixNoneCallback cb,gpointer p){(void)d;(void)cb;(void)p;g_assert_not_reached();}
static void on_goodix_tls_read_handshake(FpDevice *d,guint8 *b,guint16 n,gpointer p,GError *e){(void)d;(void)b;(void)n;(void)p;(void)e;g_assert_not_reached();}
static void goodix_read_tls(FpDevice *d,GoodixCmdCallback cb,gpointer p){(void)d;(void)cb;(void)p;g_assert_not_reached();}
static int goodix_tls_client_recv(GoodixTlsServer *s,guint8 *b,guint16 n){(void)s;g_assert_cmpuint(n,>,0);b[0]=0x16;return 1;}
static gboolean goodix_send_pack(FpDevice *d,guint8 flags,guint8 *b,guint16 n,GDestroyNotify f,GError **e){(void)b;(void)f;g_assert_cmpuint(flags,==,GOODIX_FLAGS_TLS);g_assert_cmpuint(n,==,1);send_calls++;if(cancel_in_send)goodix_cancel(d);if(fail_send)g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_FAILED,"late synchronous TX failure");return !fail_send;}
static GSource *fpi_device_add_timeout(FpDevice *d,guint ms,void(*cb)(FpDevice*,gpointer),gpointer p,GDestroyNotify f){(void)d;(void)cb;(void)p;(void)f;GSource *source=g_timeout_source_new(ms);g_source_attach(source,NULL);g_source_unref(source);return source;}
#include "cancel.inc"
static void command_done(FpDevice *d,guint8 *b,guint16 n,gpointer p,GError *e){(void)b;(void)n;(void)p;commands++;command_error=e?e->code:0;g_clear_error(&e);if(reenter_cancel)goodix_cancel(d);}
static void handshake_command_done(FpDevice *d,guint8 *b,guint16 n,gpointer p,GError *e){(void)d;(void)b;(void)n;commands++;command_error=e?e->code:0;fpi_ssm_mark_failed(p,e);}
static void tls_done(FpDevice *d,gpointer p,GError *e){(void)p;tls_callbacks++;tls_error=e?e->code:0;g_clear_error(&e);if(reenter_cancel)goodix_cancel(d);}
static void event_done(FpDevice *dev,guint8 *b,guint16 n,gpointer p,GError *e){(void)b;g_assert_true(p==dev);g_assert_cmpuint(n,==,0);g_assert_error(e,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&e);event_calls++;goodix_cancel(dev);}
static void event_destroy(gpointer p){event_destroys++;g_assert_cmpuint(event_destroys,==,event_calls);g_object_unref(p);}
static void reset(FpDevice *dev){g_assert_null(state.tls_ready_callback);g_assert_null(state.tls_ssm);goodix_reset_state(dev);g_clear_object(&state.transfer_cancel_tkn);memset(&state,0,sizeof state);state.transfer_cancel_tkn=g_cancellable_new();state.inited=state.read_pending=TRUE;commands=tls_callbacks=transitions=failed_ssms=send_calls=event_calls=event_destroys=0;command_error=tls_error=0;cancel_in_send=fail_send=reenter_cancel=FALSE;}
static void pending_tls(FpDevice *dev,gint stage){state.tls_ready_callback=g_new0(GoodixCallbackInfo,1);state.tls_ready_callback->callback=G_CALLBACK(tls_done);state.tls_ssm=g_new0(FpiSsm,1);state.tls_ssm->dev=dev;state.tls_ssm->stage=stage;state.tls_generation++;}
int main(void){
 if(!goodix_cancel){fputs("FAIL: explicit cancellation API is missing\n",stderr);return 1;}
 guint log_handler=g_log_set_handler(NULL,G_LOG_LEVEL_MESSAGE,capture_cancel_log,NULL);
 FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);reset(dev);state.ack=TRUE;state.callback=command_done;reenter_cancel=TRUE;
 goodix_cancel(dev);g_assert_cmpuint(commands,==,1);g_assert_cmpint(command_error,==,G_IO_ERROR_CANCELLED);g_assert_false(state.cancelling);g_assert_false(state.inited);g_assert_false(state.read_pending);g_assert_true(g_cancellable_is_cancelled(state.transfer_cancel_tkn));g_assert_cmpuint(state.operation_generation,==,1);goodix_cancel(dev);g_assert_cmpuint(commands,==,1);
 puts("PASS: cancellation invalidates read owner and completes command once, including reentrant cancel");
 reset(dev);pending_tls(dev,0);state.reply=TRUE;state.callback=handshake_command_done;state.user_data=state.tls_ssm;reenter_cancel=TRUE;goodix_cancel(dev);
 g_assert_cmpuint(commands,==,1);g_assert_cmpuint(failed_ssms,==,1);g_assert_cmpuint(tls_callbacks,==,1);g_assert_cmpint(tls_error,==,G_IO_ERROR_CANCELLED);g_assert_null(state.tls_ssm);g_assert_null(state.tls_ready_callback);
 puts("PASS: active handshake command unwinds its SSM and TLS readiness exactly once");
 for(guint stage=0;stage<5;stage+=4)for(guint failure=0;failure<2;failure++){
  reset(dev);pending_tls(dev,stage);cancel_in_send=TRUE;fail_send=failure;tls_handshake_run(state.tls_ssm,dev);
  g_assert_cmpuint(send_calls,==,1);g_assert_cmpuint(failed_ssms,==,1);g_assert_cmpuint(tls_callbacks,==,1);g_assert_cmpuint(transitions,==,0);g_assert_null(state.tls_ssm);
 }
 puts("PASS: cancellation during either synchronous TLS USB send cannot advance or fail a freed SSM");
 reset(dev);state.tls_ready_callback=g_new0(GoodixCallbackInfo,1);state.tls_ready_callback->callback=G_CALLBACK(tls_done);goodix_cancel(dev);g_assert_cmpuint(tls_callbacks,==,1);g_assert_cmpuint(failed_ssms,==,0);
 puts("PASS: pending readiness without an active command still completes once");
 reset(dev);goodix_wait_fdt_up(dev,5000,event_done,g_object_ref(dev),event_destroy);goodix_cancel(dev);g_assert_cmpuint(event_calls,==,1);g_assert_cmpuint(event_destroys,==,1);g_assert_null(state.fdt_up_callback);g_assert_null(state.fdt_up_timeout);goodix_cancel(dev);g_assert_cmpuint(event_calls,==,1);
 puts("PASS: cancel dispatches owned FDT context once, then destroys it after callback");

 reset(dev);cancel_log_count=0;
 goodix_wait_fdt_up(dev,5000,event_done,g_object_ref(dev),event_destroy);
 state.fdt_up_started=TRUE;state.fdt_up_phases_sent=2;state.fdt_up_acks=2;
 state.fdt_up_acknowledged=TRUE;state.operation_generation=state.fdt_up_operation+2;
 goodix_cancel(dev);
 g_assert_cmpuint(cancel_log_count,==,1);
 g_assert_cmpstr(cancel_log,==,"goodix-up-cancel phases-sent=2 ack-mask=0x3 event-seen=0 owner-current-before-cancel=1");
 g_assert_cmpuint(event_calls,==,1);g_assert_cmpuint(event_destroys,==,1);
 goodix_cancel(dev);g_assert_cmpuint(cancel_log_count,==,1);
 puts("PASS: cancellation logs pre-invalidation UP state exactly once without accepting removal");
 reset(dev);cancel_log_count=0;
 goodix_wait_fdt_up(dev,5000,event_done,g_object_ref(dev),event_destroy);
 state.fdt_up_started=TRUE;state.fdt_up_phases_sent=1;state.fdt_up_acks=1;
 state.fdt_up_received=TRUE;state.operation_generation=state.fdt_up_operation+1;
 state.read_loop_generation++;
 goodix_cancel(dev);
 g_assert_cmpuint(cancel_log_count,==,1);
 g_assert_cmpstr(cancel_log,==,"goodix-up-cancel phases-sent=1 ack-mask=0x1 event-seen=1 owner-current-before-cancel=0");
 g_assert_cmpuint(event_calls,==,1);
 puts("PASS: cancellation snapshot distinguishes stale ownership and buffered event without publishing it");

 reset(dev);cancel_log_count=0;
 goodix_wait_fdt_down(dev,5000,event_done,g_object_ref(dev),event_destroy);
 state.fdt_up_started=TRUE;state.fdt_up_phases_sent=1;state.fdt_up_acks=1;
 state.fdt_up_received=TRUE;state.operation_generation=state.fdt_up_operation+1;
 goodix_cancel(dev);
 g_assert_cmpuint(event_calls,==,1);g_assert_cmpuint(event_destroys,==,1);
 g_assert_cmpstr(cancel_log,==,"goodix-down-cancel phases-sent=1 ack-mask=0x1 event-seen=1 owner-current-before-cancel=1");
 goodix_cancel(dev);g_assert_cmpuint(event_calls,==,1);g_assert_cmpuint(cancel_log_count,==,1);
 puts("PASS: DOWN cancellation retains direction-specific precancel counters and dispatches exactly once");

 reset(dev);g_clear_object(&state.transfer_cancel_tkn);g_object_unref(dev);
 g_log_remove_handler(NULL,log_handler);g_clear_pointer(&cancel_log,g_free);return 0;}
