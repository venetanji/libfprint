/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exact-source register sender, framing, ACK parser and completion. Only the
 * final USB write and unused libfprint/TLS surfaces are synthetic. */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include "goodix_proto.h"

typedef GObject FpDevice;
typedef FpDevice FpiDeviceGoodixTls;
typedef int FpiSsm;
typedef void (*GoodixCmdCallback)(FpDevice *, guint8 *, guint16, gpointer, GError *);
typedef GoodixCmdCallback GoodixDefaultCallback;
typedef void (*GoodixNoneCallback)(FpDevice *, gpointer, GError *);
typedef struct { GCallback callback; gpointer user_data; } GoodixCallbackInfo;
typedef struct {
  gboolean cancelling;
  GoodixDefaultCallback fdt_up_callback;
  gpointer fdt_up_user_data;
  GDestroyNotify fdt_up_destroy;
  GSource *fdt_up_timeout;
  guint64 fdt_up_generation, fdt_up_operation, fdt_up_read_loop;
  gboolean fdt_up_acknowledged, fdt_up_received, fdt_up_started;guint fdt_up_phases_sent,fdt_up_acks;
  guint8 fdt_up_event[20], cmd;
  gboolean ack, reply, checksum_bypass, ack_additive, command_received;
  gboolean configuration_missing, configuration_conflict;
  guint64 operation_generation, configuration_generation, tls_generation, read_loop_generation;
  guint8 ack_echo, ack_flags, ack_checksum;
  guint8 *data;
  guint32 length;
  GSource *timeout;
  gpointer user_data;
  GoodixCmdCallback callback;
  FpiSsm *tls_ssm;
 gint64 fdt_event_deadline;
 guint8 fdt_event_opcode;
 gboolean terminal_error;
} FpiDeviceGoodixTlsPrivate;

static FpiDeviceGoodixTlsPrivate state;
static FpDevice *device;
static guint writes, completions, timer_ms;
static GError *completion_error;
static GError *write_error;
static GBytes *wire;

#define FPI_DEVICE_GOODIXTLS(d) (d)
#define GOODIX_TIMEOUT 1000
#define FP_DEVICE_ERROR (g_quark_from_static_string("test-fp-device-error"))
#define FP_DEVICE_ERROR_PROTO 1
#define GX_OK 0
#define fp_dbg(...) ((void) 0)
#define fp_warn(...) ((void) 0)
#define fp_err(...) ((void) 0)
#undef G_DEBUG_HERE
#define G_DEBUG_HERE() ((void) 0)

static FpiDeviceGoodixTlsPrivate *
fpi_device_goodixtls_get_instance_private(FpDevice *dev) { return &state; }
static void candidate_diagnostic(FpDevice *dev, GError *error, const char *site, void *transfer) { }
static int fpi_ssm_get_cur_state(FpiSsm *ssm) { g_assert_not_reached(); }
static int tls_receive_continues(FpDevice *dev, FpiSsm *ssm, int stage,
                                guint64 a, guint64 b, guint8 command) { g_assert_not_reached(); }
static int gx_b2_layout(void *data, guint32 length, gsize *offset,
                       gsize *cipher_length, gsize *capacity) { g_assert_not_reached(); }

/* Declared before the extracted ACK/event helpers. */
void goodix_receive_done(FpDevice *, guint8 *, guint16, GError *);
void goodix_receive_timeout_cb(FpDevice *, gpointer);

/* fpi timeout callbacks have no error parameter. */
typedef struct { FpDevice *dev; void (*callback)(FpDevice *, gpointer); gpointer data; } CommandTimer;
static gboolean command_timer_dispatch(gpointer data)
{
  CommandTimer *timer = data;
  timer->callback(timer->dev, timer->data);
  return G_SOURCE_REMOVE;
}
static GSource *fpi_device_add_timeout(FpDevice *dev, guint interval,
                                      void (*callback)(FpDevice *, gpointer),
                                      gpointer data, GDestroyNotify destroy)
{
  CommandTimer *timer = g_new0(CommandTimer, 1);
  timer->dev = dev; timer->callback = callback; timer->data = data;
  g_assert_null(destroy);
  timer_ms = interval;
  GSource *source = g_timeout_source_new(interval);
  g_source_set_callback(source, command_timer_dispatch, timer, g_free);
  g_source_attach(source, NULL);
  g_source_unref(source);
  return source;
}

