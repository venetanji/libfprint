// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#define FP_COMPONENT "goodix521d-transport"
#include "fpi-log.h"
#include "fpi-ssm.h"
#include "fpi-usb-transfer.h"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"
#include "../goodix-profile.h"
#include "goodix-candidate-diagnostic.h"

typedef struct {
    GoodixTlsServer* tls_hop;
    guint8 tls_psk[32];
    gboolean tls_psk_set;
    gboolean cancelling;
    FpiSsm *tls_ssm;
    guint64 tls_generation;
    GoodixDefaultCallback fdt_up_callback;
    gpointer fdt_up_user_data;
    GDestroyNotify fdt_up_destroy;
    GSource *fdt_up_timeout;
    guint64 fdt_up_generation, fdt_up_operation, fdt_up_read_loop;
    gboolean fdt_up_acknowledged, fdt_up_received, fdt_up_started;
    guint fdt_up_phases_sent, fdt_up_acks;
    guint8 fdt_up_event[20];

    GSource* timeout;

    guint8 cmd;

    gboolean ack;
    gboolean reply;

    GoodixCmdCallback callback;
    gpointer user_data;
    guint64 operation_generation, configuration_generation;
    gboolean checksum_bypass, ack_additive, command_received, configuration_missing, configuration_conflict;
    guint8 ack_echo, ack_flags, ack_checksum;
    gboolean first_error_logged;
    gint rx_endpoint;
    gssize rx_requested,rx_actual;

    guint8* data;
    guint32 length;

    GoodixCallbackInfo* tls_ready_callback;

    GCancellable* transfer_cancel_tkn;
    guint64 read_loop_generation;
    gboolean read_pending;
    guint read_outstanding;
    gboolean inited;
} FpiDeviceGoodixTlsPrivate;

typedef struct {
    FpDevice *dev;
    GCancellable *cancel;
    guint64 loop;
} GoodixReadOwner;

static void goodix_read_owner_free(GoodixReadOwner *owner)
{
    g_object_unref(owner->cancel);
    g_object_unref(owner->dev);
    g_free(owner);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GoodixReadOwner, goodix_read_owner_free)

static gboolean goodix_read_owner_current(FpiDeviceGoodixTlsPrivate *priv,
                                           const GoodixReadOwner *owner)
{
    return owner && priv->inited && owner->loop == priv->read_loop_generation &&
           owner->cancel == priv->transfer_cancel_tkn;
}

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(FpiDeviceGoodixTls, fpi_device_goodixtls,
                                    FP_TYPE_DEVICE);

/* Upstream's timeout factory may use a cached main-context timestamp. A
 * synchronous USB/TLS operation can have made it stale before we get here. */
static GSource *goodix_add_timeout(FpDevice *dev, guint timeout_ms)
{
    GSource *source = fpi_device_add_timeout(dev, timeout_ms,
                                            goodix_receive_timeout_cb, NULL, NULL);
    g_source_set_ready_time(source, g_get_monotonic_time() + (gint64) timeout_ms * 1000);
    return source;
}

static void goodix_stop_read_loop(FpDevice *dev)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    /* Invalidate before cancellation; its completion may be delivered later. */
    priv->inited = FALSE;
    priv->read_pending = FALSE;
    priv->read_loop_generation++;
    if (priv->transfer_cancel_tkn) g_cancellable_cancel(priv->transfer_cancel_tkn);
}

// TODO remove every GDestroyNotify
// TODO add cmd timeouts

gchar *data_to_str(guint8 *data, guint32 length) {
  gchar *string = g_malloc((length * 2) + 1);

  for (guint32 i = 0; i < length; i++) sprintf(string + i * 2, "%02x", data[i]);

  return string;
}

// ---- GOODIX RECEIVE SECTION START ----

static void candidate_diagnostic(FpDevice *dev,GError *error,const char *site,FpiUsbTransfer *transfer)
{
    FpiDeviceGoodixTlsPrivate *priv=fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    if(!error || priv->first_error_logged)return;
    priv->first_error_logged=TRUE;
    fp_warn("Candidate first failure: site=%s opcode=0x%02x operation=%" G_GUINT64_FORMAT " domain=%s domain_id=%u code=%d message=%s endpoint=%d requested=%" G_GSSIZE_FORMAT " actual=%" G_GSSIZE_FORMAT,
        site,priv->cmd,priv->operation_generation,error->domain==FP_DEVICE_ERROR?"fp-device-error-quark":candidate_error_domain(error),(guint)error->domain,error->code,candidate_safe_message(error),
        transfer?transfer->endpoint:priv->rx_endpoint,transfer?transfer->length:priv->rx_requested,transfer?transfer->actual_length:priv->rx_actual);
}

/* A narrow asynchronous 0x34 waiter. It is never a catch-all command reply. */
static void goodix_fdt_up_done(FpDevice *dev, GError *error)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    GoodixDefaultCallback callback = priv->fdt_up_callback;
    gpointer user_data = priv->fdt_up_user_data;
    GDestroyNotify destroy = priv->fdt_up_destroy;
    guint8 event[20];
    if (!callback) {g_clear_error(&error);return;}
    memcpy(event, priv->fdt_up_event, sizeof(event));
    priv->fdt_up_callback = NULL;
    priv->fdt_up_user_data = NULL;
    priv->fdt_up_destroy = NULL;
    priv->fdt_up_acknowledged = priv->fdt_up_received = priv->fdt_up_started = FALSE;
    priv->fdt_up_phases_sent = priv->fdt_up_acks = 0;
    priv->fdt_up_generation++;
    /* Completion may start another action. Never feed that successor the tail
     * of the batch owned by this event, even if it uses the same opcode. */
    priv->operation_generation++;
    g_clear_pointer(&priv->data, g_free);
    priv->length = 0;
    if (priv->fdt_up_timeout) g_clear_pointer(&priv->fdt_up_timeout, g_source_destroy);
    callback(dev, error ? NULL : event, error ? 0 : sizeof(event), user_data, error);
    if (destroy) destroy(user_data);
}

static gboolean goodix_fdt_up_current(FpiDeviceGoodixTlsPrivate *priv)
{
    return priv->fdt_up_callback && priv->fdt_up_started &&
           priv->fdt_up_read_loop == priv->read_loop_generation &&
           priv->operation_generation == priv->fdt_up_operation + priv->fdt_up_acks;
}

static void goodix_fdt_up_maybe_complete(FpDevice *dev)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    if (!goodix_fdt_up_current(priv)) return;
    if (priv->configuration_missing)
        goodix_fdt_up_done(dev, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                                   "Acquisition configuration unavailable"));
    else if (priv->fdt_up_acknowledged && priv->fdt_up_received)
        goodix_fdt_up_done(dev, NULL);
}

static void goodix_fdt_up_timeout_cb(FpDevice *dev, gpointer user_data)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    goodix_fdt_up_done(dev, g_error_new(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
        "Finger removal event timed out: phases-sent=%u ack-mask=0x%x event-seen=%u owner-current=%u",
        priv->fdt_up_phases_sent, (1u << priv->fdt_up_acks) - 1,
        (guint)priv->fdt_up_received, (guint)goodix_fdt_up_current(priv)));
}

