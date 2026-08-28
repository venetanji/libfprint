/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include "drivers_api.h"
#include "goodix5125-proto.h"

#define GOODIX5125_USB_EP_OUT 0x01
#define GOODIX5125_USB_EP_IN 0x81

gboolean goodix5125_usb_drain_sync (FpDevice *device,
                                    guint     timeout_ms,
                                    GError  **error);
gboolean goodix5125_usb_nop_sync (FpDevice *device,
                                  guint     timeout_ms,
                                  GError  **error);
gboolean goodix5125_usb_outer_write_sync (FpDevice           *device,
                                          Goodix5125OuterFlag flag,
                                          const guint8       *payload,
                                          gsize               payload_length,
                                          guint               timeout_ms,
                                          GError            **error);
GBytes  *goodix5125_usb_outer_read_sync (FpDevice           *device,
                                         Goodix5125OuterFlag expected_flag,
                                         guint               timeout_ms,
                                         GError            **error);
GBytes  *goodix5125_usb_command_sync (FpDevice              *device,
                                      Goodix5125Command      command,
                                      Goodix5125ChecksumMode request_checksum_mode,
                                      Goodix5125ChecksumMode reply_checksum_mode,
                                      const guint8          *payload,
                                      gsize                  payload_length,
                                      gboolean               expect_reply,
                                      Goodix5125OuterFlag    reply_flag,
                                      guint                  timeout_ms,
                                      GError               **error);