static gboolean goodix_send_data(FpDevice *dev, guint8 *data, guint32 length,
                                 GDestroyNotify destroy, GError **error)
{
  g_assert_true(dev == device);
  writes++;
  g_clear_pointer(&wire, g_bytes_unref);
  wire = g_bytes_new(data, length);
  if (destroy) destroy(data);
  if (write_error) {
    *error = g_steal_pointer(&write_error);
    return FALSE;
  }
  return TRUE;
}

#include "register.inc"

static void completed(FpDevice *dev, gpointer data, GError *error)
{
  g_assert_true(dev == device);
  g_assert_true(data == &state);
  completions++;
  g_assert_null(completion_error);
  completion_error = error;
}
static void command_completed(FpDevice *dev, guint8 *data, guint16 length,
                              gpointer user_data, GError *error)
{ completed(dev, user_data, error); }

static void reset(void)
{
  g_assert_false(state.ack || state.reply);
  g_assert_null(state.timeout);
  g_clear_pointer(&state.data, g_free);
  memset(&state, 0, sizeof state);
  g_clear_pointer(&wire, g_bytes_unref);
  g_clear_error(&completion_error);
  g_clear_error(&write_error);
  writes = completions = timer_ms = 0;
}
static void send_restore(void)
{
  goodix_send_write_sensor_register(device, 0x022c, 0x020a, completed, &state);
  g_assert_cmpuint(writes, ==, 1);
  g_assert_cmpuint(timer_ms, ==, 1000);
}
static void assert_wire(void)
{
  const guint8 expected[] = {0xa0, 0x09, 0x00, 0xa9,
                            0x80, 0x06, 0x00, 0x00, 0x2c, 0x02, 0x0a, 0x02, 0xea};
  gsize length;
  const guint8 *data = g_bytes_get_data(wire, &length);
  g_assert_cmpuint(length, ==, 64);
  g_assert_cmpmem(data, sizeof expected, expected, sizeof expected);
  for (guint i = sizeof expected; i < length; i++)
    g_assert_cmpuint(data[i], ==, 0);
}
static void receive_full(guint8 command, const guint8 *body, guint16 length, gboolean corrupt)
{
  g_autofree guint8 *protocol = NULL;
  g_autofree guint8 *packet = NULL;
  guint32 protocol_length, packet_length;
  goodix_encode_protocol(command, (guint8 *) body, length, TRUE, FALSE,
                         &protocol, &protocol_length);
  if (corrupt) protocol[protocol_length - 1] ^= 1;
  goodix_encode_pack(GOODIX_FLAGS_MSG_PROTOCOL, protocol, protocol_length, FALSE,
                     &packet, &packet_length);
  goodix_receive_pack(device, packet, packet_length);
}
static void receive(guint8 command, const guint8 *body, guint16 length)
{ receive_full(command, body, length, FALSE); }
static void ack(guint8 echoed, guint8 flags)
{
  const guint8 body[] = {echoed, flags};
  receive(GOODIX_CMD_ACK, body, sizeof body);
}
static void expire(void)
{
  g_assert_nonnull(state.timeout);
  g_source_set_ready_time(state.timeout, 0);
  while (g_main_context_iteration(NULL, FALSE));
  g_assert_cmpuint(completions, ==, 1);
  g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_null(state.timeout);
  g_assert_false(state.ack || state.reply);
}


