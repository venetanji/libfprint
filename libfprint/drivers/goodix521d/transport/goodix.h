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

#pragma once

// 1 seconds USB timeout
#define GOODIX_TIMEOUT (1000)

G_DECLARE_DERIVABLE_TYPE(FpiDeviceGoodixTls, fpi_device_goodixtls, FPI,
                         DEVICE_GOODIXTLS, FpDevice)

#define FPI_TYPE_DEVICE_GOODIXTLS (fpi_device_goodixtls_get_type())

struct _FpiDeviceGoodixTlsClass {
  FpDeviceClass parent;

  gint interface;
  guint8 ep_in;
  guint8 ep_out;
};

typedef struct __attribute__((__packed__)) _GoodixCallbackInfo {
  GCallback callback;
  gpointer user_data;
} GoodixCallbackInfo;

typedef void (*GoodixCmdCallback)(FpDevice *dev, guint8 *data, guint16 length,
                                  gpointer user_data, GError *error);

typedef void (*GoodixFirmwareVersionCallback)(FpDevice *dev, gchar *firmware,
                                              gpointer user_data,
                                              GError *error);

typedef void (*GoodixPresetPskReadCallback)(FpDevice *dev, gboolean success,
                                            guint32 flags, guint8 *psk,
                                            guint16 length, gpointer user_data,
                                            GError *error);

typedef void (*GoodixSuccessCallback)(FpDevice *dev, gboolean success,
                                      gpointer user_data, GError *error);

typedef void (*GoodixResetCallback)(FpDevice *dev, gboolean success,
                                    guint16 number, gpointer user_data,
                                    GError *error);

typedef void (*GoodixNoneCallback)(FpDevice *dev, gpointer user_data,
                                   GError *error);

typedef void (*GoodixDefaultCallback)(FpDevice *dev, guint8 *data,
                                      guint16 length, gpointer user_data,
                                      GError *error);
typedef GoodixDefaultCallback GoodixTlsCallback;

typedef void (*GoodixImageCallback)(FpDevice* dev, guint8* data, guint16 length,
                                    gpointer user_data, GError* error);

gchar *data_to_str(guint8 *data, guint32 length);

// ---- GOODIX RECEIVE SECTION START ----

void goodix_receive_done(FpDevice *dev, guint8 *data, guint16 length,
                         GError *error);

void goodix_receive_success(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error);

void goodix_receive_reset(FpDevice *dev, guint8 *data, guint16 length,
                          gpointer user_data, GError *error);

void goodix_receive_none(FpDevice *dev, guint8 *data, guint16 length,
                         gpointer user_data, GError *error);

void goodix_receive_default(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error);

void goodix_receive_preset_psk_read(FpDevice *dev, guint8 *data, guint16 length,
                                    gpointer user_data, GError *error);

void goodix_receive_ack(FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error);

void goodix_receive_firmware_version(FpDevice *dev, guint8 *data,
                                     guint16 length, gpointer user_data,
                                     GError *error);

void goodix_receive_protocol(FpDevice *dev, guint8 *data, guint32 length);

void goodix_receive_pack(FpDevice *dev, guint8 *data, guint32 length);

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error);

void goodix_receive_timeout_cb(FpDevice *dev, gpointer user_data);

void goodix_receive_data(FpDevice *dev);

void goodix_start_read_loop(FpDevice* dev);

/* Call after stopping/cancelling reads, before closing the USB device. Completes
 * asynchronously on the device action's main context, retaining dev until then.
 * Success requires every owned RX completion, including stale generations.
 * Timeout owns no transfer cancellation: the callback owns its error and MUST
 * NOT close USB on failure; pending reads continue retaining their device/token.
 * A zero timeout performs one deferred check. Calls complete exactly once. */
void goodix_wait_read_idle(FpDevice *dev, guint timeout_ms,
                          GoodixNoneCallback callback, gpointer user_data);
// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

gboolean goodix_send_data(FpDevice *dev, guint8 *data, guint32 length,
                          GDestroyNotify free_func, GError **error);

gboolean goodix_send_pack(FpDevice *dev, guint8 flags, guint8 *payload,
                          guint16 length, GDestroyNotify free_func,
                          GError **error);

void goodix_send_protocol(FpDevice *dev, guint8 cmd, guint8 *payload,
                          guint16 length, GDestroyNotify free_func,
                          gboolean calc_checksum, guint timeout_ms,
                          gboolean reply, GoodixCmdCallback callback,
                          gpointer user_data);

void goodix_send_nop(FpDevice *dev, GoodixNoneCallback callback,
                     gpointer user_data);

void goodix_send_mcu_get_image(FpDevice *dev, guint8* payload, guint16 length, GoodixImageCallback callback,
                               gpointer user_data);

void goodix_send_mcu_switch_to_fdt_down(FpDevice *dev, guint8 *mode,
                                        guint16 length, gboolean reply,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data);

void goodix_send_mcu_switch_to_fdt_up(FpDevice *dev, guint8 *mode,
                                      guint16 length, gboolean reply, GDestroyNotify free_func,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data);