void goodix_wait_fdt_up(FpDevice *dev, guint timeout_ms, GoodixDefaultCallback callback,
                       gpointer user_data, GDestroyNotify destroy)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    GError *error = NULL;
    if (!callback) {if (destroy) destroy(user_data);return;}
    if (priv->cancelling)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix transport is cancelling");
    else if (priv->fdt_up_callback || priv->ack || priv->reply || priv->timeout)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY, "Goodix operation already active");
    else if (priv->configuration_missing)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Acquisition configuration unavailable");
    else if (!timeout_ms || timeout_ms > 30000)
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid finger removal deadline");
    if (error) {
        callback(dev, NULL, 0, user_data, error);
        if (destroy) destroy(user_data);
        return;
    }
    priv->fdt_up_callback = callback;
    priv->fdt_up_user_data = user_data;
    priv->fdt_up_destroy = destroy;
    priv->fdt_up_generation++;
    priv->fdt_up_operation = priv->operation_generation;
    priv->fdt_up_read_loop = priv->read_loop_generation;
    priv->fdt_up_acknowledged = priv->fdt_up_received = priv->fdt_up_started = FALSE;
    priv->fdt_up_phases_sent = priv->fdt_up_acks = 0;
    memset(priv->fdt_up_event, 0, sizeof(priv->fdt_up_event));
    priv->fdt_up_timeout = fpi_device_add_timeout(dev, timeout_ms,
                                                 goodix_fdt_up_timeout_cb, NULL, NULL);
    g_source_set_ready_time(priv->fdt_up_timeout,
                            g_get_monotonic_time() + (gint64) timeout_ms * 1000);
}

void goodix_receive_done(FpDevice *dev, guint8 *data, guint16 length,
                         GError *error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv = fpi_device_goodixtls_get_instance_private(self);
  GoodixCmdCallback callback = priv->callback;
  gpointer user_data = priv->user_data;
  if (!(priv->ack || priv->reply)) {
    if (error && priv->fdt_up_callback) goodix_fdt_up_done(dev, error);
    else g_clear_error(&error);
    return;
  }
  gboolean fdt_owner = goodix_fdt_up_current(priv) && priv->cmd == 0x34 &&
                       priv->ack && !priv->reply;
  guint64 fdt_generation = priv->fdt_up_generation;
  GError *fdt_error = fdt_owner && error ? g_error_copy(error) : NULL;
  if (fdt_owner && !error) {
    priv->fdt_up_acks++;
    priv->fdt_up_acknowledged = priv->fdt_up_acks == 2;
  }
  candidate_diagnostic(dev,error,"receive-done",NULL);
  goodix_reset_state(dev);
  if (!error) fp_dbg("Completed command: 0x%02x", priv->cmd);
  if (callback) callback(dev, data, length, user_data, error);
  else g_clear_error(&error);
  if (fdt_owner && fdt_generation == priv->fdt_up_generation && priv->fdt_up_callback) {
    if (fdt_error) {goodix_fdt_up_done(dev, fdt_error);fdt_error = NULL;}
    else goodix_fdt_up_maybe_complete(dev);
  }
  g_clear_error(&fdt_error);
}

/* TRUE means an error callback ran and the caller must discard its old batch. */
static gboolean goodix_fdt_up_record(FpDevice *dev, guint8 *payload, guint16 length)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    if (!goodix_fdt_up_current(priv)) return FALSE;
    if (length != 20 || payload[0] != 0 || payload[1] != 2) {
        goodix_receive_done(dev, NULL, 0, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                            "Invalid finger removal event"));
        return TRUE;
    }
    memcpy(priv->fdt_up_event, payload, 20);
    priv->fdt_up_received = TRUE;
    return FALSE;
}

void goodix_receive_none(FpDevice *dev, guint8 *data, guint16 length,
                         gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback)cb_info->callback;

  callback(dev, cb_info->user_data, error);
}

void goodix_receive_default(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixDefaultCallback callback = (GoodixDefaultCallback)cb_info->callback;

  callback(dev, data, length, cb_info->user_data, error);
}

void goodix_receive_success(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  if (length != sizeof(guint8) * 2) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid success reply length: %d", length);
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  callback(dev, data[0] == 0x00 ? FALSE : TRUE, cb_info->user_data, NULL);
}

void goodix_receive_reset(FpDevice *dev, guint8 *data, guint16 length,
                          gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixResetCallback callback = (GoodixResetCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, 0, cb_info->user_data, error);
    return;
  }

  if (length != sizeof(guint8) + sizeof(guint16)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid reset reply length: %d", length);
    callback(dev, FALSE, 0, cb_info->user_data, error);
    return;
  }

  callback(dev, data[0] == 0x00 ? FALSE : TRUE,
           GUINT16_FROM_LE(*(guint16 *)(data + sizeof(guint8))),  // TODO
           cb_info->user_data, NULL);
}

