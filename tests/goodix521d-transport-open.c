/* Source-executed claim/cleanup ownership. No USB hardware is accessed. */
#include <gio/gio.h>
#include <openssl/crypto.h>
#include <stdio.h>
typedef GObject FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef struct {gint interface;} FpiDeviceGoodixTlsClass;
typedef struct {
 gboolean first_error_logged,ack,reply,inited,read_pending,tls_psk_set;
 gint rx_endpoint;gssize rx_requested,rx_actual;
 GSource *timeout;gpointer callback,user_data,tls_hop;
 guint8 *data,tls_psk[32];guint32 length;
 guint64 operation_generation,read_loop_generation;
 GCancellable *transfer_cancel_tkn;
} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate state;
static FpiDeviceGoodixTlsClass cls;
static guint claims,releases,shutdowns;
static gboolean claim_ok,shutdown_ok;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define FPI_DEVICE_GOODIXTLS_GET_CLASS(d) ((void)(d),&cls)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static gpointer fpi_device_get_usb_device(FpDevice *d){return d;}
static gboolean g_usb_device_claim_interface(gpointer d,gint i,gint f,GError **e){(void)d;(void)i;(void)f;claims++;if(!claim_ok)g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_FAILED,"synthetic claim failure");return claim_ok;}
static gboolean g_usb_device_release_interface(gpointer d,gint i,gint f,GError **e){(void)d;(void)i;(void)f;(void)e;releases++;return TRUE;}
static gboolean goodix_shutdown_tls(FpDevice *d,GError **e){(void)d;shutdowns++;if(!shutdown_ok)g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"synthetic retained TLS worker");return shutdown_ok;}
#include "open.inc"
int main(void){FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);GError *e=NULL;
 claim_ok=FALSE;g_assert_false(goodix_dev_init(dev,&e));g_assert_nonnull(e);g_clear_error(&e);g_assert_null(state.transfer_cancel_tkn);g_assert_cmpuint(claims,==,1);
 puts("PASS: interface claim failure releases its cancellation token");
 claim_ok=TRUE;g_assert_true(goodix_dev_init(dev,&e));g_assert_null(e);g_assert_nonnull(state.transfer_cancel_tkn);GCancellable *token=state.transfer_cancel_tkn;
 g_assert_false(goodix_dev_init(dev,&e));g_assert_error(e,G_IO_ERROR,G_IO_ERROR_BUSY);g_clear_error(&e);g_assert_true(token==state.transfer_cancel_tkn);g_assert_cmpuint(claims,==,2);
 puts("PASS: reopening an active transport cannot replace owned state");
 state.inited=TRUE;state.tls_psk_set=TRUE;memset(state.tls_psk,0xa5,32);shutdown_ok=FALSE;
 g_assert_false(goodix_dev_deinit(dev,&e));g_assert_error(e,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);g_clear_error(&e);g_assert_cmpuint(releases,==,0);g_assert_true(token==state.transfer_cancel_tkn);g_assert_true(state.tls_psk_set);g_assert_true(g_cancellable_is_cancelled(token));
 puts("PASS: incomplete TLS cleanup retains transport ownership without USB release");
 shutdown_ok=TRUE;g_assert_true(goodix_dev_deinit(dev,&e));g_assert_null(e);g_assert_cmpuint(releases,==,1);g_assert_cmpuint(shutdowns,==,2);g_assert_null(state.transfer_cancel_tkn);g_assert_false(state.tls_psk_set);for(guint i=0;i<32;i++)g_assert_cmpuint(state.tls_psk[i],==,0);
 puts("PASS: successful cleanup retry releases token/interface and cleanses key");g_object_unref(dev);return 0;}
