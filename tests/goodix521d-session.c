/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include "openchicago.h"
#include "goodix-chicago-calibration.h"
#include "goodix-chicago-runtime.h"
#include <string.h>
gboolean __wrap_goodix_chicago_runtime_preprocess(GoodixChicagoPreprocessor *p,const guint16 *raw,gboolean enroll,GoodixChicagoRuntimeFrame *f) {
 (void)p;(void)raw;(void)enroll;memset(f,0,sizeof *f);f->quality=80;f->coverage=100;f->context[4]=100;
 for(guint y=0;y<64;y++)for(guint x=0;x<80;x++)f->enhanced[y*80+x]=((x/5+y/5)%2)?240:10;return TRUE;
}
int main(void) {
 guint16 base[OC_FRAME_PIXELS];for(guint i=0;i<OC_FRAME_PIXELS;i++)base[i]=1000+(i%7);
 GError *e=NULL;OcSession *a=oc_session_new_for_profile(base,GOODIX_CHICAGO_PROFILE_GF5298,&e);g_assert_no_error(e);g_assert_nonnull(a);g_assert_cmpint(oc_session_get_profile(a),==,17);
 GBytes *state=oc_session_save_state(a);OcSession *b=oc_session_new_from_state_for_profile(state,GOODIX_CHICAGO_PROFILE_GF5298,&e);g_assert_no_error(e);g_assert_nonnull(b);
 GBytes *again=oc_session_save_state(b);g_assert_true(g_bytes_equal(state,again));g_bytes_unref(again);
 OcSession *wrong=oc_session_new_from_state(state,&e);g_assert_null(wrong);g_assert_nonnull(e);g_clear_error(&e);
 g_assert_true(oc_enroll_begin(a,OC_ENROLL_ENGINE,OC_ENGINE_STAGES,&e));g_assert_no_error(e);g_assert_false(oc_enroll_is_complete(a));
 GBytes *partial=oc_enroll_finish_complete(a,&e);g_assert_null(partial);g_assert_nonnull(e);g_clear_error(&e);
 g_assert_true(oc_enroll_begin(a,OC_ENROLL_PLAIN,1,&e));g_assert_no_error(e);OcEnrollResult er;
 g_assert_true(oc_enroll_add(a,base,&er,&e));g_assert_no_error(e);g_assert_true(er.complete);g_assert_true(oc_enroll_is_complete(a));
 partial=oc_enroll_finish_complete(a,&e);g_assert_no_error(e);g_assert_nonnull(partial);g_bytes_unref(partial);g_assert_false(oc_enroll_is_complete(a));
 base[0]++;g_assert_true(oc_session_rebase(a,base,&e));g_assert_no_error(e);
 oc_session_free(a);oc_session_free(b);g_bytes_unref(state);
 a=oc_session_new(base,&e);g_assert_no_error(e);g_assert_cmpint(oc_session_get_profile(a),==,24);
 state=oc_session_save_state(a);b=oc_session_new_from_state(state,&e);g_assert_no_error(e);g_assert_nonnull(b);
 wrong=oc_session_new_from_state_for_profile(state,GOODIX_CHICAGO_PROFILE_GF5298,&e);g_assert_null(wrong);g_assert_nonnull(e);g_clear_error(&e);
 oc_session_free(a);oc_session_free(b);g_bytes_unref(state);return 0;
}