void goodix_receive_preset_psk_read(FpDevice *dev, guint8 *data, guint16 length,
                                    gpointer user_data, GError *error) {
  guint32 psk_len;
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadCallback callback =
      (GoodixPresetPskReadCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (length < sizeof(guint8)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (data[0] != 0x00) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
    return;
  }

  if (length < sizeof(guint8) + sizeof(GoodixPresetPsk)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  GoodixPresetPskResponse* response = (GoodixPresetPskResponse *)(data + sizeof(guint8));
  psk_len = GUINT32_FROM_LE(response->length);
  if (psk_len > length - sizeof(guint8) - sizeof(GoodixPresetPskResponse)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  callback(dev, TRUE,
           GUINT32_FROM_LE(response->flags),
           data + sizeof(guint8) + sizeof(GoodixPresetPskResponse), psk_len,
           cb_info->user_data, NULL);
}

void goodix_receive_firmware_version(FpDevice *dev, guint8 *data,
                                     guint16 length, gpointer user_data,
                                     GError *error) {
  g_autofree gchar *payload = g_malloc(length + sizeof(gchar));
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixFirmwareVersionCallback callback =
      (GoodixFirmwareVersionCallback)cb_info->callback;

  if (error) {
    callback(dev, NULL, cb_info->user_data, error);
    return;
  }

  memcpy(payload, data, length);

  // Some device send the firmware without the null terminator
  payload[length] = 0x00;

  callback(dev, payload, cb_info->user_data, NULL);
}

static void goodix_receive_ack_full(FpDevice *dev, guint8 *data, guint16 length,
                                    gboolean configuration_recorded) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;

  if (length != 2) {
    fp_warn("Invalid ACK length: %d", length);
    return;
  }

  priv->ack_echo = data[0];
  priv->ack_flags = data[1];
  if (data[1] != 0x01 && data[1] != 0x03) {
    fp_warn("Invalid ACK flags: 0x%02x", data[sizeof(guint8)]);
    return;
  }

  cmd = data[0];
  if (priv->ack && priv->cmd == cmd) priv->command_received = TRUE;
  /* The paired APP/IAP reports pre-execution configuration state. Absence
   * of bit 1 cannot revalidate a background invalidated by this event. */
  if (data[1] & 2) {
    if (priv->cmd == 0x90 && (cmd != 0x90 || !priv->ack)) priv->configuration_conflict=TRUE;
    if (!configuration_recorded) {
      priv->configuration_missing = TRUE;
      priv->configuration_generation++;
      fp_dbg("MCU configuration unavailable, generation %" G_GUINT64_FORMAT,
             priv->configuration_generation);
    }
    if (((priv->ack || priv->reply) && (priv->cmd == 0x20 || priv->cmd == 0x32 || priv->cmd == 0x34 || priv->cmd == 0x36)) || priv->fdt_up_callback) {
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Acquisition configuration unavailable"));
      return;
    }
  }

  if (priv->cmd != cmd) {
    fp_warn("Invalid ACK command: 0x%02x", cmd);
    return;
  }

  if (!priv->ack) {
    fp_warn("Didn't excpect an ACK for command: 0x%02x", priv->cmd);
    return;
  }

  priv->command_received = TRUE;
  if (priv->configuration_missing && (cmd == 0x20 || cmd == 0x32 || cmd == 0x34 || cmd == 0x36)) {
    goodix_receive_done(dev, NULL, 0,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "Acquisition configuration unavailable"));
    return;
  }

  if (!priv->reply) {
      G_DEBUG_HERE();
      goodix_receive_done(dev, NULL, 0, NULL);
      return;
  }

  priv->ack = FALSE;
}

void goodix_receive_ack(FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error) {
  goodix_receive_ack_full(dev, data, length, FALSE);
}

static void goodix_receive_protocol_full(FpDevice *dev, guint8 *data,
                                         guint32 length,
                                         gboolean configuration_recorded) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum, valid_null_checksum;

  if (!goodix_decode_protocol(data, length, &cmd, &payload, &payload_len,
                              &valid_checksum, &valid_null_checksum)) {
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                              "Invalid complete protocol framing"));
      return;
  }
  if (cmd == GOODIX_CMD_ACK) {
    priv->ack_additive = valid_checksum;
    priv->ack_checksum = data[length - 1];
    if (payload_len == 2) {
      priv->ack_echo = payload[0]; priv->ack_flags = payload[1];
      fp_dbg("ACK echo=%02x flags=%02x trailer=%02x additive=%d operation=%" G_GUINT64_FORMAT,
             priv->ack_echo, priv->ack_flags, priv->ack_checksum,
             priv->ack_additive, priv->operation_generation);
    }
  }
  gboolean additive_required = cmd == GOODIX_CMD_ACK || cmd == 0x32 || cmd == 0x34 || cmd == 0x36 || cmd == 0xd0 || cmd == 0xd4 || cmd == 0x90;
  if (!valid_checksum && (additive_required || !valid_null_checksum)) {
    goodix_receive_done(dev, NULL, 0,
        g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                            "Invalid protocol checksum"));
    return;
  }
  priv->checksum_bypass = !valid_checksum && valid_null_checksum;

  if (cmd == GOODIX_CMD_ACK) {
      fp_dbg("got ack");
      goodix_receive_ack_full(dev, payload, payload_len, configuration_recorded);
      return;
  }

  if (cmd == 0x34 && goodix_fdt_up_current(priv)) {
    if (goodix_fdt_up_record(dev, payload, payload_len)) return;
    goodix_fdt_up_maybe_complete(dev);
    return;
  }

  if (priv->cmd != cmd) {
    if (priv->cmd == 0x20 && cmd == 0xd0 && payload_len == 2 &&
        payload[0] == 0 && payload[1] == 0) {
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "MCU image rejected: TLS unavailable"));
      return;
    }
    fp_warn("Unmatched protocol command: received=0x%02x expected=0x%02x "
            "ack-pending=%u reply-pending=%u operation=%" G_GUINT64_FORMAT,
            cmd, priv->cmd, (guint) priv->ack, (guint) priv->reply,
            priv->operation_generation);
    return;
  }

  if (!priv->reply) {
    fp_warn("Didn't excpect a reply for command: 0x%02x", priv->cmd);
    return;
  }

  if (priv->ack) {
    goodix_receive_done(dev, NULL, 0,
        g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                            "Command data arrived without its validated ACK"));
    return;
  }
  if (priv->ack) fp_warn("Didn't got ACK for command: 0x%02x", priv->cmd);

  if (cmd == 0x90) {
    if (payload_len != 2 || payload[0] != 1 || payload[1] != 0 || priv->configuration_conflict) {
      goodix_receive_done(dev,NULL,0,g_error_new_literal(FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Configuration completion invalid or concurrent configuration loss"));
      return;
    }
    priv->configuration_missing=FALSE;
  }
  goodix_receive_done(dev, payload, payload_len, NULL);
}

void goodix_receive_protocol(FpDevice *dev, guint8 *data, guint32 length) {
  goodix_receive_protocol_full(dev, data, length, FALSE);
}

static gboolean goodix_batch_configuration(FpDevice *dev, const guint8 *data,
                                            guint32 length,
                                            gboolean *configuration_recorded)
{
  guint32 offset = 0;
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
  gboolean prefix_received=FALSE;
  gboolean own_receipt_seen=FALSE;
  *configuration_recorded = FALSE;
  /* Inspect only the validated prefix of this owned batch. Request completion
   * may free its tail, so session invalidation must precede ordinary callbacks. */
  while (length - offset >= 4) {
    guint8 flags, cmd;
    guint16 payload_len, body_len;
    gboolean outer_valid, additive, bypass;
    g_autofree guint8 *payload = NULL;
    g_autofree guint8 *body = NULL;
    guint32 size = 4u + data[offset + 1] + ((guint32)data[offset + 2] << 8);
    if (size > length - offset) return FALSE;
    if ((data[offset] != GOODIX_FLAGS_MSG_PROTOCOL &&
         data[offset] != GOODIX_FLAGS_TLS && data[offset] != GOODIX_FLAGS_TLS_DATA) ||
        !goodix_decode_pack((guint8 *)data + offset, size, &flags, &payload,
                             &payload_len, &outer_valid) || !outer_valid) {
      if (priv->fdt_up_callback) {
        goodix_receive_done(dev, NULL, 0, g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                                            "Invalid outer framing/checksum"));
        return TRUE;
      }
      return FALSE;
    }
    if (flags == GOODIX_FLAGS_MSG_PROTOCOL) {
      if (!goodix_decode_protocol(payload, payload_len, &cmd, &body, &body_len,
                                   &additive, &bypass)) {
        if (priv->fdt_up_callback) {
          goodix_receive_done(dev, NULL, 0, g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                                              "Invalid complete protocol framing"));
          return TRUE;
        }
        return FALSE;
      }
      gboolean required = cmd == GOODIX_CMD_ACK || cmd == 0x32 || cmd == 0x34 || cmd == 0x36 ||
                          cmd == 0xd0 || cmd == 0xd4 || cmd == 0x90;
      if (!additive && (required || !bypass)) {
        if (priv->fdt_up_callback) {
          goodix_receive_done(dev, NULL, 0, g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                                              "Invalid protocol checksum"));
          return TRUE;
        }
        return FALSE;
      }
      if (cmd == 0x34 && additive && goodix_fdt_up_current(priv)) {
        if (goodix_fdt_up_record(dev, body, body_len)) return TRUE;
      }
      if (cmd == GOODIX_CMD_ACK && additive && body_len == 2 &&
          body[0] == priv->cmd && body[1] == 1 && priv->ack)
        prefix_received=TRUE;
      if (cmd == GOODIX_CMD_ACK && additive && body_len == 2 && body[1] == 3) {
        if (priv->cmd == 0x90 && (body[0] != 0x90 || own_receipt_seen || !priv->ack)) priv->configuration_conflict=TRUE;
        if (!(priv->ack || priv->reply) || (priv->cmd != 0x20 && priv->cmd != 0x32 && priv->cmd != 0x34 && priv->cmd != 0x36)) {
          /* Hoist only session invalidation. Startup receipt/data must retain
           * their wire order, including data that arrived before its ACK. */
          priv->configuration_missing = TRUE;
          priv->configuration_generation++;
          *configuration_recorded = TRUE;
          fp_dbg("Owned configuration event echo=%02x flags=03 trailer=%02x generation=%"
                 G_GUINT64_FORMAT, body[0], payload[payload_len - 1],
                 priv->configuration_generation);
          if (body[0] == priv->cmd) own_receipt_seen=TRUE;
          offset += size;
          continue;
        }
        if (prefix_received) {
          priv->command_received=TRUE;
          fp_dbg("Validated owned prefix ACK receipt for 0x%02x before configuration event",priv->cmd);
        }
        g_clear_pointer(&priv->data, g_free);
        priv->length = 0;
        goodix_receive_protocol(dev, payload, payload_len);
        return TRUE;
      }
      if (cmd == GOODIX_CMD_ACK && additive && body_len == 2 && body[0] == priv->cmd) own_receipt_seen=TRUE;
    }
    offset += size;
  }
  return FALSE;
}

