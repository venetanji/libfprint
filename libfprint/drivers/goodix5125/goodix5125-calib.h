/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <glib.h>

#include "goodix5125-proto.h"

#define GOODIX5125_OTP_SIZE 0x40
#define GOODIX5125_FDT_BASE_SIZE 0x0c

typedef struct
{
  guint16 tcode;
  guint8  fdt_delta;
  guint8  fdt_offset;
  guint8  dac[4];
} Goodix5125OtpInfo;

gboolean goodix5125_otp_parse (const guint8      *otp,
                               gsize              otp_length,
                               Goodix5125OtpInfo *info,
                               GError           **error);
gboolean goodix5125_config_build (const Goodix5125OtpInfo *info,
                                  guint8                   config[GOODIX5125_CONFIG_SIZE],
                                  GError                 **error);
