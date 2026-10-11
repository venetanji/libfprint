/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "drivers_api.h"
#include "goodix-profile.h"
typedef struct _GoodixAcquisition GoodixAcquisition;
typedef enum { GOODIX_ACQUIRED_IMAGE, GOODIX_ACQUIRED_REFRESH_REQUIRED } GoodixAcquiredOutcome;
typedef void (*GoodixAcquired) (FpDevice *device, const GxImage *image, GoodixAcquiredOutcome outcome, gpointer user_data, GError *error);
GoodixAcquisition *goodix_acquisition_new (FpDevice *device, const guint8 factory[64]);
void goodix_acquisition_free (GoodixAcquisition *self);
/* Logging context only; set while idle, before start. */
void goodix_acquisition_set_timing_context (GoodixAcquisition *self, guint64 action_id, guint contact);
void goodix_acquisition_start (GoodixAcquisition *self, gboolean reference, GoodixAcquired callback, gpointer user_data);
void goodix_acquisition_cancel (GoodixAcquisition *self);

/* Invalidate all detector state before reset/teardown or on ownership loss. */
void goodix_acquisition_invalidate_context (GoodixAcquisition *self);

/* Main-context commit only after successful algorithm init/rebase and ownership checks. */
gboolean goodix_acquisition_commit_reference (GoodixAcquisition *self);