void goodix_receive_pack(FpDevice *dev, guint8 *data, guint32 length) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  const guint32 max_buffer = 2 * (G_MAXUINT16 + 4u);
  guint64 generation = priv->operation_generation;
  if (!length) return;
  if (!data || length > max_buffer - priv->length) {
    g_clear_pointer(&priv->data,g_free);
    priv->length=0;
    goodix_receive_done(dev, NULL, 0,
        g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                            "Receive buffer limit exceeded or null input"));
    return;
  }
  priv->data = g_realloc(priv->data, priv->length + length);
  memcpy(priv->data + priv->length, data, length);
  priv->length += length;

  gboolean configuration_recorded;
  if (goodix_batch_configuration(dev, priv->data, priv->length, &configuration_recorded)) {
    /* Configuration evidence invalidates this acquisition batch; it never
     * attributes ordinary trailing replies to a replacement request. */
    return;
  }

  while (priv->length >= 4) {
    guint8 flags;
    g_autofree guint8 *payload = NULL;
    guint16 payload_len;
    g_autofree guint8 *complete=NULL;
    gboolean valid_checksum;
    guint32 packet_size = 4u + priv->data[1] + ((guint32) priv->data[2] << 8);
    if ((priv->data[0] != GOODIX_FLAGS_MSG_PROTOCOL &&
         priv->data[0] != GOODIX_FLAGS_TLS && priv->data[0] != GOODIX_FLAGS_TLS_DATA) ||
        goodix_calc_checksum(priv->data, 3) != priv->data[3]) {
      g_clear_pointer(&priv->data,g_free);
      priv->length=0;
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                              "Invalid outer framing/checksum"));
      return;
    }
    if (priv->length < packet_size) return;
    if (!goodix_decode_pack(priv->data, packet_size, &flags, &payload,
                            &payload_len, &valid_checksum) || !valid_checksum) {
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                              "Invalid outer packet"));
      return;
    }
    if(flags==GOODIX_FLAGS_TLS_DATA)complete=g_memdup2(priv->data,packet_size);
    /* Consume before invoking callbacks, which can advance the operation. */
    priv->length -= packet_size;
    memmove(priv->data, priv->data + packet_size, priv->length);
    if ((priv->ack || priv->reply) && priv->cmd == GOODIX_CMD_READ_OTP &&
        flags != GOODIX_FLAGS_MSG_PROTOCOL) {
      goodix_receive_done(dev, NULL, 0,
          g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                              "OTP reply arrived on an unexpected outer route"));
      return;
    }
    switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
        fp_dbg("Got protocol msg");
        goodix_receive_protocol_full(dev, payload, payload_len, configuration_recorded);
        break;

    case GOODIX_FLAGS_TLS:
        fp_dbg("Got TLS msg");
        if (!priv->reply || priv->ack ||
            (priv->cmd != GOODIX_CMD_REQUEST_TLS_CONNECTION && priv->cmd != 0))
          goodix_receive_done(dev, NULL, 0,
              g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                  "TLS handshake on an unexpected command or before ACK"));
        else {
          goodix_receive_done(dev, payload, payload_len, NULL);
        }
        break;

    case GOODIX_FLAGS_TLS_DATA:
        fp_dbg("Got TLS data msg");
        gsize offset,cipher_length,capacity;
        if (!priv->reply || priv->ack || priv->cmd != GOODIX_CMD_MCU_GET_IMAGE ||
            priv->configuration_missing || packet_size>G_MAXUINT16 ||
            gx_b2_layout(complete,packet_size,&offset,&cipher_length,&capacity)!=GX_OK) {
          goodix_receive_done(dev, NULL, 0,
              g_error_new_literal(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                  "Invalid image boundary, command, configuration or ACK"));
        } else {
          goodix_receive_done(dev, complete, packet_size, NULL);
        }
        break;

    default:
      fp_warn("Unknown flags: 0x%02x", flags);
      break;
    }
    if (priv->operation_generation != generation) {
      /* Reset discarded this batch before invoking completion. Any buffered
       * bytes now belong to a reentrant request and must not be cleared. */
      return;
    }
  }
  if (!priv->length) g_clear_pointer(&priv->data, g_free);
}

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error) {
    g_autoptr(GoodixReadOwner) owner = user_data;
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    /* Cancellation invalidates the current generation before libusb has
     * delivered its callback. Account for every owned transfer, even stale. */
    g_assert(priv->read_outstanding > 0);
    priv->read_outstanding--;
    if (!goodix_read_owner_current(priv, owner)) {
        g_clear_error(&error);
        return;
    }
    priv->read_pending = FALSE;
    priv->rx_endpoint=transfer->endpoint;priv->rx_requested=transfer->length;priv->rx_actual=transfer->actual_length;
    if (g_cancellable_is_cancelled(owner->cancel)) {
        fp_dbg("transfer cancelled, aborting read loop...");
        g_clear_error(&error);
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix transfer canceled");
    }
  if(error) {
    goodix_stop_read_loop(dev);
    goodix_receive_done(dev,NULL,0,error);
    return;
  }

  goodix_receive_pack(dev, transfer->buffer, transfer->actual_length);

  if (goodix_read_owner_current(priv, owner) && !g_cancellable_is_cancelled(owner->cancel))
    goodix_receive_data(dev);
}

