/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gx-print-envelope.h"
#include "chicago/goodix-chicago-enrollment.h"
#include <string.h>
static gboolean identity_valid(const GxPrintIdentity *id,GError **error) {
 if(!id || !id->backend || strcmp(id->backend,"openchicago") || id->backend_version==0 ||
    (id->profile!=17 && id->profile!=24) || id->preprocess_version==0) {
  g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"unsupported expected matcher identity");return FALSE;
 }
 return TRUE;
}
static gboolean payload_valid(GBytes *payload,GError **error) {
 if(!payload || g_bytes_get_size(payload)==0 || g_bytes_get_size(payload)>GX_PRINT_MAX_PAYLOAD) {
  g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"invalid native template size");return FALSE;
 }
 gsize n;const guint8 *p=g_bytes_get_data(payload,&n);
 g_autoptr(GoodixChicagoEnrollment) parsed=goodix_chicago_enrollment_unpack(p,n,error);
 if(!parsed)return FALSE;
 guint count=goodix_chicago_enrollment_get_count(parsed);
 if(!count){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"empty native enrollment");return FALSE;}
 for(guint i=0;i<count;i++) {
  GoodixChicagoSubtemplateView view;
  if(!goodix_chicago_enrollment_get_subtemplate(parsed,i,&view)||!view.record_count) {
   g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"empty native subtemplate");return FALSE;
  }
 }
 return TRUE;
}
GVariant *gx_print_envelope_export(const GxPrintIdentity *id,gboolean complete,GBytes *payload,GError **error) {
 if(!identity_valid(id,error))return NULL;
 if(!complete){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_INITIALIZED,"enrollment is incomplete");return NULL;}
 if(!payload_valid(payload,error))return NULL;
 gsize n;const guint8 *p=g_bytes_get_data(payload,&n);
 return g_variant_ref_sink(g_variant_new("(usuuu@ay@ay@ay)",GX_PRINT_ENVELOPE_VERSION,id->backend,id->backend_version,id->profile,id->preprocess_version,
  g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,id->sensor_id,sizeof id->sensor_id,1),
  g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,id->config_digest,sizeof id->config_digest,1),
  g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,p,n,1)));
}
GBytes *gx_print_envelope_import(GVariant *data,const GxPrintIdentity *id,GError **error) {
 if(!identity_valid(id,error))return NULL;
 if(!data || !g_variant_is_of_type(data,G_VARIANT_TYPE(GX_PRINT_ENVELOPE_TYPE)) ||
    g_variant_get_size(data)>GX_PRINT_MAX_PAYLOAD+1024u || !g_variant_is_normal_form(data)) {
  g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"invalid matcher envelope");return NULL;
 }
 guint32 version,backend_version,profile,preprocess_version;const gchar *backend;
 g_autoptr(GVariant) sensor=NULL;g_autoptr(GVariant) config=NULL;g_autoptr(GVariant) native=NULL;
 g_variant_get(data,"(u&suuu@ay@ay@ay)",&version,&backend,&backend_version,&profile,&preprocess_version,&sensor,&config,&native);
 gsize sn,cn,pn;const guint8 *sp=g_variant_get_fixed_array(sensor,&sn,1),*cp=g_variant_get_fixed_array(config,&cn,1),*pp=g_variant_get_fixed_array(native,&pn,1);
 if(version!=GX_PRINT_ENVELOPE_VERSION || strcmp(backend,id->backend) || backend_version!=id->backend_version ||
    profile!=id->profile || preprocess_version!=id->preprocess_version || sn!=sizeof id->sensor_id || cn!=sizeof id->config_digest ||
    memcmp(sp,id->sensor_id,sn) || memcmp(cp,id->config_digest,cn)) {
  g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"matcher identity/profile/configuration mismatch");return NULL;
 }
 g_autoptr(GBytes) payload=g_bytes_new(pp,pn);if(!payload_valid(payload,error))return NULL;
 return g_steal_pointer(&payload);
}
