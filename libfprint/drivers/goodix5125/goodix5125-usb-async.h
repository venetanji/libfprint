/*
 * Goodix 27c6:5125 asynchronous USB building blocks
 * Copyright (C) 2026 Goodix 5125 driver contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "drivers_api.h"
#include "goodix5125-proto.h"

G_BEGIN_DECLS

/*
 * All finish functions may be called from their corresponding
 * GAsyncReadyCallback.  Pass fpi_device_get_cancellable() for operations that
 * may wait for a finger or image; timeout_ms == 0 is accepted only when a
 * GCancellable is supplied, so an unbounded USB wait always remains
 * interruptible.
 */

void     goodix5125_usb_outer_write_async (FpDevice           *device,
                                           Goodix5125OuterFlag flag,
                                           const guint8       *payload,
                                           gsize               payload_length,
                                           guint               timeout_ms,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data);
gboolean goodix5125_usb_outer_write_finish (FpDevice     *device,
                                            GAsyncResult *result,
                                            GError      **error);

void     goodix5125_usb_outer_read_async (FpDevice           *device,
                                          Goodix5125OuterFlag expected_flag,
                                          guint               timeout_ms,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data);
GBytes  *goodix5125_usb_outer_read_finish (FpDevice     *device,
                                           GAsyncResult *result,
                                           GError      **error);

void     goodix5125_usb_command_async (FpDevice              *device,
                                       Goodix5125Command      command,
                                       Goodix5125ChecksumMode request_checksum_mode,
                                       Goodix5125ChecksumMode reply_checksum_mode,
                                       const guint8          *payload,
                                       gsize                  payload_length,
                                       gboolean               expect_reply,
                                       Goodix5125OuterFlag    reply_flag,
                                       guint                  timeout_ms,
                                       GCancellable          *cancellable,
                                       GAsyncReadyCallback    callback,
                                       gpointer               user_data);
GBytes  *goodix5125_usb_command_finish (FpDevice     *device,
                                        GAsyncResult *result,
                                        GError      **error);

G_END_DECLS