void goodix_receive_timeout_cb(FpDevice *dev, gpointer user_data) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  GError *error = NULL;

  g_set_error(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "Command timed out: 0x%02x", priv->cmd);
  goodix_receive_done(dev, NULL, 0, error);
}

void goodix_start_read_loop(FpDevice* dev)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    if (priv->cancelling) return;
    if (priv->inited && !g_cancellable_is_cancelled(priv->transfer_cancel_tkn)) {
        // Already going
        return;
    }
    goodix_stop_read_loop(dev);
    /* Old submissions retain the old token; never reuse/reset it. */
    g_clear_object(&priv->transfer_cancel_tkn);
    priv->transfer_cancel_tkn = g_cancellable_new();
    priv->inited = TRUE;
    goodix_receive_data(dev);
}

void goodix_receive_data(FpDevice * dev)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsClass* class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    if (!priv->inited || priv->read_pending ||
        g_cancellable_is_cancelled(priv->transfer_cancel_tkn)) return;
    FpiUsbTransfer* transfer = fpi_usb_transfer_new(dev);
    GoodixReadOwner *owner = g_new0(GoodixReadOwner, 1);
    owner->dev = g_object_ref(dev);
    owner->cancel = g_object_ref(priv->transfer_cancel_tkn);
    owner->loop = priv->read_loop_generation;
    priv->read_pending = TRUE;

    transfer->short_is_error = FALSE;

    fpi_usb_transfer_fill_bulk(transfer, class->ep_in,
                                GOODIX_EP_IN_MAX_BUF_SIZE);

    priv->read_outstanding++;
    fpi_usb_transfer_submit(transfer, 0, owner->cancel,
                            goodix_receive_data_cb, owner);
}

typedef struct {
    FpDevice *dev;
    GoodixNoneCallback callback;
    gpointer user_data;
    gint64 deadline;
} GoodixReadIdleWait;

static void goodix_read_idle_wait_free(gpointer data)
{
    GoodixReadIdleWait *wait = data;
    g_object_unref(wait->dev);
    g_free(wait);
}

static gboolean goodix_read_idle_poll(gpointer data)
{
    GoodixReadIdleWait *wait = data;
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(wait->dev));
    GError *error = NULL;

    if (priv->read_outstanding) {
        if (g_get_monotonic_time() < wait->deadline) return G_SOURCE_CONTINUE;
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                   "Goodix USB reads did not drain; USB must remain open");
    }
    wait->callback(wait->dev, wait->user_data, error);
    return G_SOURCE_REMOVE;
}

static void goodix_read_idle_begin(FpDevice *dev, gpointer data)
{
    GSource *source = g_timeout_source_new(10);
    /* fpi_device_add_timeout selects the current action's context but retains a
     * raw device through source finalization. Its bootstrap must finish before
     * a user callback can drop the last device reference. A plain GLib source
     * safely owns the device until its callback and source destruction finish. */
    g_source_set_callback(source, goodix_read_idle_poll, data, goodix_read_idle_wait_free);
    g_source_set_ready_time(source, 0);
    g_source_attach(source, g_source_get_context(g_main_current_source()));
    g_source_unref(source);
}

void goodix_wait_read_idle(FpDevice *dev, guint timeout_ms,
                          GoodixNoneCallback callback, gpointer user_data)
{
    GoodixReadIdleWait *wait;
    g_return_if_fail(callback != NULL);
    wait = g_new0(GoodixReadIdleWait, 1);
    wait->dev = g_object_ref(dev);
    wait->callback = callback;
    wait->user_data = user_data;
    wait->deadline = g_get_monotonic_time() + (gint64) timeout_ms * 1000;
    fpi_device_add_timeout(dev, 0, goodix_read_idle_begin, wait, NULL);
}

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

gboolean goodix_send_data(FpDevice * dev, guint8 * data, guint32 length,
                          GDestroyNotify free_func, GError * *error)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsClass* class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);

    FpiDeviceGoodixTlsPrivate *priv = fpi_device_goodixtls_get_instance_private(self);
    guint64 operation = priv->operation_generation;
    guint64 read_loop = priv->read_loop_generation;
    for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
        if (priv->cancelling || operation != priv->operation_generation ||
            read_loop != priv->read_loop_generation) {
            if (free_func) free_func(data);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Goodix packet send no longer owns the operation");
            return FALSE;
        }
        FpiUsbTransfer* transfer = fpi_usb_transfer_new(dev);
        transfer->short_is_error = TRUE;

        fpi_usb_transfer_fill_bulk_full(transfer, class->ep_out, data + i,
                                        GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

        gboolean trace_nop = priv->cmd == GOODIX_CMD_NOP;
        gint64 started = g_get_monotonic_time();
        gboolean sent = fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, error);
        if (trace_nop)
            fp_info("NOP TX: success=%d elapsed-ms=%" G_GINT64_FORMAT
                    " usb-timeout-ms=%u endpoint=%u requested=%" G_GSSIZE_FORMAT
                    " actual=%" G_GSSIZE_FORMAT,
                    sent, (g_get_monotonic_time()-started)/1000,
                    GOODIX_TIMEOUT, transfer->endpoint, transfer->length, transfer->actual_length);
        if (!sent) {
            candidate_diagnostic(dev,error?*error:NULL,"usb-submit-sync",transfer);
            if (free_func)
                free_func(data);
            fpi_usb_transfer_unref(transfer);
            return FALSE;
        }
        fpi_usb_transfer_unref(transfer);
    }

    if (free_func)
        free_func(data);
    return TRUE;
}

gboolean goodix_send_pack(FpDevice * dev, guint8 flags, guint8 * payload,
                          guint16 length, GDestroyNotify free_func,
                          GError * *error)
{
    guint8* data;
    guint32 data_len;

    goodix_encode_pack(flags, payload, length, TRUE, &data, &data_len);
    if (free_func)
        free_func(payload);

    return goodix_send_data(dev, data, data_len, g_free, error);
}

