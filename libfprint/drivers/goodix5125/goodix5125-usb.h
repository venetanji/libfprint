/*
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "drivers_api.h"
#include "goodix5125-proto.h"

#define GOODIX5125_USB_EP_OUT 0x01
#define GOODIX5125_USB_EP_IN 0x81

/* Unrelated messages tolerated before a command's own reply or ACK. */
#define GOODIX5125_MAX_RX_MESSAGES 16
/* Wait for the ACK of a command without a reply; on timeout the ACK is
 * taken as lost (the command has been executed). */
#define GOODIX5125_ACK_ONLY_TIMEOUT_MS 1000

gboolean goodix5125_usb_in_check_empty (FpiUsbTransfer *transfer,
                                        GError        **error);
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