void goodix_wait_fdt_down(FpDevice *,guint,GoodixDefaultCallback,gpointer,GDestroyNotify) __attribute__((weak));
static guint8 event_opcode=0x34;
static guint up_commands, up_events;
static GError *up_error;
static gboolean up_fail_second,up_cancel_after_first,up_expire_second;
static void up_event(FpDevice *d,guint8 *data,guint16 length,gpointer user,GError *error)
{
  up_events++;g_assert_null(up_error);up_error=error;
  if (!error) {g_assert_cmpuint(length,==,20);g_assert_cmpuint(data[1],==,event_opcode==0x34?2:0);}
}
static void up_phase(FpDevice *d,guint8 *data,guint16 length,gpointer user,GError *error)
{
  up_commands++;
  if(error){g_clear_error(&error);return;}
  if(!GPOINTER_TO_UINT(user)) {
    if(up_cancel_after_first) {
      goodix_fdt_up_done(d,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"synthetic between-phase cancellation"));return;
    }
    if(up_expire_second)state.fdt_event_deadline=g_get_monotonic_time()-1;
    if(up_fail_second)write_error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_BROKEN_PIPE,"synthetic phase1 write failure");
    guint8 payload[40]={0x8e,1};payload[26]=1;
    goodix_send_protocol(d,event_opcode,payload,40,NULL,TRUE,1000,FALSE,up_phase,GUINT_TO_POINTER(1));
  }
}
static void up_prepare(void)
{
  reset();up_commands=up_events=0;up_fail_second=up_cancel_after_first=up_expire_second=FALSE;g_clear_error(&up_error);
  if(event_opcode==0x34) goodix_wait_fdt_up(device,30000,up_event,NULL,NULL);
  else {g_assert_nonnull(goodix_wait_fdt_down);goodix_wait_fdt_down(device,30000,up_event,NULL,NULL);}
}
static void up_start(void)
{
  guint8 payload[40]={0x8e,1};
  goodix_send_protocol(device,event_opcode,payload,40,NULL,TRUE,1000,FALSE,up_phase,NULL);
}
static guint packet(guint8 *out,guint8 cmd,const guint8 *body,guint n)
{
  out[0]=0xa0;out[1]=n+4;out[2]=0;out[3]=out[0]+out[1];
  out[4]=cmd;out[5]=n+1;out[6]=0;memcpy(out+7,body,n);
  out[7+n]=(guint8)(0xaa-goodix_calc_checksum(out+4,n+3));return n+8;
}
static void up_early_test(void)
{
  const guint8 event[20]={event_opcode==0x32?2:0,event_opcode==0x34?2:0},receipt[2]={event_opcode,1};
  for(guint order=0;order<5;order++) {
    up_prepare();up_start();g_assert_cmpuint(writes,==,1);g_assert_cmpuint(up_commands,==,0);
    if(order==0) {receive(event_opcode,event,20);ack(event_opcode,1);}
    else if(order<3) {
      guint8 joined[38];guint n=packet(joined,order==1?event_opcode:0xb0,order==1?event:receipt,order==1?20:2);
      n+=packet(joined+n,order==1?0xb0:event_opcode,order==1?receipt:event,order==1?2:20);
      goodix_receive_pack(device,joined,n);
    } else {ack(event_opcode,1);if(order==3)receive(event_opcode,event,20);}
    g_assert_cmpuint(writes,==,2);g_assert_cmpuint(up_events,==,0);
    ack(event_opcode,1);if(order==4) {g_assert_cmpuint(up_events,==,0);receive(event_opcode,event,20);}
    g_assert_cmpuint(up_commands,==,2);g_assert_cmpuint(up_events,==,1);g_assert_no_error(up_error);
    ack(event_opcode,1);receive(event_opcode,event,20);g_assert_cmpuint(up_events,==,1);
  }
  puts("PASS: phase0/phase1 events and either coalesced order require both ordered ACKs exactly once");

  up_prepare();guint8 skipped_phase[40]={0x8e,1};skipped_phase[26]=1;
  goodix_send_protocol(device,event_opcode,skipped_phase,40,NULL,TRUE,1000,FALSE,command_completed,&state);
  g_assert_cmpuint(writes,==,0);g_assert_error(completion_error,G_IO_ERROR,G_IO_ERROR_BUSY);
  g_assert_false(state.fdt_up_started);g_assert_cmpuint(up_events,==,0);
  goodix_fdt_up_done(device,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"test cleanup"));
  puts("PASS: phase1 cannot skip phase0 or transmit under an unstarted owner");

  up_prepare();receive(event_opcode,event,20);g_assert_false(state.fdt_up_received);up_start();ack(event_opcode,1);ack(event_opcode,1);
  g_assert_cmpuint(up_events,==,0);goodix_fdt_up_timeout_cb(device,NULL);
  g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);
  g_assert_nonnull(strstr(up_error->message,"phases-sent=2 ack-mask=0x3 event-seen=0 owner-current=1"));
  puts("PASS: pre-arm event cannot release and timeout retains bounded scalar diagnosis");

  up_prepare();up_start();guint8 joined[48];guint n=packet(joined,0xb0,receipt,2);
  n+=packet(joined+n,0xb0,receipt,2);n+=packet(joined+n,event_opcode,event,20);goodix_receive_pack(device,joined,n);
  g_assert_cmpuint(up_commands,==,1);g_assert_cmpuint(up_events,==,0);g_assert_cmpuint(state.fdt_up_acks,==,1);
  ack(event_opcode,1);g_assert_cmpuint(up_events,==,1);g_assert_no_error(up_error);
  puts("PASS: extra phase0-batch ACK cannot acknowledge successor phase1");

  for(guint phase=0;phase<2;phase++) {
    up_prepare();up_start();if(phase)ack(event_opcode,1);receive(event_opcode,event,20);ack(event_opcode,3);
    g_assert_cmpuint(up_events,==,1);g_assert_nonnull(up_error);g_assert_true(state.configuration_missing);
  }
  puts("PASS: configuration loss in either phase invalidates even a buffered UP");

  up_prepare();up_fail_second=TRUE;up_start();receive(event_opcode,event,20);ack(event_opcode,1);
  g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_BROKEN_PIPE);
  g_assert_cmpuint(writes,==,2);puts("PASS: phase1 write failure cannot publish buffered UP");

  up_prepare();up_cancel_after_first=TRUE;up_start();receive(event_opcode,event,20);ack(event_opcode,1);
  g_assert_cmpuint(writes,==,1);g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
  puts("PASS: between-phase cancellation prevents phase1 and buffered release");

  for(guint kind=0;kind<4;kind++) {
    up_prepare();up_start();
    if(kind==0)receive_full(event_opcode,event,20,TRUE);
    else if(kind==1) {guint8 down[20]={event_opcode==0x34?2:0,event_opcode==0x32?2:0};receive(event_opcode,down,20);}
    else if(kind==2)receive(event_opcode,event,19);
    else goodix_receive_timeout_cb(device,NULL);
    g_assert_cmpuint(up_events,==,1);g_assert_nonnull(up_error);g_assert_cmpuint(writes,==,1);
  }
  puts("PASS: phase0 bad checksum/IRQ/length/ACK-timeout never advance or release");

  up_prepare();up_start();receive(0x36,event,20);
  receive(event_opcode==0x32?0x34:0x32,event,20);g_assert_false(state.fdt_up_received);
  ack(event_opcode,1);ack(event_opcode,1);state.read_loop_generation++;receive(event_opcode,event,20);
  g_assert_cmpuint(up_events,==,0);goodix_fdt_up_timeout_cb(device,NULL);
  g_assert_nonnull(strstr(up_error->message,"owner-current=0"));
  puts("PASS: wrong opcode and stale read generation cannot satisfy the two-phase owner");
  g_clear_error(&up_error);
  up_prepare();up_start();ack(event_opcode,1);ack(event_opcode,1);
  state.fdt_event_deadline=g_get_monotonic_time()-1;
  receive(event_opcode,event,20);
  g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);
  puts("PASS: expired event deadline rejects completion even before timer dispatch");
  g_clear_error(&up_error);
  up_prepare();up_start();receive(event_opcode,event,20);
  state.fdt_event_deadline=g_get_monotonic_time()-1;
  ack(event_opcode,1);
  g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);
  g_assert_cmpuint(writes,==,1);g_assert_false(state.ack || state.reply);
  puts("PASS: late phase0 ACK cannot start phase1 or publish buffered event");
  g_clear_error(&up_error);
  up_prepare();state.fdt_event_deadline=g_get_monotonic_time()-1;up_start();
  g_assert_cmpuint(writes,==,0);g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);
  g_clear_error(&up_error);
  up_prepare();up_expire_second=TRUE;up_start();ack(event_opcode,1);
  g_assert_cmpuint(writes,==,1);g_assert_cmpuint(up_events,==,1);g_assert_error(up_error,G_IO_ERROR,G_IO_ERROR_TIMED_OUT);
  g_clear_error(&up_error);
  puts("PASS: expired owner sends neither initial phase nor delayed successor phase");
}