void goodix_send_protocol(
      FpDevice * dev, guint8 cmd, guint8 * payload, guint16 length,
      GDestroyNotify free_func, gboolean calc_checksum, guint timeout_ms,
      gboolean reply, GoodixCmdCallback callback, gpointer user_data)
  {
  FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate* priv =
      fpi_device_goodixtls_get_instance_private(self);
  GError* error = NULL;
  guint8* data;
  guint32 data_len;
  guint64 operation;

    if (priv->cancelling) {
      if (free_func) free_func(payload);
      if (callback) callback(dev, NULL, 0, user_data,
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix transport is cancelling"));
      return;
    }
    if (priv->fdt_up_callback && (cmd != 0x34 ||
        !payload || length != 40 || priv->fdt_up_phases_sent >= 2 ||
        payload[26] != priv->fdt_up_phases_sent || reply ||
        priv->fdt_up_acks != priv->fdt_up_phases_sent ||
        priv->fdt_up_operation + priv->fdt_up_acks != priv->operation_generation ||
        priv->fdt_up_read_loop != priv->read_loop_generation)) {
      if (free_func) free_func(payload);
      if (callback) callback(dev, NULL, 0, user_data,
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY, "Finger removal owns the transport"));
      return;
    }
    if (priv->configuration_missing && (cmd == 0x20 || cmd == 0x32 || cmd == 0x34 || cmd == 0x36)) {
      if (free_func) free_func(payload);
      if (callback) callback(dev,NULL,0,user_data,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_NOT_INITIALIZED,"Acquisition configuration unavailable"));
      return;
    }
    if (priv->ack || priv->reply || priv->timeout) {
        // A command is already running.
        fp_warn("A command is already running: 0x%02x", priv->cmd);
        if (free_func)
            free_func(payload);
        if (callback) callback(dev, NULL, 0, user_data,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY, "Goodix command already active"));
        return;
    }

    operation = priv->operation_generation;
    fp_dbg("Running command: 0x%02x", cmd);

    if (timeout_ms)
        priv->timeout = goodix_add_timeout(dev, timeout_ms);
    priv->cmd = cmd;
    if (priv->fdt_up_callback) {
      priv->fdt_up_started = TRUE;
      priv->fdt_up_phases_sent++;
    }
    if (cmd == 0x90) {priv->configuration_conflict=FALSE;priv->configuration_missing=TRUE;}
    priv->ack = TRUE;
    priv->reply = reply;
    priv->callback = callback;
    priv->user_data = user_data;

    goodix_encode_protocol(cmd, payload, length, calc_checksum, FALSE,
                           &data, &data_len);
    if (free_func)
        free_func(payload);

    if (!goodix_send_pack(dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len,
                          g_free, &error)) {
        /* Synchronous USB dispatch may have completed this operation reentrantly. */
        if (priv->operation_generation == operation)
            goodix_receive_done(dev, NULL, 0, error);
        else
            g_clear_error(&error);
        return;
    };
}
void goodix_send_nop(FpDevice *dev, GoodixNoneCallback callback,
                     gpointer user_data) {
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
  GoodixNop payload = {.unknown = 0x00000000};
  GoodixCallbackInfo *cb_info = NULL;
  guint64 operation = priv->operation_generation;

  if (priv->ack || priv->reply || priv->timeout) {
    if (callback)
      callback(dev, user_data, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY,
                                                 "NOP requested while command active"));
    return;
  }
  if (callback) {
    cb_info = g_new(GoodixCallbackInfo, 1);
    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;
  }
  /* NOP is send-only. USB retains GOODIX_TIMEOUT; there is no response to time
   * out. A response timer here can dispatch inside synchronous USB submission. */
  goodix_send_protocol(dev, GOODIX_CMD_NOP, (guint8 *)&payload, sizeof(payload),
                       NULL, FALSE, 0, FALSE,
                       callback ? goodix_receive_none : NULL, cb_info);
  /* A reentrant completion/error can already have advanced to another command. */
  if (priv->operation_generation == operation && priv->cmd == GOODIX_CMD_NOP)
    goodix_receive_done(dev, NULL, 0, NULL);
}

