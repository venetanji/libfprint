#pragma once
/* Test-only subset: production is compiled unchanged against this GLib shim. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
typedef int gboolean;
typedef unsigned int guint;
typedef uint8_t guint8;
typedef uint16_t guint16;
typedef uint32_t guint32;
typedef int64_t gint64;
typedef size_t gsize;
typedef void *gpointer;
typedef struct { int domain, code; char *message; } GError;
typedef pthread_mutex_t GMutex;
typedef pthread_cond_t GCond;
#define TRUE 1
#define FALSE 0
#define G_MAXINT INT32_MAX
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define G_IO_ERROR 1
#define G_FILE_ERROR 2
#define G_IO_ERROR_FAILED 1
#define G_IO_ERROR_INVALID_DATA 2
#define G_IO_ERROR_CLOSED 3
#define G_IO_ERROR_TIMED_OUT 4
#define G_IO_ERROR_BUSY 5
#define G_IO_ERROR_INVALID_ARGUMENT 6
#define G_IO_ERROR_CONNECTION_CLOSED 7
#define G_IO_ERROR_CANCELLED 8
#define G_IO_ERROR_PERMISSION_DENIED 9
#define G_IO_ERROR_NOT_INITIALIZED 10
#define g_assert assert
#define g_strerror strerror
static inline GError *g_error_new_literal(int domain, int code, const char *msg) {
 GError *e = calloc(1, sizeof *e); assert(e); e->domain=domain; e->code=code; e->message=strdup(msg); assert(e->message); return e;
}
static inline GError *g_error_new(int domain, int code, const char *fmt, ...) {
 char message[1024]; va_list ap; va_start(ap,fmt); vsnprintf(message,sizeof message,fmt,ap); va_end(ap); return g_error_new_literal(domain,code,message);
}
static inline GError *g_error_copy(const GError *e) { return g_error_new_literal(e->domain,e->code,e->message); }
static inline void g_error_free(GError *e) { if(e) { free(e->message); free(e); } }
static inline void g_clear_error(GError **e) { if(e) { g_error_free(*e); *e=NULL; } }
static inline void g_set_error_literal(GError **e,int domain,int code,const char *msg) { if(e && !*e) *e=g_error_new_literal(domain,code,msg); }
static inline void g_set_error(GError **e,int domain,int code,const char *fmt,...) {
 char message[1024]; va_list ap; va_start(ap,fmt); vsnprintf(message,sizeof message,fmt,ap); va_end(ap); g_set_error_literal(e,domain,code,message);
}
static inline void g_mutex_init(GMutex *m) { assert(!pthread_mutex_init(m,NULL)); }
static inline void g_mutex_clear(GMutex *m) { assert(!pthread_mutex_destroy(m)); }
static inline gboolean g_mutex_trylock(GMutex *m) { return pthread_mutex_trylock(m)==0; }
static inline void g_mutex_lock(GMutex *m) { assert(!pthread_mutex_lock(m)); }
static inline void g_mutex_unlock(GMutex *m) { assert(!pthread_mutex_unlock(m)); }
static inline void g_cond_init(GCond *c) {
 pthread_condattr_t a; assert(!pthread_condattr_init(&a)); assert(!pthread_condattr_setclock(&a,CLOCK_MONOTONIC)); assert(!pthread_cond_init(c,&a)); pthread_condattr_destroy(&a);
}
static inline void g_cond_clear(GCond *c) { assert(!pthread_cond_destroy(c)); }
static inline void g_cond_broadcast(GCond *c) { assert(!pthread_cond_broadcast(c)); }
static inline gboolean g_cond_wait_until(GCond *c,GMutex *m,gint64 us) {
 struct timespec t={.tv_sec=us/1000000,.tv_nsec=(us%1000000)*1000}; return pthread_cond_timedwait(c,m,&t)==0;
}
static inline gint64 g_get_monotonic_time(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return (gint64)t.tv_sec*1000000+t.tv_nsec/1000; }
