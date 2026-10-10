/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "goodix-timing.h"
static void elapsed(void) {
  GxTiming t;
  gx_timing_begin(&t,7,1000);
  g_assert_cmpuint(t.action_id,==,7);
  g_assert_cmpint(gx_timing_mark(&t,1200),==,200);
  g_assert_cmpint(gx_timing_mark(&t,1500),==,300);
  g_assert_cmpint(gx_timing_total(&t,1500),==,500);
}
static void repeated(void) {
  GxTiming t;
  gx_timing_begin(&t,1,1000);
  gx_timing_mark(&t,2000);
  gx_timing_begin(&t,2,3000);
  g_assert_cmpuint(t.action_id,==,2);
  g_assert_cmpint(gx_timing_total(&t,3000),==,0);
  g_assert_cmpint(gx_timing_mark(&t,3100),==,100);
}
static void backwards(void) {
  GxTiming t;
  gx_timing_begin(&t,3,1000);
  g_assert_cmpint(gx_timing_mark(&t,900),==,0);
  g_assert_cmpint(gx_timing_mark(&t,1100),==,100);
  g_assert_cmpint(gx_timing_total(&t,900),==,0);
}
int main(int argc,char **argv) {
  g_test_init(&argc,&argv,NULL);
  g_test_add_func("/goodix/timing/elapsed",elapsed);
  g_test_add_func("/goodix/timing/repeated",repeated);
  g_test_add_func("/goodix/timing/backwards",backwards);
  return g_test_run();
}