void goodix_send_mcu_get_image(FpDevice* dev, guint8* payload, guint16 length, GoodixImageCallback callback,
                               gpointer user_data)
{
    GoodixCallbackInfo* cb_info;

    // I want to remove this static value into the respective sub-drivers

    if (callback) {
        cb_info = malloc(sizeof(GoodixCallbackInfo));

        cb_info->callback = G_CALLBACK(callback);
        cb_info->user_data = user_data;

        goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE, payload,
                             length, NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                             goodix_receive_default, cb_info);
        return;
    }

    goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE,  payload,
                         length, NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_down(FpDevice *dev, guint8 *mode,
                                        guint16 length, gboolean reply,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                         free_func, TRUE, 0, reply, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                       free_func, TRUE, 0, reply, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_up(FpDevice *dev, guint8 *mode,
                                      guint16 length, gboolean reply, GDestroyNotify free_func,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                         free_func, TRUE, 0, reply, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                       free_func, TRUE, 0, reply, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_mode(FpDevice *dev, guint8 *mode,
                                        guint16 length, gboolean reply,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                         free_func, TRUE, 0, reply, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                       free_func, TRUE, 0, reply, NULL, NULL);
}

void goodix_send_nav_0(FpDevice *dev, GoodixDefaultCallback callback,
                       gpointer user_data) {
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_mcu_switch_to_idle_mode(FpDevice *dev, guint8 sleep_time,
                                         GoodixNoneCallback callback,
                                         gpointer user_data) {
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void goodix_send_write_sensor_register(FpDevice *dev, guint16 address,
                                       guint16 value,
                                       GoodixNoneCallback callback,
                                       gpointer user_data) {
  // Only support one address and one value

  GoodixWriteSensorRegister payload = {.multiples = FALSE,
                                       .address = address,
                                       .value = value};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void goodix_send_read_sensor_register(FpDevice *dev, guint16 address,
                                      guint8 length,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data) {
  // Only support one address

  GoodixReadSensorRegister payload = {
      .multiples = FALSE, .address = GUINT16_TO_LE(address), .length = length};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_READ_SENSOR_REGISTER,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_upload_config_mcu(FpDevice *dev, guint8 *config,
                                   guint16 length, GDestroyNotify free_func,
                                   GoodixSuccessCallback callback,
                                   gpointer user_data) {
    GoodixCallbackInfo* cb_info;

    if (callback) {
        cb_info = malloc(sizeof(GoodixCallbackInfo));

        cb_info->callback = G_CALLBACK(callback);
        cb_info->user_data = user_data;

        goodix_send_protocol(dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                             free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                             goodix_receive_success, cb_info);
        return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                       free_func, TRUE, GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void goodix_send_set_powerdown_scan_frequency(FpDevice *dev,
                                              guint16 powerdown_scan_frequency,
                                              GoodixSuccessCallback callback,
                                              gpointer user_data) {
  GoodixSetPowerdownScanFrequency payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, goodix_receive_success, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void goodix_send_enable_chip(FpDevice *dev, gboolean enable,
                             GoodixNoneCallback callback, gpointer user_data) {
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE, NULL,
                       NULL);
}

void goodix_send_reset(FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                       GoodixResetCallback callback, gpointer user_data) {
  // Only support reset sensor

  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_RESET, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_reset, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_RESET, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_firmware_version(FpDevice *dev,
                                  GoodixFirmwareVersionCallback callback,
                                  gpointer user_data) {
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_firmware_version, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_query_mcu_state(FpDevice *dev, GoodixDefaultCallback callback,
                                 gpointer user_data) {
  GoodixQueryMcuState payload = {.unused_flags = 0x55};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_request_tls_connection(FpDevice* dev,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data)
{
    GoodixNone payload = {};
    GoodixCallbackInfo* cb_info;

    if (callback) {
        cb_info = malloc(sizeof(GoodixCallbackInfo));

        cb_info->callback = G_CALLBACK(callback);
        cb_info->user_data = user_data;

        goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                             (guint8*) &payload, sizeof(payload), NULL, TRUE, 0,
                             TRUE, goodix_receive_default, cb_info);
        return;
    }

    goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                         (guint8*) &payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void goodix_send_tls_successfully_established(FpDevice *dev,
                                              GoodixNoneCallback callback,
                                              gpointer user_data) {
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void goodix_send_read_otp(FpDevice* dev, GoodixDefaultCallback callback, gpointer user_data)
{
    if(callback)callback(dev,NULL,0,user_data,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"A6 is disabled in the cached-factory candidate"));
}
void goodix_send_preset_psk_read(FpDevice *dev, guint32 flags, guint16 length,
                                 GoodixPresetPskReadCallback callback,
                                 gpointer user_data) {
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE(flags),
                             .length = GUINT32_TO_LE(length),
                             .offset = GUINT16_TO_LE(0)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_preset_psk_read, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  if (priv->transfer_cancel_tkn || priv->tls_hop || priv->inited) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Goodix transport already initialized");
    return FALSE;
  }
  priv->first_error_logged=FALSE;
  priv->rx_endpoint=-1;priv->rx_requested=priv->rx_actual=-1;
  priv->timeout = NULL;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->data = NULL;
  priv->length = 0;
  priv->transfer_cancel_tkn = g_cancellable_new();

  if (!g_usb_device_claim_interface(fpi_device_get_usb_device(dev),
                                     class->interface, 0, error)) {
    g_clear_object(&priv->transfer_cancel_tkn);
    return FALSE;
  }
  return TRUE;
}
void goodix_reset_state(FpDevice* dev)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    if (priv->timeout)
        g_clear_pointer(&priv->timeout, g_source_destroy);
    priv->operation_generation++;
    g_clear_pointer(&priv->data, g_free);
    priv->length = 0;
    priv->ack = FALSE;
    priv->reply = FALSE;
    priv->callback = NULL;
    priv->user_data = NULL;
}

gboolean goodix_dev_deinit(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  if (priv->timeout) g_clear_pointer(&priv->timeout,g_source_destroy);
  g_clear_pointer(&priv->data,g_free);
  priv->length=0;
  goodix_stop_read_loop(dev);
  if(!goodix_shutdown_tls(dev,error))return FALSE;

  goodix_reset_state(dev);
  g_clear_object(&priv->transfer_cancel_tkn);
  priv->inited = FALSE;
  OPENSSL_cleanse(priv->tls_psk, sizeof(priv->tls_psk));
  priv->tls_psk_set = FALSE;

  return g_usb_device_release_interface(fpi_device_get_usb_device(dev),
                                        class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void goodix_read_tls(FpDevice* dev, GoodixTlsCallback callback,
                     gpointer user_data)
{

    fp_dbg("goodix_read_tls()");
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    priv->callback = callback;
    priv->user_data = user_data;
    priv->reply = TRUE;
    priv->cmd = 0;
    priv->timeout = goodix_add_timeout(dev, GOODIX_TIMEOUT);
}

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  TLS_NUM_STATES,
};

static void candidate_tls_finish(FpDevice *dev, GError *error)
{
    FpiDeviceGoodixTlsPrivate *priv=fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    candidate_diagnostic(dev,error,"tls-finish",NULL);
    GoodixCallbackInfo *info=priv->tls_ready_callback;
    priv->tls_ready_callback=NULL;
    if(info){((GoodixNoneCallback)info->callback)(dev,info->user_data,error);g_free(info);}
    else g_clear_error(&error);
}

static void on_goodix_tls_server_ready(GoodixTlsServer* server, GError* err, gpointer dev)
{
    /* Worker callback does not enter libfprint; main-thread Finished wait owns activation. */
    (void)server; (void)err; (void)dev;
}

static void on_goodix_tls_read_handshake(FpDevice* dev, guint8* data,
                                         guint16 length, gpointer user_data,
                                         GError* error)
{
    //   goodix_tls_handshake_state* state = (goodix_tls_handshake_state*)
    //   user_data;
    FpiSsm* ssm = user_data;
    if (error) {
        fpi_ssm_mark_failed(ssm, error);
        return;
    }
    FpiDeviceGoodixTls* self =
        FPI_DEVICE_GOODIXTLS(fpi_ssm_get_data(user_data));
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    int sent = goodix_tls_client_send(priv->tls_hop, data, length);
    if (sent != length) {
        fpi_ssm_mark_failed(ssm, g_error_new(g_io_error_quark(), sent,
                                             "failed to sent data to "
                                             "tls server"));
        return;
    }
    fpi_ssm_next_state(ssm);
}

enum goodix_tls_handshake_stages {
    TLS_HANDSHAKE_STAGE_HELLO_S,
    TLS_HANDSHAKE_STAGE_KH_EXCHANGE,
    TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_C,
    TLS_HANDSHAKE_STAGE_HANDSHAKE_C,
    TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S,

    TLS_HANDSHAKE_STAGE_NUM,
};

static void on_tls_successfully_established(FpDevice* dev, gpointer user_data, GError* error)
{
    candidate_tls_finish(dev,error);
}
static void tls_handshake_done(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    FpiDeviceGoodixTlsPrivate *priv=fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    if (priv->tls_ssm != ssm) {g_clear_error(&error);return;}
    priv->tls_ssm = NULL;
    priv->tls_generation++;
    if(error){candidate_tls_finish(dev,error);return;}
    if(!goodix_tls_server_wait_authenticated(priv->tls_hop,&error)) {
        candidate_tls_finish(dev,error);return;
    }
    goodix_send_tls_successfully_established(dev,on_tls_successfully_established,NULL);
}

static void tls_handshake_run(FpiSsm* ssm, FpDevice* dev)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    guint64 generation = priv->tls_generation;
    int stage = fpi_ssm_get_cur_state(ssm);
    if (stage == TLS_HANDSHAKE_STAGE_HELLO_S) {
        guint8 buff[1024];
        int size = goodix_tls_client_recv(priv->tls_hop, buff, sizeof(buff));
        if (size <= 0) {
            fpi_ssm_mark_failed(ssm, g_error_new(g_io_error_quark(), size,
                                                 "failed to read tls server "
                                                 "hello"));
            return;
        }
        GError* err = NULL;
        gboolean sent = goodix_send_pack(dev, GOODIX_FLAGS_TLS, buff, size, NULL, &err);
        /* USB submission can dispatch cancellation and free this SSM. */
        if (generation != priv->tls_generation || priv->tls_ssm != ssm) {
            g_clear_error(&err);
            return;
        }
        if (!sent) {
            fpi_ssm_mark_failed(ssm, err);
            return;
        }
        fpi_ssm_next_state(ssm);
    }
    else if (stage < TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S) {
        // Still proxying from hardware
        fpi_ssm_set_data(ssm, dev, NULL);
        goodix_read_tls(dev, on_goodix_tls_read_handshake, ssm);
    }
    else if (stage == TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S) {
        fp_dbg("Reading to proxy back");
        guint8 buff[1024];
        int size = goodix_tls_client_recv(priv->tls_hop, buff, sizeof(buff));
        if (size <= 0) {
            fpi_ssm_mark_failed(ssm, g_error_new(g_io_error_quark(), size,
                                                 "failed to read server "
                                                 "handshake"));

            return;
        }
        GError* err = NULL;
        gboolean sent = goodix_send_pack(dev, GOODIX_FLAGS_TLS, buff, size, NULL, &err);
        /* USB submission can dispatch cancellation and free this SSM. */
        if (generation != priv->tls_generation || priv->tls_ssm != ssm) {
            g_clear_error(&err);
            return;
        }
        if (!sent) {
            fpi_ssm_mark_failed(ssm, err);
            return;
        }
        fpi_ssm_next_state(ssm);
    }
}

static void do_tls_handshake(FpDevice* dev)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    priv->tls_generation++;
    priv->tls_ssm = fpi_ssm_new(dev, tls_handshake_run, TLS_HANDSHAKE_STAGE_NUM);
    fpi_ssm_start(priv->tls_ssm, tls_handshake_done);
}

static void on_goodix_request_tls_connection(FpDevice* dev, guint8* data,
                                             guint16 length, gpointer user_data,
                                             GError* error)
{
    if (error) {
        fp_err("failed to get tls handshake: %s", error->message);
        candidate_tls_finish(dev,error);
        return;
    }
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(user_data);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    if(goodix_tls_client_send(priv->tls_hop,data,length)!=length) {
        candidate_tls_finish(dev,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"TLS client hello forwarding failed"));return;
    }
    do_tls_handshake(dev);
}

static void goodix_tls_ready(GoodixTlsServer* server, GError* err, gpointer dev)
{
    if (err) {
        fp_err("failed to init tls server: %s, code: %d", err->message,
               err->code);
        return;
    }
    goodix_send_request_tls_connection(FP_DEVICE(dev),
                                       on_goodix_request_tls_connection, dev);
}

void goodix_tls_set_psk(FpDevice* dev, const guint8 psk[32])
{
    FpiDeviceGoodixTlsPrivate* priv = fpi_device_goodixtls_get_instance_private(
        FPI_DEVICE_GOODIXTLS(dev));
    memcpy(priv->tls_psk, psk, sizeof(priv->tls_psk));
    priv->tls_psk_set = TRUE;
}

void goodix_tls(FpDevice* dev, GoodixNoneCallback callback, gpointer user_data)
{
    fp_dbg("Starting up goodix tls server");
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    /* A caller must explicitly supply a validated key and a completion owner.
     * An all-zero caller-supplied key remains distinct from an unset key. */
    if (!callback) return;
    if (priv->cancelling) {
        callback(dev, user_data, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "Goodix transport is cancelling"));
        return;
    }
    if (priv->tls_hop || priv->tls_ready_callback) {
        callback(dev, user_data, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY,
                                                    "TLS session already active"));
        return;
    }
    if (!priv->tls_psk_set) {
        callback(dev, user_data, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                                    "Goodix reader key has not been supplied"));
        return;
    }
    priv->tls_hop = g_malloc0(sizeof(GoodixTlsServer));
    memcpy(priv->tls_hop->psk, priv->tls_psk, sizeof(priv->tls_psk));

    if (!priv->tls_ready_callback) {
        priv->tls_ready_callback = malloc(sizeof(GoodixCallbackInfo));
    }
    priv->tls_ready_callback->callback = G_CALLBACK(callback);
    priv->tls_ready_callback->user_data = user_data;
    GoodixTlsServer* s = priv->tls_hop;
    s->connection_callback = on_goodix_tls_server_ready;
    s->user_data = self;
    GError* err = NULL;
    if (!goodix_tls_server_init(priv->tls_hop, &err)) {
        candidate_tls_finish(dev,err);
        return;
    }

    goodix_tls_ready(s, err, self);
}

gboolean goodix_shutdown_tls(FpDevice* dev, GError** error)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);

    if (priv->tls_hop) {
        gboolean rs = goodix_tls_server_deinit(priv->tls_hop, error);
        if(!rs)return FALSE; /* Never free storage still owned by a TLS worker. */
        g_free(priv->tls_hop);
        priv->tls_hop = NULL;
        return rs;
    }
    return TRUE;
}
static void goodix_tls_ready_image_handler(FpDevice* dev, guint8* data,
                                           guint16 length, gpointer user_data,
                                           GError* error)
{

    GoodixCallbackInfo* cb_info = user_data;
    GoodixImageCallback callback = (GoodixImageCallback) cb_info->callback;
    if (error) {
        callback(dev, NULL, 0, cb_info->user_data, error);
        g_free(cb_info);
        return;
    }
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    gsize offset,cipher_length,capacity;
    if(!priv->tls_hop || priv->configuration_missing ||
       gx_b2_layout(data,length,&offset,&cipher_length,&capacity)!=GX_OK ||
       goodix_tls_client_send(priv->tls_hop,data+offset,cipher_length)!=cipher_length) {
        callback(dev,NULL,0,cb_info->user_data,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"Image TLS forwarding or configuration failed"));
        g_free(cb_info);return;
    }
    const guint16 size = 7684;
    g_autofree guint8* buff = g_malloc(size);
    GError* err = NULL;
    int read_size = goodix_tls_server_receive(priv->tls_hop, buff, size, &err);
    GxImage integrity;
    if (read_size != GX_BLOB_SIZE || gx_image_blob(buff,read_size,data+8,1,1,&integrity)!=GX_OK) {
        if(!err)err=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Image plaintext length or CRC failed");
        callback(dev, NULL, 0, cb_info->user_data, err);
        g_free(cb_info);
        return;
    }

    callback(dev, buff, read_size, cb_info->user_data, NULL);
    g_free(cb_info);
}

