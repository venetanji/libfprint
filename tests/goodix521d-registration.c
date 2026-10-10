/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fpi-context.h"
#include "fpi-device.h"
#include "fpi-image-device.h"
int main(void)
{
  g_autoptr(GArray) types=fpi_get_driver_types();
  guint owners=0;
  for(guint i=0;i<types->len;i++) {
    GType type=g_array_index(types,GType,i);
    g_autoptr(FpDeviceClass) klass=g_type_class_ref(type);
    if(klass->type!=FP_DEVICE_TYPE_USB)continue;
    for(const FpIdEntry *id=klass->id_table;id->vid;id++) if(id->vid==0x27c6 && id->pid==0x521d) {
      owners++;
      g_assert_cmpstr(klass->id,==,"goodix521d");
      g_assert_false(g_type_is_a(type,FP_TYPE_IMAGE_DEVICE));
      g_assert_nonnull(klass->open);g_assert_nonnull(klass->close);
      g_assert_nonnull(klass->enroll);g_assert_nonnull(klass->verify);g_assert_nonnull(klass->identify);g_assert_nonnull(klass->cancel);
      g_assert_cmpint(klass->nr_enroll_stages,==,12);
      g_assert_true(klass->features & FP_DEVICE_FEATURE_VERIFY);
      g_assert_true(klass->features & FP_DEVICE_FEATURE_IDENTIFY);
    }
  }
  g_assert_cmpuint(owners,==,1);
  return 0;
}
