/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include <string.h>
#include "fpi-context.h"
#include "fpi-device.h"
#include "fpi-print.h"
#include "fp-print.h"
#include "gx-print-envelope.h"
#include "goodix-chicago-enrollment.h"
int main(void) {
 GError *e=NULL;GoodixChicagoEnrollment *t=goodix_chicago_enrollment_new();GoodixChicagoFeatureRecord r={.foreground=1,.refined_x=20,.refined_y=25,.strength=200};GoodixChicagoEnrollmentResult result;GoodixChicagoMetricData metrics={0};
 g_assert_true(goodix_chicago_enrollment_insert_first(t,&r,1,1,70,90,&metrics,&result,&e));g_assert_no_error(e);GBytes *payload=goodix_chicago_enrollment_pack(t,&e);g_assert_no_error(e);
 GxPrintIdentity identity={.backend="openchicago",.backend_version=1,.profile=17,.preprocess_version=1,.sensor_id={1},.config_digest={2}};
 GVariant *v=gx_print_envelope_export(&identity,FALSE,payload,&e);g_assert_null(v);g_assert_nonnull(e);g_clear_error(&e);
 v=gx_print_envelope_export(&identity,TRUE,payload,&e);g_assert_no_error(e);g_assert_nonnull(v);GBytes *restored=gx_print_envelope_import(v,&identity,&e);g_assert_no_error(e);g_assert_true(g_bytes_equal(payload,restored));g_bytes_unref(restored);
 /* Exercise the public persistence format used by fprintd, not just GVariant. */
 GType driver=G_TYPE_INVALID;GArray *types=fpi_get_driver_types();
 for(guint i=0;i<types->len;i++) {GType type=g_array_index(types,GType,i);FpDeviceClass *klass=g_type_class_ref(type);if(!strcmp(klass->id,"goodix521d"))driver=type;g_type_class_unref(klass);}
 g_array_unref(types);g_assert_true(driver!=G_TYPE_INVALID);
 FpDevice *device=g_object_new(driver,NULL);FpPrint *print=fp_print_new(device);
 fpi_print_set_type(print,FPI_PRINT_RAW);g_object_set(print,"fpi-data",v,NULL);
 guint8 *serialized=NULL;gsize serialized_length=0;
 g_assert_true(fp_print_serialize(print,&serialized,&serialized_length,&e));g_assert_no_error(e);
 FpPrint *reloaded=fp_print_deserialize(serialized,serialized_length,&e);g_assert_no_error(e);g_assert_nonnull(reloaded);
 GVariant *print_data=NULL;g_object_get(reloaded,"fpi-data",&print_data,NULL);
 restored=gx_print_envelope_import(print_data,&identity,&e);g_assert_no_error(e);g_assert_true(g_bytes_equal(restored,payload));
 g_bytes_unref(restored);g_variant_unref(print_data);g_free(serialized);g_object_unref(reloaded);g_object_unref(print);g_object_unref(device);
 for(guint i=0;i<6;i++){GxPrintIdentity wrong=identity;if(i==0)wrong.profile=24;if(i==1)wrong.backend="sigfm";if(i==2)wrong.backend_version++;if(i==3)wrong.preprocess_version++;if(i==4)wrong.sensor_id[0]++;if(i==5)wrong.config_digest[0]++;restored=gx_print_envelope_import(v,&wrong,&e);g_assert_null(restored);g_assert_nonnull(e);g_clear_error(&e);}
 GBytes *encoded=g_variant_get_data_as_bytes(v);GVariant *decoded=g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE(GX_PRINT_ENVELOPE_TYPE),encoded,FALSE));
 restored=gx_print_envelope_import(decoded,&identity,&e);g_assert_no_error(e);g_assert_true(g_bytes_equal(payload,restored));g_bytes_unref(restored);g_variant_unref(decoded);g_bytes_unref(encoded);
 GVariant *bad_native=g_variant_ref_sink(g_variant_new("(usuuu@ay@ay@ay)",1u,"openchicago",1u,17u,1u,g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,identity.sensor_id,16,1),g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,identity.config_digest,32,1),g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,"bad",3,1)));
 restored=gx_print_envelope_import(bad_native,&identity,&e);g_assert_null(restored);g_assert_nonnull(e);g_clear_error(&e);g_variant_unref(bad_native);
 GVariant *wrong=g_variant_ref_sink(g_variant_new_string("not a template"));restored=gx_print_envelope_import(wrong,&identity,&e);g_assert_null(restored);g_assert_nonnull(e);g_clear_error(&e);g_variant_unref(wrong);
 GBytes *bad=g_bytes_new_static("bad",3);wrong=gx_print_envelope_export(&identity,TRUE,bad,&e);g_assert_null(wrong);g_assert_nonnull(e);g_clear_error(&e);g_bytes_unref(bad);
 g_variant_unref(v);g_bytes_unref(payload);goodix_chicago_enrollment_free(t);return 0;
}
