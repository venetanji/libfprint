/* Exact production response parsing, synthetic public digest bytes only. */
#include <gio/gio.h>
#include <stdio.h>
#include "goodix_proto.h"
typedef GObject FpDevice;
typedef struct {GCallback callback;gpointer user_data;} GoodixCallbackInfo;
typedef void (*GoodixPresetPskReadCallback)(FpDevice *,gboolean,guint32,guint8 *,guint16,gpointer,GError *);
static gboolean succeeded,errored;
static guint calls;
static guint16 count;
static guint32 flags;
static void reply(FpDevice *d,gboolean ok,guint32 f,guint8 *data,guint16 n,gpointer p,GError *e){g_assert_true(d==p);calls++;succeeded=ok;errored=e!=NULL;count=n;flags=f;if(ok&&n==32)for(guint i=0;i<32;i++)g_assert_cmpuint(data[i],==,i);g_clear_error(&e);}
#include "psk-reply.inc"
static void run(FpDevice *d,guint8 *data,guint16 n){GoodixCallbackInfo *info=g_new0(GoodixCallbackInfo,1);info->callback=G_CALLBACK(reply);info->user_data=d;succeeded=errored=FALSE;count=flags=0;guint before=calls;goodix_receive_preset_psk_read(d,data,n,info,NULL);g_assert_cmpuint(calls,==,before+1);}
int main(void){FpDevice *dev=g_object_new(G_TYPE_OBJECT,NULL);guint8 data[41]={0,0x78,0x56,0x34,0x12,32};for(guint i=0;i<32;i++)data[9+i]=(guint8)i;
 run(dev,data,sizeof data);g_assert_true(succeeded);g_assert_false(errored);g_assert_cmpuint(count,==,32);g_assert_cmpuint(flags,==,0x12345678);
 memset(data+5,0xff,4);run(dev,data,13);g_assert_false(succeeded);g_assert_true(errored);g_assert_cmpuint(count,==,0);
 puts("PASS: overflowing PSK response length cannot expose bytes beyond the packet");
 memset(data+5,0,4);data[5]=32;run(dev,data,40);g_assert_false(succeeded);g_assert_true(errored);
 run(dev,data,8);g_assert_false(succeeded);g_assert_true(errored);
 data[0]=1;run(dev,data,41);g_assert_false(succeeded);g_assert_false(errored);
 puts("PASS: PSK response flags/length decode and truncated/negative responses fail closed");g_object_unref(dev);return 0;}
