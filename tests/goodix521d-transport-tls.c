/* Synthetic peers and keys only. No reader, key file, USB, or calibration. */
#include <errno.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>
#include "goodixtls.h"

/* Weak only to demonstrate the missing authentication barrier on old source. */
extern gboolean goodix_tls_server_wait_authenticated(GoodixTlsServer *, GError **)
  __attribute__((weak));
static atomic_int callbacks, callback_errors, forced_socket_failure,
  forced_thread_failure, forced_ctx_failure, forced_ssl_failure,
  forced_fd_failure, send_calls, read_calls, hold_callback, in_callback,
  reject_plaintext_read;
static gboolean short_sends;
SSL_CTX *__real_SSL_CTX_new(const SSL_METHOD *);
SSL_CTX *__wrap_SSL_CTX_new(const SSL_METHOD *method) {
 if(atomic_exchange(&forced_ctx_failure,0)) return NULL;
 return __real_SSL_CTX_new(method);
}
SSL *__real_SSL_new(SSL_CTX *);
SSL *__wrap_SSL_new(SSL_CTX *ctx) {
 if(atomic_exchange(&forced_ssl_failure,0)) return NULL;
 return __real_SSL_new(ctx);
}
int __real_SSL_set_fd(SSL *,int);
int __wrap_SSL_set_fd(SSL *ssl,int fd) {
 if(atomic_exchange(&forced_fd_failure,0)) return 0;
 return __real_SSL_set_fd(ssl,fd);
}
int __real_socketpair(int, int, int, int[2]);
int __wrap_socketpair(int d,int t,int p,int sv[2]) {
 if (atomic_exchange(&forced_socket_failure,0)) { errno=EMFILE; return -1; }
 return __real_socketpair(d,t,p,sv);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *v) {
 if (atomic_exchange(&forced_thread_failure,0)) return EAGAIN;
 return __real_pthread_create(t,a,f,v);
}
ssize_t __real_send(int,const void *,size_t,int);
ssize_t __wrap_send(int fd,const void *b,size_t n,int flags) {
 atomic_fetch_add(&send_calls,1); if(short_sends && n>7) n=7;
 return __real_send(fd,b,n,flags);
}
int __real_SSL_read(SSL *,void *,int);
int __wrap_SSL_read(SSL *ssl,void *b,int n) {
 assert(!atomic_load(&reject_plaintext_read) && "SSL_read called before authenticated barrier");
 atomic_fetch_add(&read_calls,1); return __real_SSL_read(ssl,b,n);
}
static void ready(GoodixTlsServer *self,GError *err,gpointer data) {
 (void)self; (void)data; atomic_fetch_add(&callbacks,1);
 if(err) atomic_fetch_add(&callback_errors,1);
 atomic_store(&in_callback,1);
 while(atomic_load(&hold_callback)) usleep(1000);
}
static void key_bytes(guint8 key[32],gboolean wrong) {
 for(int i=0;i<32;i++) key[i]=(guint8)(i*7+19);
 if(wrong) key[0]^=0xff;
}
static unsigned client_key(SSL *ssl,const char *hint,char *identity,
 unsigned max_identity,unsigned char *key,unsigned max_key) {
 (void)hint; assert(max_identity>=5 && max_key>=32); memcpy(identity,"test",5);
 key_bytes(key,SSL_get_app_data(ssl)!=NULL); return 32;
}
typedef struct { SSL_CTX *ctx; SSL *ssl; } Peer;
static Peer peer_new(gboolean wrong) {
 Peer p={0}; p.ctx=SSL_CTX_new(TLS_client_method()); assert(p.ctx);
 assert(SSL_CTX_set_min_proto_version(p.ctx,TLS1_2_VERSION));
 assert(SSL_CTX_set_max_proto_version(p.ctx,TLS1_2_VERSION));
 assert(SSL_CTX_set_cipher_list(p.ctx,"PSK-AES128-CBC-SHA256"));
 SSL_CTX_set_psk_client_callback(p.ctx,client_key);
 p.ssl=SSL_new(p.ctx); assert(p.ssl);
 SSL_set_app_data(p.ssl,wrong ? &callbacks : NULL);
 BIO *r=BIO_new(BIO_s_mem()), *w=BIO_new(BIO_s_mem()); assert(r && w);
 BIO_set_mem_eof_return(r,-1); BIO_set_mem_eof_return(w,-1);
 SSL_set_bio(p.ssl,r,w); SSL_set_connect_state(p.ssl); return p;
}
static void peer_free(Peer *p) { SSL_free(p->ssl); SSL_CTX_free(p->ctx); }
static void start(GoodixTlsServer *s) {
 memset(s,0,sizeof *s); key_bytes(s->psk,FALSE); s->connection_callback=ready;
 GError *e=NULL; assert(goodix_tls_server_init(s,&e)); assert(!e);
}
static void release(GoodixTlsServer *s) {
 GError *e=NULL; gint64 before=g_get_monotonic_time();
 assert(goodix_tls_server_deinit(s,&e)); assert(!e);
 assert(g_get_monotonic_time()-before<2500000);
 assert(!s->ssl_layer && !s->ssl_ctx && s->sock_fd==-1 && s->client_fd==-1);
 for(size_t i=0;i<sizeof s->psk;i++) assert(s->psk[i]==0);
 assert(goodix_tls_server_deinit(s,&e)); assert(!e);
}
static gboolean peer_output(GoodixTlsServer *s,Peer *p,size_t fragment) {
 guint8 bytes[16384]; int n;
 while((n=BIO_read(SSL_get_wbio(p->ssl),bytes,sizeof bytes))>0) {
   for(int off=0;off<n;) {
     int part=(int)MIN(fragment,(size_t)(n-off));
     if(goodix_tls_client_send(s,bytes+off,part)!=part) return FALSE;
     off+=part;
   }
 }
 return TRUE;
}
static gboolean handshake(GoodixTlsServer *s,Peer *p,size_t fragment) {
 gint64 end=g_get_monotonic_time()+4000000;
 while(g_get_monotonic_time()<end) {
   ERR_clear_error(); int result=SSL_do_handshake(p->ssl);
   int why=result==1 ? SSL_ERROR_NONE : SSL_get_error(p->ssl,result);
   if(!peer_output(s,p,fragment)) return FALSE;
   if(result==1) return TRUE;
   if(why!=SSL_ERROR_WANT_READ && why!=SSL_ERROR_WANT_WRITE) return FALSE;
   struct pollfd fd={.fd=s->client_fd,.events=POLLIN};
   int count=poll(&fd,1,100);
   if(count<0 && errno==EINTR) continue;
   assert(count>=0);
   if(count>0) {
     guint8 bytes[16384]; int n=goodix_tls_client_recv(s,bytes,sizeof bytes);
     if(n<=0) return FALSE;
     assert(BIO_write(SSL_get_rbio(p->ssl),bytes,n)==n);
   }
 }
 return FALSE;
}
static void authenticated(void) {
 assert(goodix_tls_server_wait_authenticated && "missing TLS Finished barrier");
 GoodixTlsServer s; start(&s); Peer p=peer_new(FALSE);
 short_sends=TRUE; atomic_store(&send_calls,0);
 assert(handshake(&s,&p,16384)); assert(atomic_load(&send_calls)>3); short_sends=FALSE;
 GError *e=NULL; assert(goodix_tls_server_wait_authenticated(&s,&e)); assert(!e);
 assert(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(s.ssl_layer))==0x00ae);
 assert(SSL_CTX_get_options(s.ssl_ctx)&SSL_OP_NO_TICKET);
 assert(atomic_load(&callbacks)==1 && atomic_load(&callback_errors)==0);
 assert(goodix_tls_server_wait_authenticated(&s,&e)); assert(!e);
 const guint8 message[]={1,7,9,42,0,255}; guint8 out[sizeof message]={0};
 assert(SSL_write(p.ssl,message,sizeof message)==sizeof message);
 assert(peer_output(&s,&p,3));
 assert(goodix_tls_server_receive(&s,out,sizeof out,&e)==sizeof message);
 assert(!e && memcmp(out,message,sizeof out)==0);
 release(&s); peer_free(&p);
}
static void unsupported_cipher(void) {
 GoodixTlsServer s;start(&s);Peer p=peer_new(FALSE);
 assert(SSL_set_cipher_list(p.ssl,"PSK-AES128-GCM-SHA256"));
 assert(!handshake(&s,&p,7));GError *e=NULL;
 assert(!goodix_tls_server_wait_authenticated(&s,&e));g_clear_error(&e);release(&s);peer_free(&p);
}
static void wrong_key(void) {
 GoodixTlsServer s; start(&s); Peer p=peer_new(TRUE); assert(!handshake(&s,&p,7));
 GError *e=NULL; assert(!goodix_tls_server_wait_authenticated(&s,&e)); assert(e); g_clear_error(&e);
 guint8 out[8]; memset(out,0xa5,sizeof out); atomic_store(&read_calls,0);
 assert(goodix_tls_server_receive(&s,out,sizeof out,&e)==-1 && e);
 assert(atomic_load(&read_calls)==0); g_clear_error(&e); release(&s); peer_free(&p);
}
static void unauthenticated(void) {
 GoodixTlsServer s; start(&s); GError *e=NULL; guint8 out[8]={0};
 gint64 before=g_get_monotonic_time(); atomic_store(&read_calls,0);
 atomic_store(&reject_plaintext_read,1);
 assert(goodix_tls_server_receive(&s,out,sizeof out,&e)==-1 && e);
 assert(g_get_monotonic_time()-before<500000 && atomic_load(&read_calls)==0);
 atomic_store(&reject_plaintext_read,0);
 g_clear_error(&e); release(&s);
}
static void handshake_timeout(void) {
 GoodixTlsServer s; start(&s); Peer p=peer_new(FALSE);
 SSL_do_handshake(p.ssl); guint8 first[3]; assert(BIO_read(SSL_get_wbio(p.ssl),first,3)==3);
 assert(goodix_tls_client_send(&s,first,3)==3); GError *e=NULL;
 gint64 before=g_get_monotonic_time();
 assert(!goodix_tls_server_wait_authenticated(&s,&e)); assert(e && e->code==G_IO_ERROR_TIMED_OUT);
 assert(g_get_monotonic_time()-before<6000000); g_clear_error(&e);
 release(&s); peer_free(&p);
}
static void blocked_read(void) {
 GoodixTlsServer s; start(&s); Peer p=peer_new(FALSE); assert(handshake(&s,&p,7));
 GError *e=NULL; assert(goodix_tls_server_wait_authenticated(&s,&e));
 guint8 out[8]; gint64 before=g_get_monotonic_time();
 assert(goodix_tls_server_receive(&s,out,sizeof out,&e)==-1);
 assert(e && e->code==G_IO_ERROR_TIMED_OUT);
 assert(g_get_monotonic_time()-before<3000000); g_clear_error(&e); release(&s); peer_free(&p);
}
static void blocked_send(void) {
 GoodixTlsServer s; start(&s); Peer p=peer_new(FALSE); assert(handshake(&s,&p,7));
 GError *e=NULL; assert(goodix_tls_server_wait_authenticated(&s,&e));
 int size=1024; assert(!setsockopt(s.client_fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof size));
 guint8 bytes[65535]={0}; gint64 before=g_get_monotonic_time();
 assert(goodix_tls_client_send(&s,bytes,sizeof bytes)==-1);
 assert(g_get_monotonic_time()-before<3000000); release(&s); peer_free(&p);
}
static void init_failure(void) {
 atomic_int *failures[]={&forced_socket_failure,&forced_thread_failure,
   &forced_ctx_failure,&forced_ssl_failure,&forced_fd_failure};
 for(size_t kind=0;kind<sizeof failures/sizeof *failures;kind++) {
   GoodixTlsServer s={0}; key_bytes(s.psk,FALSE); s.connection_callback=ready;
   atomic_store(failures[kind],1);
   GError *e=NULL; assert(!goodix_tls_server_init(&s,&e) && e); g_clear_error(&e);
   release(&s);
 }
}
static void early_cleanup(void) {
 for(int i=0;i<30;i++) { GoodixTlsServer s; start(&s); release(&s); }
}
static void peer_disconnect(void) {
 GoodixTlsServer s; start(&s); assert(!shutdown(s.client_fd,SHUT_RDWR));
 GError *e=NULL; assert(!goodix_tls_server_wait_authenticated(&s,&e)); assert(e);
 g_clear_error(&e); release(&s);
}
static void tampered_plaintext(void) {
 GoodixTlsServer s; start(&s); Peer p=peer_new(FALSE); assert(handshake(&s,&p,7));
 GError *e=NULL; assert(goodix_tls_server_wait_authenticated(&s,&e));
 const guint8 plain[]={3,6,9,12}; guint8 cipher[512], out[8];
 assert(SSL_write(p.ssl,plain,sizeof plain)==sizeof plain);
 int n=BIO_read(SSL_get_wbio(p.ssl),cipher,sizeof cipher); assert(n>0);
 cipher[n-1]^=1;
 assert(goodix_tls_client_send(&s,cipher,n)==n);
 assert(goodix_tls_server_receive(&s,out,sizeof out,&e)==-1 && e);
 g_clear_error(&e); release(&s); peer_free(&p);
}
static void recv_timeout(void) {
 GoodixTlsServer s; start(&s); guint8 bytes[8]; gint64 before=g_get_monotonic_time();
 assert(goodix_tls_client_recv(&s,bytes,sizeof bytes)==-1);
 assert(g_get_monotonic_time()-before<3000000); release(&s);
}
static void cleanup_retains(void) {
 atomic_store(&hold_callback,1); GoodixTlsServer s; start(&s);
 assert(!shutdown(s.client_fd,SHUT_RDWR));
 gint64 end=g_get_monotonic_time()+500000;
 while(!atomic_load(&in_callback)) { assert(g_get_monotonic_time()<end); usleep(1000); }
 GError *e=NULL; gint64 before=g_get_monotonic_time();
 assert(!goodix_tls_server_deinit(&s,&e)); assert(e && e->code==G_IO_ERROR_TIMED_OUT);
 assert(g_get_monotonic_time()-before<3000000);
 assert(s.ssl_layer && s.ssl_ctx && s.sock_fd>=0 && s.client_fd>=0);
 g_clear_error(&e); atomic_store(&hold_callback,0); release(&s);
}
int main(int argc,char **argv) {
 assert(argc==2); alarm(15); signal(SIGPIPE,SIG_DFL);
 struct { const char *name; void (*fn)(void); } cases[]={
   {"unsupported-cipher",unsupported_cipher},{"authenticated",authenticated},{"wrong-key",wrong_key},{"unauthenticated",unauthenticated},
   {"handshake-timeout",handshake_timeout},{"blocked-read",blocked_read},{"blocked-send",blocked_send},
   {"init-failure",init_failure},{"early-cleanup",early_cleanup},{"peer-disconnect",peer_disconnect},
   {"tampered-plaintext",tampered_plaintext},{"recv-timeout",recv_timeout},
   {"cleanup-retains",cleanup_retains}};
 for(size_t i=0;i<sizeof cases/sizeof *cases;i++) if(!strcmp(argv[1],cases[i].name)) {
   cases[i].fn(); printf("PASS: %s\n",argv[1]); return 0;
 }
 return 2;
}
