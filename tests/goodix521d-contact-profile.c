/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "goodix-profile.h"
#include <string.h>
int main(void) {
 guint8 otp[64]={0},raw[16]={0},seed[16],out[40],manual[27],a[16]={0},b[16]={0};
 const guint16 input[8]={0,1,2,0x1fe,0x200,0xffff,0x202,0x8000};
 const guint16 expected[8]={0,0,0x101,0xffff,0x100,0x7eff,0x201,0x4000};
 for(guint i=0;i<8;i++){raw[2*i]=input[i];raw[2*i+1]=input[i]>>8;}
 g_assert_cmpint(gx_fdt_seed(raw,16,seed),==,GX_OK);
 for(guint i=0;i<8;i++)g_assert_cmpuint(seed[2*i]|seed[2*i+1]<<8,==,expected[i]);
 otp[23]=0x6f;otp[26]=40;otp[33]=41;otp[40]=0x2d;
 otp[41]=42;otp[42]=43;otp[43]=44;otp[44]=45;otp[45]=46;otp[46]=47;
 GxFields fields;g_assert_cmpint(gx_fields(otp,64,&fields),==,GX_OK);
 for(guint mode=0;mode<2;mode++)for(guint phase=0;phase<2;phase++){
  otp[23]=(otp[23]&127)|(mode<<7);
  g_assert_cmpint(gx_down_request(otp,seed,16,phase,out),==,GX_OK);
  g_assert_cmpuint(out[0],==,0x9c);g_assert_cmpuint(out[1],==,1);
  for(guint i=0;i<4;i++){
   g_assert_cmpuint(out[2+2*i]|out[3+2*i]<<8,==,fields.fdt[i]);
   g_assert_cmpuint(out[30+2*i]|out[31+2*i]<<8,==,fields.image[i]);
  }
  g_assert_cmpmem(out+10,16,seed,16);g_assert_cmpuint(out[26],==,phase);
  g_assert_cmpuint(out[27],==,0);g_assert_cmpuint(out[28],==,mode?5:10);
  g_assert_cmpuint(out[29],==,3);g_assert_cmpuint(out[38],==,3);g_assert_cmpuint(out[39],==,0);
  g_assert_cmpint(gx_contact_manual_request(otp,seed,16,phase,manual),==,GX_OK);
  g_assert_cmpuint(manual[0],==,0x0d);g_assert_cmpuint(manual[1],==,1);
  g_assert_cmpmem(manual+2,8,out+2,8);g_assert_cmpmem(manual+10,16,seed,16);
  g_assert_cmpuint(manual[26],==,phase);
 }
 gboolean equal=FALSE; b[0]=17;
 g_assert_cmpint(gx_preburst_fdt_equal(a,b,8,&equal),==,GX_OK);g_assert_true(equal);
 b[0]=18;g_assert_cmpint(gx_preburst_fdt_equal(a,b,8,&equal),==,GX_OK);g_assert_false(equal);
 g_assert_cmpint(gx_down_request(otp,seed,15,0,out),==,GX_LENGTH);
 for(guint i=0;i<40;i++)g_assert_cmpuint(out[i],==,0);
 g_assert_cmpint(gx_down_request(otp,seed,16,2,out),==,GX_PROFILE);
 g_assert_cmpint(gx_fdt_seed(raw,15,seed),==,GX_LENGTH);
 return 0;
}
