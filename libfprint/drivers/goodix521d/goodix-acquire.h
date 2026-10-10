/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "drivers_api.h"
#include "goodix-profile.h"
typedef struct _GoodixAcquisition GoodixAcquisition;
typedef void (*GoodixAcquired) (FpDevice *device, const GxImage *image, gpointer user_data, GError *error);
GoodixAcquisition *goodix_acquisition_new (FpDevice *device, const guint8 factory[64]);
void goodix_acquisition_free (GoodixAcquisition *self);
/* Logging context only; set while idle, before start. */
void goodix_acquisition_set_timing_context (GoodixAcquisition *self, guint64 action_id, guint contact);
void goodix_acquisition_start (GoodixAcquisition *self, gboolean reference, GoodixAcquired callback, gpointer user_data);
void goodix_acquisition_cancel (GoodixAcquisition *self);
