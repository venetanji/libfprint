/* Executes the production key setter/start/completion with only the USB-facing
 * initiation stubbed. Authentication and TLS I/O are covered with real OpenSSL. */
#include <openssl/crypto.h>
#include "goodixtls.h"
typedef int FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef void (*GCallback)(void);
typedef void (*GoodixNoneCallback)(FpDevice *, gpointer, GError *);
typedef struct {GCallback callback;gpointer user_data;} GoodixCallbackInfo;
typedef struct {
 GoodixTlsServer *tls_hop;
 guint8 tls_psk[32];
 gboolean tls_psk_set,cancelling;
 GoodixCallbackInfo *tls_ready_callback;
} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate state;
static unsigned init_calls, ready_calls, callbacks;
static int last_error;
static gpointer last_data;
static gboolean init_ok=TRUE;
#define FPI_DEVICE_GOODIXTLS(d) (d)
#define G_CALLBACK(cb) ((GCallback)(cb))
#define fp_dbg(...) ((void)0)
#define g_malloc0(n) calloc(1,n)
#define g_free free
#define G_IO_ERROR_NOT_INITIALIZED 10
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(FpDevice *d){(void)d;return &state;}
static void candidate_diagnostic(FpDevice *d,GError *e,const char *site,void *t){(void)d;(void)e;(void)site;(void)t;}
static void on_goodix_tls_server_ready(GoodixTlsServer *s,GError *e,gpointer d){(void)s;(void)e;(void)d;}
static void goodix_tls_ready(GoodixTlsServer *s,GError *e,gpointer d){(void)s;(void)e;(void)d;ready_calls++;}
gboolean goodix_tls_server_init(GoodixTlsServer *s,GError **e){(void)s;init_calls++;if(!init_ok)g_set_error_literal(e,G_IO_ERROR,G_IO_ERROR_FAILED,"synthetic init failure");return init_ok;}
#include "completion.inc"
static FpDevice dev;
static void completed(FpDevice *d,gpointer data,GError *e){assert(d==&dev);callbacks++;last_data=data;last_error=e?e->code:0;g_clear_error(&e);}
static void reset(void){free(state.tls_hop);free(state.tls_ready_callback);memset(&state,0,sizeof state);init_calls=ready_calls=callbacks=0;last_data=NULL;last_error=0;init_ok=TRUE;}
int main(void){
 reset();goodix_tls(&dev,completed,&dev);
 assert(callbacks==1 && last_error==G_IO_ERROR_NOT_INITIALIZED && init_calls==0 && ready_calls==0);
 puts("PASS: unset PSK rejects through caller completion without creating TLS");
 reset();guint8 zero_key[32]={0};goodix_tls_set_psk(&dev,zero_key);goodix_tls(&dev,completed,&dev);
 assert(init_calls==1 && ready_calls==1 && callbacks==0);assert(!memcmp(state.tls_hop->psk,zero_key,32));
 candidate_tls_finish(&dev,NULL);assert(callbacks==1 && !last_error && last_data==&dev && !state.tls_ready_callback);
 candidate_tls_finish(&dev,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"late completion"));assert(callbacks==1);
 puts("PASS: explicit key and caller data preserved; completion owns error exactly once");
 goodix_tls(&dev,completed,NULL);assert(callbacks==2 && last_error==G_IO_ERROR_BUSY && init_calls==1);
 puts("PASS: overlapping TLS start rejected without replacing existing session");
 reset();goodix_tls_set_psk(&dev,zero_key);init_ok=FALSE;goodix_tls(&dev,completed,&dev);
 assert(callbacks==1 && last_error==G_IO_ERROR_FAILED && init_calls==1 && ready_calls==0 && !state.tls_ready_callback);
 puts("PASS: TLS initialization failure reaches caller once");
 reset();return 0;
}