static void reverse_fault_test(void)
{
  const guint8 opcodes[]={0x32,0x34};
  const guint8 active[]={0,0x20,0x36};
  for(guint i=0;i<2;i++)for(guint j=0;j<3;j++)for(guint r=0;r<2;r++) {
    reset();state.cmd=active[j];state.ack=state.reply=active[j]!=0;state.callback=command_completed;state.user_data=&state;
    guint8 event[20]={r?0x82:0x80,0};receive(opcodes[i],event,20);
    g_assert_true(state.terminal_error);g_assert_cmpuint(completions,==,active[j]?1:0);
    if(active[j])g_assert_nonnull(completion_error);
    g_assert_false(state.ack || state.reply);
  }
  reset();guint8 ordinary[20]={2,0};receive(0x32,ordinary,20);g_assert_false(state.terminal_error);
  guint8 opaque[20]={0x80,0};receive(0x20,opaque,20);g_assert_false(state.terminal_error);
  reset();state.cmd=0x20;state.ack=state.reply=TRUE;state.callback=command_completed;state.user_data=&state;
  receive(0x32,ordinary,20);g_assert_false(state.terminal_error);g_assert_cmpuint(completions,==,0);
  goodix_receive_done(device,NULL,0,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"test cleanup"));
  reset();receive(0x32,opaque,19);g_assert_false(state.terminal_error);
  for(guint r=0;r<2;r++)for(guint joined_mode=0;joined_mode<2;joined_mode++) {
    guint8 manual[20]={r?0x82:0x80,0},receipt[2]={0x36,1},joined[38];
    reset();state.cmd=0x36;state.ack=state.reply=TRUE;state.callback=command_completed;state.user_data=&state;
    if(joined_mode) {
      guint n=packet(joined,0xb0,receipt,2);n+=packet(joined+n,0x36,manual,20);
      goodix_receive_pack(device,joined,n);
    } else {ack(0x36,1);receive(0x36,manual,20);}
    g_assert_cmpuint(completions,==,1);g_assert_no_error(completion_error);g_assert_false(state.terminal_error);
    reset();receive(0x36,manual,20);g_assert_false(state.terminal_error);
  }
  puts("PASS: owned and unsolicited manual36 IRQ0080/0082 remain snapshots, not reverse events");
  guint8 saved_opcode=event_opcode;
  for(guint i=0;i<2;i++) {
    event_opcode=i?0x34:0x32;up_prepare();up_start();ack(event_opcode,1);
    guint8 event[20]={i?0:2,i?2:0},receipt[2]={event_opcode,1},joined[66];
    guint n=packet(joined,event_opcode,event,20);
    n+=packet(joined+n,0xb0,receipt,2);n+=packet(joined+n,i?0x32:0x34,opaque,20);
    goodix_receive_pack(device,joined,n);
    g_assert_true(state.terminal_error);g_assert_cmpuint(up_events,==,1);g_assert_nonnull(up_error);
    g_assert_cmpuint(up_commands,==,2);g_clear_error(&up_error);
  }
  event_opcode=saved_opcode;
  puts("PASS: reverse after apparent event/ACK success aborts the whole owned batch before publication");
  puts("PASS: only exact qualified FDT reverse events invalidate active or idle transport");
}

