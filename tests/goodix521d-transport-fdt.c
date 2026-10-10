/* Synthetic FDT-up events through the production receiver and event waiter. */
#include "goodix521d-transport-receiver-shim.h"
void goodix_wait_fdt_up(FpDevice *,guint,GoodixCmdCallback,gpointer,GDestroyNotify) __attribute__((weak));
#include "receiver.inc"
static FpDevice dev;
static unsigned command_calls,event_calls,command_errors,event_errors,destroys;
static guint8 received[20];
static gboolean replace_on_event;
static void command_done(FpDevice *d,guint8 *p,guint16 n,gpointer u,GError *e){(void)d;(void)p;(void)n;(void)u;command_calls++;command_errors+=e!=NULL;free(e);}
static void event_destroy(gpointer p){assert(p==&dev);assert(event_calls==destroys+1);destroys++;}
static void event_done(FpDevice *d,guint8 *p,guint16 n,gpointer u,GError *e){assert(d==&dev&&u==&dev);event_calls++;event_errors+=e!=NULL;if(!e){assert(n==20);memcpy(received,p,n);if(replace_on_event){private_state.cmd=0x90;private_state.ack=TRUE;private_state.reply=FALSE;private_state.callback=command_done;}}free(e);}
static size_t packet(guint8 *out,guint8 cmd,const guint8 *body,guint16 n){out[0]=0xa0;out[1]=(guint8)(n+4);out[2]=(guint8)((n+4)>>8);out[3]=(guint8)(out[0]+out[1]+out[2]);out[4]=cmd;out[5]=(guint8)(n+1);out[6]=(guint8)((n+1)>>8);memcpy(out+7,body,n);out[7+n]=(guint8)(0xaa-goodix_calc_checksum(out+4,n+3));return n+8;}
static void reset(void){if(private_state.fdt_up_callback)goodix_fdt_up_done(&dev,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"test reset"));assert(event_calls==destroys);free(private_state.data);free(private_state.timeout);free(private_state.fdt_up_timeout);memset(&private_state,0,sizeof private_state);command_calls=event_calls=command_errors=event_errors=destroys=0;memset(received,0,sizeof received);replace_on_event=FALSE;}
static void arm(void){goodix_wait_fdt_up(&dev,5000,event_done,&dev,event_destroy);assert(!event_calls);private_state.fdt_up_started=TRUE;private_state.fdt_up_phases_sent=2;private_state.fdt_up_acks=1;private_state.operation_generation++;private_state.cmd=0x34;private_state.ack=TRUE;private_state.callback=command_done;}
int main(void){
 if(!goodix_wait_fdt_up){fputs("FAIL: bounded FDT-up waiter API is missing\n",stderr);return 1;}
 guint8 ack[10],event[28],down[28],wrong[28],bad[28],joined[66];guint8 receipt[]={0x34,1},body[20]={0,2};for(unsigned i=2;i<20;i++)body[i]=(guint8)i;
 packet(ack,0xb0,receipt,2);packet(event,0x34,body,20);packet(wrong,0x36,body,20);body[0]=2;body[1]=0;packet(down,0x34,body,20);memcpy(bad,event,28);bad[27]^=1;
 reset();arm();goodix_receive_pack(&dev,ack,10);assert(command_calls==1&&!command_errors&&!event_calls);goodix_receive_pack(&dev,event,28);assert(event_calls==1&&!event_errors&&!memcmp(received,event+7,20));puts("PASS: ACK alone cannot release; exact checksummed UP event completes after ACK");
 reset();arm();goodix_receive_pack(&dev,event,28);assert(!command_calls&&!event_calls);goodix_receive_pack(&dev,ack,10);assert(command_calls==1&&event_calls==1&&!event_errors);puts("PASS: event before ACK is retained without premature completion");
 for(unsigned order=0;order<2;order++){reset();arm();memcpy(joined,order?event:ack,order?28:10);memcpy(joined+(order?28:10),order?ack:event,order?10:28);goodix_receive_pack(&dev,joined,38);assert(command_calls==1&&event_calls==1&&!command_errors&&!event_errors);}puts("PASS: coalesced ACK/event in either order preserves phase ownership");
 reset();arm();goodix_receive_pack(&dev,wrong,28);assert(!event_calls);goodix_receive_pack(&dev,ack,10);assert(!event_calls);goodix_receive_pack(&dev,event,28);assert(event_calls==1&&!event_errors);puts("PASS: manual36 event cannot release a34 waiter");
 reset();arm();goodix_receive_pack(&dev,down,28);assert(event_calls==1&&event_errors==1);puts("PASS: DOWN IRQ0002 cannot count as release");
 reset();arm();goodix_receive_pack(&dev,bad,28);assert(event_calls==1&&event_errors==1);puts("PASS: bad event checksum fails closed");
 reset();arm();guint8 short_event[27];packet(short_event,0x34,event+7,19);goodix_receive_pack(&dev,short_event,27);assert(event_calls==1&&event_errors==1);puts("PASS: event body must be exactly20 bytes");
 reset();arm();receipt[1]=3;packet(ack,0xb0,receipt,2);goodix_receive_pack(&dev,ack,10);assert(command_errors==1&&event_errors==1&&private_state.configuration_missing);receipt[1]=1;packet(ack,0xb0,receipt,2);puts("PASS: ACK03 invalidates arm and event waiter");
 reset();arm();goodix_receive_pack(&dev,ack,10);receipt[0]=0x36;receipt[1]=3;packet(joined,0xb0,receipt,2);goodix_receive_pack(&dev,joined,10);assert(event_errors==1&&private_state.configuration_missing);puts("PASS: configuration loss after ACK still invalidates pending event");
 reset();arm();private_state.read_loop_generation++;goodix_receive_pack(&dev,event,28);assert(!event_calls&&!private_state.fdt_up_received);puts("PASS: event from a different read generation cannot release");
 reset();arm();goodix_fdt_up_timeout_cb(&dev,NULL);assert(event_calls==1&&event_errors==1);puts("PASS: bounded event timeout never synthesizes release");
 reset();arm();memcpy(joined,event,28);memcpy(joined+28,ack,10);memcpy(joined+38,bad,28);goodix_receive_pack(&dev,joined,66);assert(event_calls==1&&event_errors==1);puts("PASS: corrupt full trailing event cannot be hidden by an earlier coalesced ACK");
 reset();arm();goodix_wait_fdt_up(&dev,1000,event_done,&dev,event_destroy);assert(event_errors==1&&private_state.fdt_up_callback);goodix_receive_pack(&dev,ack,10);goodix_receive_pack(&dev,event,28);assert(event_calls==2&&event_errors==1);puts("PASS: duplicate registration rejects its own context without consuming the owner");
 reset();arm();goodix_receive_pack(&dev,ack,10);replace_on_event=TRUE;receipt[0]=0x90;receipt[1]=1;memcpy(joined,event,28);packet(joined+28,0xb0,receipt,2);goodix_receive_pack(&dev,joined,38);assert(event_calls==1&&!event_errors&&command_calls==1&&private_state.ack&&private_state.cmd==0x90);puts("PASS: event completion cannot attribute trailing old bytes to a reentrant successor");
 reset();goodix_wait_fdt_up(&dev,0,event_done,&dev,event_destroy);assert(event_errors==1);goodix_wait_fdt_up(&dev,30001,event_done,&dev,event_destroy);assert(event_errors==2);puts("PASS: unbounded or invalid event deadline is rejected");
 reset();return 0;}