/* Register immediately before the ACK-only phase0 (payload[26] == 0) 0x34
 * command, then send phase1 only after phase0 ACK. Both ordinary command
 * callbacks remain separate from this event callback. Only an additive-
 * checksummed, exactly20-byte 0x34 event with IRQ LE16 0x0200 can complete this
 * waiter, and only after both ordered phase ACKs succeed. timeout_ms is 1..30000.
 * callback owns error and borrows data. destroy(user_data) follows dispatch or
 * cancellation, including rejected registration. No wire transaction ID exists:
 * the caller must serialize arm phases. Generations establish host ownership,
 * but cannot distinguish identical stale same-opcode events on the wire. */
void goodix_wait_fdt_up(FpDevice *dev, guint timeout_ms, GoodixDefaultCallback callback,
                       gpointer user_data, GDestroyNotify destroy);

void goodix_send_mcu_switch_to_fdt_mode(FpDevice *dev, guint8 *mode,
                                        guint16 length, gboolean reply,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data);

void goodix_send_nav_0(FpDevice *dev, GoodixDefaultCallback callback,
                       gpointer user_data);

void goodix_send_mcu_switch_to_idle_mode(FpDevice *dev, guint8 sleep_time,
                                         GoodixNoneCallback callback,
                                         gpointer user_data);

void goodix_send_write_sensor_register(FpDevice *dev, guint16 address,
                                       guint16 value,
                                       GoodixNoneCallback callback,
                                       gpointer user_data);

void goodix_send_read_sensor_register(FpDevice *dev, guint16 address,
                                      guint8 length,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data);

void goodix_send_upload_config_mcu(FpDevice *dev, guint8 *config,
                                   guint16 length, GDestroyNotify free_func,
                                   GoodixSuccessCallback callback,
                                   gpointer user_data);

void goodix_send_set_powerdown_scan_frequency(FpDevice *dev,
                                              guint16 powerdown_scan_frequency,
                                              GoodixSuccessCallback callback,
                                              gpointer user_data);

void goodix_send_enable_chip(FpDevice *dev, gboolean enable,
                             GoodixNoneCallback callback, gpointer user_data);

void goodix_send_reset(FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                       GoodixResetCallback callback, gpointer user_data);

void goodix_send_firmware_version(FpDevice *dev,
                                  GoodixFirmwareVersionCallback callback,
                                  gpointer user_data);

void goodix_send_query_mcu_state(FpDevice *dev, GoodixDefaultCallback callback,
                                 gpointer user_data);

void goodix_send_request_tls_connection(FpDevice* dev,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data);

void goodix_send_tls_successfully_established(FpDevice *dev,
                                              GoodixNoneCallback callback,
                                              gpointer user_data);

void goodix_send_preset_psk_read(FpDevice *dev, guint32 flags, guint16 length,
                                 GoodixPresetPskReadCallback callback,
                                 gpointer user_data);

void goodix_tls_set_psk(FpDevice* dev, const guint8 psk[32]);

void goodix_send_read_otp(FpDevice* dev, GoodixDefaultCallback callback,
                          gpointer user_data);

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error);

/* Cancel on the device's main context before deinitializing an active action.
 * Outstanding command/TLS callbacks run inline exactly once and own errors;
 * callbacks must not start a replacement operation until cancellation returns.
 * Old USB completions retain their device/token and are ignored by generation.
 * Deinit may fail with TLS resources retained; retry without freeing the device. */
void goodix_cancel(FpDevice *dev);

gboolean goodix_dev_deinit(FpDevice *dev, GError **error);

void goodix_reset_state(FpDevice* dev);

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void goodix_read_tls(FpDevice* dev, GoodixTlsCallback callback,
                     gpointer user_data);

/* Completes exactly once through callback, on the caller's libfprint context.
 * Success follows authenticated TLS Finished and the reader's validated ACK.
 * The callback owns error; no FpImageDevice activation is performed here.
 * Supply a key with goodix_tls_set_psk before starting; no default key exists. */
void goodix_tls(FpDevice* dev, GoodixNoneCallback callback, gpointer user_data);

gboolean goodix_shutdown_tls(FpDevice* dev, GError** error);

void goodix_tls_read_image(FpDevice* dev, guint8* payload, guint16 length, GoodixImageCallback callback,
                           gpointer user_data);

// ---- TLS SECTION END ----

/* DOWN counterpart of goodix_wait_fdt_up: register before ACK-only phase0,
 * then phase1 after its ACK. Exact20-byte opcode32, IRQ LE16 0002 and both
 * ordered ACKs are required. UP and DOWN waiters are mutually exclusive.
 * Same callback lifetime and 1..30000ms deadline rules as UP. */
void goodix_wait_fdt_down(FpDevice *dev, guint timeout_ms, GoodixDefaultCallback callback,
                         gpointer user_data, GDestroyNotify destroy);

/* Main-thread-only, read-only readiness snapshot. Zeroes snapshot on failure.
 * Equality is required across reuse; never treat a stopped/restarted transport
 * as the same reference context. This does not perform I/O or authenticate. */
typedef struct {
    guint64 read_loop;
    guint64 tls;
    guint64 configuration;
} GoodixTransportHealth;
gboolean goodix_transport_health(FpDevice *dev, GoodixTransportHealth *snapshot,
                                  GError **error);
/* Main-thread, read-only check allowing an ACKed current UP detector only.
 * Does not consume the event or permit another capture before release. */
gboolean goodix_transport_health_during_release(FpDevice *dev,
                                                GoodixTransportHealth *snapshot,
                                                GError **error);
