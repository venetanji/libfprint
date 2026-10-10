#include "goodix521d-transport-receiver-shim.h"
#include "receiver.inc"
static FpDevice dev;
static int calls,errors;
static void done(FpDevice*d,guint8*p,guint16 n,gpointer u,GError*e){calls++;errors+=e!=NULL;free(e);}
static void setup(guint8 cmd){free(private_state.data);memset(&private_state,0,sizeof private_state);calls=errors=0;private_state.cmd=cmd;private_state.ack=private_state.reply=1;private_state.callback=done;}
static void packet(guint8 *out,guint8 op,guint8 a,guint8 b){guint8 bytes[]={0xa0,6,0,0xa6,op,3,0,a,b,0};bytes[9]=(guint8)(0xaa-op-3-a-b);memcpy(out,bytes,10);}
int main(void){guint8 ack[10],yes[10],notice[10],joined[30];packet(ack,0xb0,0x90,1);packet(yes,0x90,1,0);packet(notice,0xb0,0x36,3);
setup(0x90);goodix_receive_pack(&dev,yes,10);assert(errors==1);puts("reply without ACK rejected");
setup(0x90);ack[9]^=1;goodix_receive_pack(&dev,ack,10);assert(errors==1);ack[9]^=1;puts("bad ACK checksum rejected");
setup(0x90);memcpy(joined,ack,10);memcpy(joined+10,yes,10);goodix_receive_pack(&dev,joined,20);assert(calls==1&&!errors);puts("coalesced ACK and completion accepted");
setup(0x90);goodix_receive_pack(&dev,ack,3);goodix_receive_pack(&dev,ack+3,7);assert(!calls);goodix_receive_pack(&dev,yes,10);assert(calls==1&&!errors);puts("split ACK and completion accepted");
setup(0x20);goodix_receive_pack(&dev,notice,10);assert(errors==1&&private_state.configuration_missing);puts("configuration loss fails image request");
setup(0x90);memcpy(joined+20,notice,10);goodix_receive_pack(&dev,joined,30);assert(private_state.configuration_missing);puts("coalesced trailing configuration loss retained");
setup(0x90);yes[9]=0x88;goodix_receive_pack(&dev,ack,10);goodix_receive_pack(&dev,yes,10);assert(errors==1);puts("90 null checksum bypass rejected");
setup(0x90);packet(yes,0x90,1,0);packet(notice,0xb0,0x90,3);memcpy(joined,ack,10);memcpy(joined+10,yes,10);memcpy(joined+20,notice,10);goodix_receive_pack(&dev,joined,30);assert(errors==1&&private_state.configuration_missing);puts("trailing same-echo configuration loss rejected");
free(private_state.data);return 0;}
