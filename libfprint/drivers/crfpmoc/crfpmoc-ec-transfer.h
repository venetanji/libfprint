/*
 * ChromeOS Fingerprint driver for libfprint
 *
 * Copyright (C) 2026 Marco Trevisan (Treviño) <mail@3v1n0.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

#include "fpi-device.h"

G_BEGIN_DECLS

typedef struct _CrfpMocEcTransfer
{
  FpDevice *device;

  /* For CRFPMOC_CROS_EC_DEV_IOCXCMD_V2 */
  int   command;
  int   version;
  void *outdata;               /* owned */
  int   outsize;
  void *indata;                /* owned, allocated to insize */
  int   insize;

  /* For CRFPMOC_CROS_EC_DEV_IOCEVENTMASK_V2 */
  unsigned long event_mask;

  /* Full raw cmd struct saved if recording is enabled in emulation mode */
  void *raw_cmd;
  gsize raw_cmd_size;
} CrfpMocEcTransfer;

typedef void (*CrfpMocEcTransferCallback) (CrfpMocEcTransfer *transfer,
                                           GAsyncResult      *result,
                                           gpointer           user_data);

CrfpMocEcTransfer *crfpmoc_ec_transfer_new_cmd (FpDevice     *device,
                                                int           command,
                                                int           version,
                                                gconstpointer outdata,
                                                int           outsize,
                                                int           insize);

CrfpMocEcTransfer *crfpmoc_ec_transfer_new_eventmask (FpDevice     *device,
                                                      unsigned long event_mask);

void crfpmoc_ec_transfer_free (CrfpMocEcTransfer *transfer);

void crfpmoc_ec_transfer_submit_cmd (CrfpMocEcTransfer        *transfer,
                                     GCancellable             *cancellable,
                                     CrfpMocEcTransferCallback callback,
                                     gpointer                  user_data);

void crfpmoc_ec_transfer_submit_eventmask (CrfpMocEcTransfer        *transfer,
                                           GCancellable             *cancellable,
                                           CrfpMocEcTransferCallback callback,
                                           gpointer                  user_data);

gboolean crfpmoc_ec_transfer_submit_finish (GAsyncResult *result,
                                            GError      **error);

gboolean crfpmoc_ec_transfer_submit_sync (CrfpMocEcTransfer *transfer,
                                          GError           **error);

void crfpmoc_ssm_ec_transfer_cb (CrfpMocEcTransfer *transfer,
                                 GAsyncResult      *result,
                                 gpointer           user_data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CrfpMocEcTransfer, crfpmoc_ec_transfer_free)

G_END_DECLS
