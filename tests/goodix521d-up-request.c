/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "goodix-profile.h"
#include <string.h>
int main(void) {
 guint8 otp[64]={0},down[20]={2,0,5,0},out[40];
 otp[23]=0x6e;otp[26]=40;otp[33]=41;otp[40]=7;
 otp[41]=42;otp[42]=43;otp[43]=44;otp[44]=45;otp[45]=46;otp[46]=47;
 down[4]=100;down[8]=254;down[9]=1;
 g_assert_cmpint(gx_up_request(otp,down,20,0,out),==,GX_OK);
 g_assert_cmpuint(out[0],==,0x8e);g_assert_cmpuint(out[1],==,1);
 g_assert_cmpuint(out[10]|out[11]<<8,==,0x3a3a);
 g_assert_cmpuint(out[12]|out[13]<<8,==,0x0606);
 g_assert_cmpuint(out[14]|out[15]<<8,==,0x0707);
 for(guint i=27;i<40;i++)g_assert_cmpuint(out[i],==,0);
 g_assert_cmpint(gx_up_request(otp,down,20,1,out),==,GX_OK);g_assert_cmpuint(out[26],==,1);
 g_assert_cmpint(gx_up_request(otp,down,19,1,out),==,GX_LENGTH);
 g_assert_cmpint(gx_up_request(otp,down,20,2,out),==,GX_PROFILE);
 return 0;
}
