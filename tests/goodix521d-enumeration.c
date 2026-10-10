/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual FpContext discovery over a JSON-backed GUsbContext. The GInitable
 * USB initializer is deliberately never called: no libusb enumeration/open. */
#include "fprint.h"
#include <gusb.h>
#include <stdarg.h>
#include <json-glib/json-glib.h>
GUsbContext *__wrap_g_usb_context_new(GError **error) {
 (void)error;return g_object_new(G_USB_TYPE_CONTEXT,NULL);
}
void __wrap_g_usb_context_set_debug(GUsbContext *context,GLogLevelFlags flags) {(void)context;(void)flags;}
void __wrap_g_usb_context_enumerate(GUsbContext *context) {
 const char *fixture="{\"UsbDevices\":[{\"PlatformId\":\"usb:FF:FF:01\",\"IdVendor\":10182,\"IdProduct\":21021,\"Device\":1,\"USB\":512}]}";
 g_autoptr(JsonParser) parser=json_parser_new();g_autoptr(GError) error=NULL;
 g_assert_true(json_parser_load_from_data(parser,fixture,-1,&error));g_assert_no_error(error);
 g_assert_true(g_usb_context_load(context,json_node_get_object(json_parser_get_root(parser)),&error));g_assert_no_error(error);
}
/* FpContext's selection and object construction are real. Probe completion
 * is mocked because the upstream generic probe writes USB wakeup sysfs. */
void __wrap_g_async_initable_new_async(GType type,gint priority,GCancellable *cancel,
 GAsyncReadyCallback callback,gpointer user,const gchar *first,...) {
 va_list args;va_start(args,first);GObject *object=g_object_new_valist(type,first,args);va_end(args);
 GTask *task=g_task_new(object,cancel,callback,user);g_task_set_priority(task,priority);
 g_task_return_pointer(task,object,g_object_unref);g_object_unref(task);
}
GObject *__wrap_g_async_initable_new_finish(GAsyncInitable *self,GAsyncResult *result,GError **error) {
 (void)self;return g_task_propagate_pointer(G_TASK(result),error);
}
int main(void) {
 g_setenv("FP_DRIVERS_ALLOWLIST","goodix521d",TRUE);
 g_autoptr(FpContext) context=fp_context_new();
 fp_context_enumerate(context);
 GPtrArray *devices=fp_context_get_devices(context);
 g_assert_cmpuint(devices->len,==,1);
 FpDevice *device=g_ptr_array_index(devices,0);
 g_assert_cmpstr(fp_device_get_driver(device),==,"goodix521d");
 g_assert_cmpuint(fp_device_get_nr_enroll_stages(device),==,12);
 g_assert_false(fp_device_is_open(device));
 return 0;
}
