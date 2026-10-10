/* Review-only scaffolding around exact uploaded receive functions. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t guint8;typedef uint16_t guint16;typedef uint32_t guint32;
typedef unsigned int guint;typedef int64_t gint64;
typedef uint64_t guint64;typedef int gboolean;typedef size_t gsize;typedef void *gpointer;
typedef void (*GDestroyNotify)(void *);typedef struct {int code;} GError;
typedef int FpiSsm;typedef int FpDevice;typedef FpDevice FpiDeviceGoodixTls;
typedef void (*GoodixCmdCallback)(FpDevice*,guint8*,guint16,gpointer,GError*);
typedef GoodixCmdCallback GoodixDefaultCallback;
typedef struct {gboolean cancelling;GoodixDefaultCallback fdt_up_callback;gpointer fdt_up_user_data;GDestroyNotify fdt_up_destroy;void *fdt_up_timeout;guint64 fdt_up_generation,fdt_up_operation,fdt_up_read_loop;gboolean fdt_up_acknowledged,fdt_up_received,fdt_up_started;guint8 fdt_up_event[20];guint8 cmd;gboolean ack,reply,checksum_bypass,ack_additive,command_received,configuration_missing,configuration_conflict;guint64 operation_generation,configuration_generation,tls_generation,read_loop_generation;guint8 ack_echo,ack_flags,ack_checksum;guint8 *data;guint32 length;void *timeout;GDestroyNotify callback_data_destroy;gpointer user_data;GoodixCmdCallback callback;FpiSsm *tls_ssm;} FpiDeviceGoodixTlsPrivate;
static FpiDeviceGoodixTlsPrivate private_state;
#define FPI_DEVICE_GOODIXTLS(x) (x)
static FpiDeviceGoodixTlsPrivate *fpi_device_goodixtls_get_instance_private(void *x){return &private_state;}
#define TRUE 1
#define FALSE 0
#define G_MAXUINT16 UINT16_MAX
#define G_GUINT64_FORMAT "lu"
#define GUINT16_FROM_LE(x) (x)
#define GOODIX_FLAGS_MSG_PROTOCOL 0xa0
#define GOODIX_FLAGS_TLS 0xb0
#define GOODIX_FLAGS_TLS_DATA 0xb2
#define GOODIX_CMD_ACK 0xb0
#define GOODIX_CMD_READ_OTP 0xa6
#define GOODIX_CMD_REQUEST_TLS_CONNECTION 0xd0
#define GOODIX_CMD_MCU_GET_IMAGE 0x20
#define GOODIX_NULL_CHECKSUM 0x88
#define GX_OK 0
#define FP_DEVICE_ERROR 0
#define FP_DEVICE_ERROR_PROTO 1
#define G_IO_ERROR 0
#define G_IO_ERROR_NOT_INITIALIZED 2
#define G_IO_ERROR_CANCELLED 3
#define G_IO_ERROR_BUSY 4
#define G_IO_ERROR_INVALID_ARGUMENT 5
#define G_IO_ERROR_TIMED_OUT 6
#define G_IO_ERROR_INVALID_DATA 7
#define fp_dbg(...) ((void)0)
#define fp_warn(...) ((void)0)
#define G_DEBUG_HERE() ((void)0)
#define g_free free
#define g_realloc realloc
#define g_clear_pointer(pp,fn) do {void *p_=*(pp);*(pp)=NULL;if(p_)fn(p_);}while(0)
static void autofree(void *p){free(*(void**)p);}
#define g_autofree __attribute__((cleanup(autofree)))
#define g_steal_pointer(pp) ({void *p_=*(pp);*(pp)=NULL;p_;})
static void *g_memdup(const void *p,size_t n){void *q=malloc(n?n:1);if(n)memcpy(q,p,n);return q;}
#define g_memdup2 g_memdup
static GError *g_error_new_literal(int domain,int code,const char*text){GError*e=malloc(sizeof*e);e->code=code;return e;}
#define g_clear_error(pp) g_clear_pointer(pp,free)
static void g_source_destroy(void *p){free(p);}
static int fpi_ssm_get_cur_state(FpiSsm*p){return 0;}
static int tls_receive_continues(FpDevice*a,FpiSsm*b,int c,guint64 d,guint64 e,guint8 f){assert(!"TLS not supported by extracted probe");return 0;}
static int gx_b2_layout(void*a,guint32 b,gsize*c,gsize*d,gsize*e){assert(!"B2 not supported by extracted probe");return 1;}
typedef struct __attribute__((packed)){guint8 flags;guint16 length;}GoodixPack;
typedef struct __attribute__((packed)){guint8 cmd;guint16 length;}GoodixProtocol;
#define fp_err(...) ((void)0)
typedef struct __attribute__((packed)){guint8 cmd;guint8 always_true:1;guint8 has_no_config:1;guint8 reserved:6;} GoodixAck;

static void candidate_diagnostic(FpDevice*d,GError*e,const char*s,void*t){(void)d;(void)e;(void)s;(void)t;}

static GError *g_error_copy(const GError *e){GError *copy=malloc(sizeof *copy);*copy=*e;return copy;}
static void *fpi_device_add_timeout(FpDevice*d,guint ms,void(*cb)(FpDevice*,gpointer),gpointer p,void*destroy){(void)d;(void)ms;(void)cb;(void)p;(void)destroy;return malloc(1);}
static gint64 g_get_monotonic_time(void){return 100;}
static void g_source_set_ready_time(void*s,gint64 t){(void)s;(void)t;}