int main(void)
{
  device = g_object_new(G_TYPE_OBJECT, NULL);
  reset(); send_restore(); assert_wire();
  g_assert_true(state.ack); g_assert_false(state.reply);
  g_assert_cmpuint(completions, ==, 0);
  ack(0x80, 1);
  g_assert_cmpuint(completions, ==, 1); g_assert_no_error(completion_error);
  g_assert_null(state.timeout); g_assert_false(state.ack || state.reply);
  ack(0x80, 1); g_assert_cmpuint(completions, ==, 1);
  puts("PASS: restore022c=020a is exact opcode80/00 2c 02 0a 02 wire bytes and completes once only after ACK80");

  reset(); send_restore(); ack(0x34, 1);
  g_assert_cmpuint(completions, ==, 0); g_assert_true(state.ack);
  const guint8 data_reply[] = {1, 0};
  receive(0x80, data_reply, sizeof data_reply);
  g_assert_cmpuint(completions, ==, 0); g_assert_true(state.ack);
  ack(0x80, 1);
  g_assert_cmpuint(completions, ==, 1); g_assert_no_error(completion_error);
  puts("PASS: wrong-command ACK and opcode80 data cannot substitute for the restore ACK");

  const guint8 negative_flags[] = {0, 2, 0xff};
  for (guint i = 0; i < G_N_ELEMENTS(negative_flags); i++) {
    reset(); send_restore(); ack(0x80, negative_flags[i]);
    g_assert_cmpuint(completions, ==, 0); g_assert_true(state.ack);
    expire(); ack(0x80, 1); g_assert_cmpuint(completions, ==, 1);
  }
  puts("PASS: negative/invalid ACK flags never report restore success and fail through the bounded timeout");

  reset(); send_restore(); expire();
  puts("PASS: missing restore ACK propagates the production one-second command timeout");

  reset(); write_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                              "synthetic register USB write failure");
  GError *expected_error = write_error;
  send_restore(); assert_wire();
  g_assert_cmpuint(completions, ==, 1); g_assert_true(completion_error == expected_error);
  g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_assert_null(state.timeout); g_assert_false(state.ack || state.reply);
  ack(0x80, 1); g_assert_cmpuint(completions, ==, 1);
  puts("PASS: restore USB write error retains identity and completes once without an ACK wait");

  reset(); send_restore();
  const guint8 corrupt_ack[] = {0x80, 1};
  receive_full(GOODIX_CMD_ACK, corrupt_ack, sizeof corrupt_ack, TRUE);
  g_assert_cmpuint(completions, ==, 1);
  g_assert_error(completion_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null(state.timeout);
  ack(0x80, 1); g_assert_cmpuint(completions, ==, 1);
  puts("PASS: malformed restore ACK checksum fails closed without later duplicate completion");

  reset(); send_restore();
  goodix_receive_done(device, NULL, 0,
    g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "synthetic transport cancellation"));
  g_assert_cmpuint(completions, ==, 1);
  g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(state.timeout);
  ack(0x80, 1); g_assert_cmpuint(completions, ==, 1);
  puts("PASS: transport cancellation reaches the restore callback once and rejects a late ACK");

  reset(); send_restore(); ack(0x80, 3);
  g_assert_cmpuint(completions, ==, 1); g_assert_no_error(completion_error);
  g_assert_true(state.configuration_missing);
  guint8 up[40] = {0};
  goodix_send_protocol(device, 0x34, up, sizeof up, NULL, TRUE,
                       1000, FALSE, command_completed, &state);
  g_assert_cmpuint(writes, ==, 1);
  g_assert_cmpuint(completions, ==, 2);
  g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  puts("PASS: ACK80/03 records configuration loss and blocks the following UP command before any write");

  reverse_fault_test();
  up_early_test();
  event_opcode=0x32;up_early_test();
  puts("PASS: complete two-phase safety matrix also holds for DOWN opcode32/IRQ0002");
  reset(); g_object_unref(device); return 0;
}