void goodix_tls_read_image(FpDevice* dev, guint8* payload, guint16 length, GoodixImageCallback callback,
                           gpointer user_data)
{
    g_assert(callback);
    GoodixCallbackInfo* cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_mcu_get_image(dev, payload, length, goodix_tls_ready_image_handler, cb_info);
}

/* Main-context only. Invalidate the read owner before invoking anything that
 * may unwind the parent's SSM. Reject reentrant operations until that finishes. */
void goodix_cancel(FpDevice *dev)
{
    FpiDeviceGoodixTlsPrivate *priv =
        fpi_device_goodixtls_get_instance_private(FPI_DEVICE_GOODIXTLS(dev));
    if (priv->cancelling) return;
    g_object_ref(dev);
    priv->cancelling = TRUE;
    goodix_stop_read_loop(dev);
    if (priv->ack || priv->reply)
        goodix_receive_done(dev, NULL, 0,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix operation cancelled"));
    /* The command callback normally unwinds its TLS SSM. During a direct
     * synchronous TLS send there is no command callback to do so. */
    if (priv->tls_ssm)
        fpi_ssm_mark_failed(priv->tls_ssm,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix TLS cancelled"));
    if (priv->fdt_up_callback)
        goodix_fdt_up_done(dev,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Finger removal cancelled"));
    if (priv->tls_ready_callback)
        candidate_tls_finish(dev,
            g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Goodix TLS cancelled"));
    priv->cancelling = FALSE;
    g_object_unref(dev);
}

// ---- TLS SECTION END ----

static void fpi_device_goodixtls_init(FpiDeviceGoodixTls *self) {}

static void fpi_device_goodixtls_class_init(FpiDeviceGoodixTlsClass *class) {}
