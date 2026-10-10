/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Execute the production FpDevice lifecycle, GTask workers and callback paths.
 * Only hardware, algorithm and public-result boundaries are substituted. No
 * reader, private provisioning data, subprocess or system installation needed.
 *
 * These assertions fail if processing results survive cancellation, if a late
 * timeout dispatch permits publication, if a reentrant progress cancellation
 * starts another contact, or if an error completes an operation twice. */
#define fpi_device_get_cancellable mock_get_cancellable
#define fpi_device_add_timeout mock_add_timeout
#define fpi_device_open_complete mock_open_complete
#define fpi_device_close_complete mock_close_complete
#define fpi_device_enroll_complete mock_enroll_complete
#define fpi_device_verify_complete mock_verify_complete
#define fpi_device_identify_complete mock_identify_complete
#define fpi_device_enroll_progress mock_enroll_progress
#define fpi_device_verify_report mock_verify_report
#define fpi_device_identify_report mock_identify_report
#define fpi_device_get_identify_data mock_get_identify_data
#define fpi_device_report_finger_status mock_report_finger_status
#define goodix_wait_read_idle mock_wait_read_idle
#define goodix_dev_init mock_dev_init
#define goodix_dev_deinit mock_dev_deinit
#define goodix_cancel mock_cancel
#define goodix_start_read_loop mock_start_read_loop
#define goodix_send_nop mock_send_nop
#define goodix_send_enable_chip mock_send_enable_chip
#define goodix_send_firmware_version mock_send_firmware_version
#define goodix_send_preset_psk_read mock_send_preset_psk_read
#define goodix_send_reset mock_send_reset
#define goodix_send_mcu_switch_to_idle_mode mock_send_idle
#define goodix_send_upload_config_mcu mock_send_config
#define goodix_tls_set_psk mock_set_psk
#define goodix_tls mock_tls
#define goodix_tls_load_psk_file mock_load_psk
#define goodix521d_read_private mock_read_private
#define goodix521d_configure mock_configure
#define goodix_acquisition_cancel mock_acquisition_cancel
#define goodix_acquisition_new mock_acquisition_new
#define goodix_acquisition_free mock_acquisition_free
#define goodix_acquisition_start mock_acquisition_start
#define oc_session_new_for_profile mock_session_new
#define oc_session_free mock_session_free
#define oc_enroll_begin mock_enroll_begin
#define oc_enroll_add mock_enroll_add
#define oc_enroll_finish_complete mock_enroll_finish
#define oc_identify mock_identify
#define gx_print_envelope_export mock_envelope_export
#include <glib-object.h>
#include "../libfprint/drivers/goodix521d/goodix521d.c"

/* Public-API regressions use real completion and generic device bookkeeping. */
#undef fpi_device_open_complete
#undef fpi_device_close_complete
#undef fpi_device_enroll_complete
#undef fpi_device_verify_complete
#undef fpi_device_verify_report
#undef fpi_device_identify_complete
#undef fpi_device_identify_report
#undef fpi_device_get_identify_data
#undef gx_print_envelope_export
#include "goodix-chicago-enrollment.h"
void fpi_device_open_complete (FpDevice *device, GError *error);
void fpi_device_close_complete (FpDevice *device, GError *error);
void fpi_device_enroll_complete (FpDevice *device, FpPrint *print, GError *error);
void fpi_device_verify_complete (FpDevice *device, GError *error);
void fpi_device_identify_complete (FpDevice *device, GError *error);
void fpi_device_identify_report (FpDevice *device, FpPrint *match, FpPrint *print, GError *error);
void fpi_device_get_identify_data (FpDevice *device, GPtrArray **prints);
void fpi_device_verify_report (FpDevice *device, FpiMatchResult result,
                              FpPrint *print, GError *error);
GVariant *gx_print_envelope_export (const GxPrintIdentity *identity, gboolean completed,
                                   GBytes *blob, GError **error);

/* A mutex-protected gate holds a real worker inside an algorithm boundary.
 * Cancellation/timeout remains on the main thread, never races test state. */
static struct {
  GCancellable *cancellable;
  GMutex mutex;
  GCond condition;
  gboolean worker_entered, release_worker, block_worker;
  gboolean enroll_complete, cancel_progress, cancel_report;
  gboolean fail_input, fail_init, fail_cleanup, cancel_unwinds_error;
  guint completed[5], succeeded[5], close_completed;
  guint progress, reports, captures, starts, stops, cancels, exports;
  guint matcher_calls, matcher_count, identify_matches;
  gboolean matcher_fail, matcher_nonmatch, report_retry;
  FpiMatchResult last_match_report;
  FpDevice *checked_match_device;
  OcSession *expected_match_session;
  GBytes *expected_template;
  guint expected_stops_before_match;
  OcReject matcher_reject;
  guint sessions_created, sessions_freed, acquisitions_created, acquisitions_freed;
  GError *error;
  gboolean public_api, usb_open, public_success, public_match;
  guint usb_opens, usb_closes, public_callbacks;
  FpDevice *usb_owner;
  GError *public_error;
  gboolean hold_drain, fail_drain;
  guint drain_waits;
  FpDevice *drain_device;
  GoodixNoneCallback drain_callback;
  gpointer drain_data;
  GoodixAcquired capture_callback;
  gpointer capture_data;
  GPtrArray *prints;
} state;

static OcSession *
new_session (void)
{
  state.sessions_created++;
  return (OcSession *) g_malloc0 (1);
}

static void
worker_gate (void)
{
  g_mutex_lock (&state.mutex);
  state.worker_entered = TRUE;
  g_cond_signal (&state.condition);
  while (state.block_worker && !state.release_worker)
    g_cond_wait (&state.condition, &state.mutex);
  g_mutex_unlock (&state.mutex);
}

GCancellable *
mock_get_cancellable (FpDevice *device)
{
  return state.cancellable;
}

/* The source is attached and owned like a libfprint timeout, but dispatch is
 * deliberately suppressed: tests prove that checking wall-clock state is not
 * delegated solely to dispatching a pending GLib timeout. */
GSource *
mock_add_timeout (FpDevice *device, gint interval, FpTimeoutFunc callback,
                  gpointer data, GDestroyNotify destroy)
{
  GSource *source = g_timeout_source_new (interval);
  g_source_attach (source, NULL);
  g_source_unref (source);
  return source;
}

static void
complete (Action action, GError *error)
{
  state.completed[action]++;
  if (!error)
    state.succeeded[action]++;
  g_clear_error (&state.error);
  state.error = error;
}

void mock_open_complete (FpDevice *device, GError *error)
{
  if (state.public_api) {
    complete (ACTION_OPEN, error ? g_error_copy (error) : NULL);
    fpi_device_open_complete (device, error);
  } else {
    complete (ACTION_OPEN, error);
  }
}
void mock_verify_complete (FpDevice *device, GError *error)
{
  if (state.public_api) {
    complete (ACTION_VERIFY, error ? g_error_copy (error) : NULL);
    fpi_device_verify_complete (device, error);
  } else {
    complete (ACTION_VERIFY, error);
  }
}
void mock_identify_complete (FpDevice *device, GError *error)
{
  if (state.public_api) {
    complete (ACTION_IDENTIFY, error ? g_error_copy (error) : NULL);
    fpi_device_identify_complete (device, error);
  } else {
    complete (ACTION_IDENTIFY, error);
  }
}
void mock_enroll_complete (FpDevice *device, FpPrint *print, GError *error)
{
  if (state.public_api) {
    complete (ACTION_ENROLL, error ? g_error_copy (error) : NULL);
    fpi_device_enroll_complete (device, print, error);
  } else {
    g_clear_object (&print);
    complete (ACTION_ENROLL, error);
  }
}
void mock_close_complete (FpDevice *device, GError *error)
{
  state.close_completed++;
  g_clear_error (&state.error);
  if (state.public_api) {
    state.error = error ? g_error_copy (error) : NULL;
    fpi_device_close_complete (device, error);
  } else {
    state.error = error;
  }
}
void mock_enroll_progress (FpDevice *device, gint stages, FpPrint *print, GError *error)
{
  state.progress++;
  g_clear_error (&error);
  if (state.cancel_progress)
    cancel_operation (device);
}
void mock_verify_report (FpDevice *device, FpiMatchResult result, FpPrint *print, GError *error)
{
  state.reports++;
  state.report_retry = error && error->domain == FP_DEVICE_RETRY;
  state.last_match_report = result;
  if (state.public_api)
    fpi_device_verify_report (device, result, print, error);
  else
    g_clear_error (&error);
  if (state.cancel_report)
    cancel_operation (device);
}
void mock_identify_report (FpDevice *device, FpPrint *match, FpPrint *print, GError *error)
{
  if (match) state.identify_matches++;
  if (state.public_api) {
    state.reports++;
    state.report_retry = error && error->domain == FP_DEVICE_RETRY;
    fpi_device_identify_report (device, match, print, error);
    if (state.cancel_report)
      cancel_operation (device);
  } else {
    mock_verify_report (device, FPI_MATCH_SUCCESS, print, error);
  }
}
void mock_get_identify_data (FpDevice *device, GPtrArray **prints)
{
  if (state.public_api)
    fpi_device_get_identify_data (device, prints);
  else
    *prints = state.prints;
}
gboolean mock_report_finger_status (FpDevice *device, FpFingerStatusFlags status) { return TRUE; }

static void
release_read_drain (GError *error)
{
  g_autoptr(FpDevice) device = g_steal_pointer (&state.drain_device);
  GoodixNoneCallback callback = state.drain_callback;
  gpointer data = state.drain_data;
  state.drain_callback = NULL;
  state.drain_data = NULL;
  callback (device, data, error);
}

static gboolean
read_drain_idle (gpointer unused)
{
  release_read_drain (state.fail_drain ?
    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "synthetic RX drain timeout") : NULL);
  return G_SOURCE_REMOVE;
}

void
mock_wait_read_idle (FpDevice *device, guint timeout_ms,
                    GoodixNoneCallback callback, gpointer data)
{
  g_assert_cmpuint (timeout_ms, ==, 2000);
  g_assert_null (state.drain_callback);
  state.drain_waits++;
  state.drain_device = g_object_ref (device);
  state.drain_callback = callback;
  state.drain_data = data;
  if (!state.hold_drain)
    g_idle_add (read_drain_idle, NULL);
}

gboolean mock_dev_init (FpDevice *device, GError **error)
{
  state.starts++;
  if (state.fail_init) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "synthetic startup failure");
    return FALSE;
  }
  return TRUE;
}
gboolean mock_dev_deinit (FpDevice *device, GError **error)
{
  state.stops++;
  if (state.fail_cleanup) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "synthetic cleanup failure");
    return FALSE;
  }
  return TRUE;
}
void mock_cancel (FpDevice *device)
{
  state.cancels++;
  if (state.cancel_unwinds_error) {
    operation_error (device, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                 "synchronous command unwind"));
    operation_error (device, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                 "synchronous TLS unwind"));
    /* Neither teardown nor completion is safe on the transport's stack. */
    g_assert_cmpuint (state.stops, ==, 0);
    g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, 0);
  }
}
void mock_start_read_loop (FpDevice *device) { }
void mock_send_nop (FpDevice *device, GoodixNoneCallback callback, gpointer data)
{ callback (device, data, NULL); }
void mock_send_enable_chip (FpDevice *device, gboolean enable, GoodixNoneCallback callback, gpointer data)
{ callback (device, data, NULL); }
void mock_send_firmware_version (FpDevice *device, GoodixFirmwareVersionCallback callback, gpointer data)
{ gchar firmware[] = "GFUSB_GM168SEC_APP_10034"; callback (device, firmware, data, NULL); }
void mock_send_preset_psk_read (FpDevice *device, guint32 flags, guint16 length,
                               GoodixPresetPskReadCallback callback, gpointer data)
{
  guint8 key[32] = {0}, digest[32];
  gsize size = sizeof digest;
  g_autoptr(GChecksum) hash = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (hash, key, sizeof key);
  g_checksum_get_digest (hash, digest, &size);
  callback (device, TRUE, flags, digest, sizeof digest, data, NULL);
}
void mock_send_reset (FpDevice *device, gboolean reset, guint8 sleep,
                     GoodixResetCallback callback, gpointer data)
{ callback (device, TRUE, 2048, data, NULL); }
void mock_send_idle (FpDevice *device, guint8 sleep, GoodixNoneCallback callback, gpointer data)
{ callback (device, data, NULL); }
void mock_send_config (FpDevice *device, guint8 *config, guint16 length,
                      GDestroyNotify destroy, GoodixSuccessCallback callback, gpointer data)
{ callback (device, TRUE, data, NULL); }
void mock_set_psk (FpDevice *device, const guint8 key[32]) { }
void mock_tls (FpDevice *device, GoodixNoneCallback callback, gpointer data)
{ callback (device, data, NULL); }
gboolean mock_load_psk (const char *path, guint8 key[32], GError **error)
{ memset (key, 0, 32); return TRUE; }
gboolean mock_read_private (const char *path, guint8 *data, gsize length, GError **error)
{
  if (state.fail_input) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "synthetic missing input");
    return FALSE;
  }
  memset (data, 0, length);
  return TRUE;
}
gboolean mock_configure (const guint8 factory[64], guint8 config[256], GError **error)
{ memset (config, 0, 256); return TRUE; }
GoodixAcquisition *mock_acquisition_new (FpDevice *device, const guint8 factory[64])
{
  state.acquisitions_created++;
  return (GoodixAcquisition *) g_malloc0 (1);
}
void mock_acquisition_cancel (GoodixAcquisition *acquisition) { }
void mock_acquisition_free (GoodixAcquisition *acquisition)
{
  state.acquisitions_freed++;
  g_free (acquisition);
}
void mock_acquisition_start (GoodixAcquisition *acquisition, gboolean reference,
                            GoodixAcquired callback, gpointer data)
{
  state.captures++;
  state.capture_callback = callback;
  state.capture_data = data;
}
OcSession *mock_session_new (const guint16 frame[OC_FRAME_PIXELS],
                            GoodixChicagoProfile profile, GError **error)
{
  worker_gate ();
  return new_session ();
}
void mock_session_free (OcSession *session)
{
  state.sessions_freed++;
  g_free (session);
}
gboolean mock_enroll_begin (OcSession *session, OcEnrollProtocol protocol,
                            guint stages, GError **error) { return TRUE; }
gboolean mock_enroll_add (OcSession *session, const guint16 frame[OC_FRAME_PIXELS],
                         OcEnrollResult *result, GError **error)
{
  worker_gate ();
  result->accepted = TRUE;
  result->stage = 1;
  result->complete = state.enroll_complete;
  return TRUE;
}
GBytes *mock_enroll_finish (OcSession *session, GError **error)
{ return g_bytes_new_static ("synthetic completed template", 28); }
gboolean mock_identify (OcSession *session, GBytes *const *templates, guint count,
                       const guint16 frame[OC_FRAME_PIXELS], OcVerifyResult *result,
                       gint *match, GBytes **updated, GError **error)
{
  if (state.checked_match_device) {
    g_assert_false (SELF (state.checked_match_device)->transport_open);
    g_assert_cmpuint (state.stops, ==, state.expected_stops_before_match);
    g_assert_true (session == state.expected_match_session);
    g_assert_cmpuint (count, ==, 1);
    g_assert_true (g_bytes_equal (templates[0], state.expected_template));
    g_assert_null (updated); /* Verification never requests template study. */
  }
  worker_gate ();
  state.matcher_calls++;
  state.matcher_count = count;
  if (state.matcher_fail) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "synthetic matcher failure");
    return FALSE;
  }
  result->match = count > 0 && !state.matcher_nonmatch;
  result->score = result->match ? 42 : 0;
  result->reject = state.matcher_reject;
  *match = result->match ? 0 : -1;
  return TRUE;
}
GVariant *mock_envelope_export (const GxPrintIdentity *identity, gboolean completed,
                               GBytes *blob, GError **error)
{
  state.exports++;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "test must never publish a cancelled enrollment");
  return NULL;
}

/* GUsb's JSON emulation permits repeated open, so model the real handle's
 * strict open/close contract at the hardware boundary. Both the shared generic
 * FpDevice implementation and source-included driver call these functions. */
gboolean
g_usb_device_open (GUsbDevice *usb, GError **error)
{
  g_assert_true (G_USB_IS_DEVICE (usb));
  state.usb_opens++;
  if (state.usb_open) {
    g_set_error_literal (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_ALREADY_OPEN,
                         "Device 27c6:521d is already open");
    return FALSE;
  }
  state.usb_open = TRUE;
  return TRUE;
}

gboolean
g_usb_device_close (GUsbDevice *usb, GError **error)
{
  /* Older white-box tests invoke driver vfuncs without the generic USB open. */
  if (!state.public_api)
    return TRUE;
  g_assert_true (G_USB_IS_DEVICE (usb));
  g_assert_nonnull (state.usb_owner);
  g_assert_false (SELF (state.usb_owner)->transport_open);
  g_assert_null (state.drain_callback);
  state.usb_closes++;
  if (!state.usb_open) {
    g_set_error_literal (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NOT_OPEN,
                         "Device 27c6:521d is not open");
    return FALSE;
  }
  state.usb_open = FALSE;
  return TRUE;
}

static void
reset_state (void)
{
  memset (&state, 0, sizeof state);
  g_mutex_init (&state.mutex);
  g_cond_init (&state.condition);
  state.cancellable = g_cancellable_new ();
  state.prints = g_ptr_array_new ();
  g_ptr_array_add (state.prints, NULL);
}

static void
finish_state (FpDevice *device)
{
  g_object_unref (device);
  while (g_main_context_iteration (NULL, FALSE));
  g_assert_cmpuint (state.sessions_created, ==, state.sessions_freed);
  g_assert_cmpuint (state.acquisitions_created, ==, state.acquisitions_freed);
  g_clear_object (&state.cancellable);
  g_clear_pointer (&state.prints, g_ptr_array_unref);
  g_clear_pointer (&state.expected_template, g_bytes_unref);
  g_clear_error (&state.error);
  g_clear_error (&state.public_error);
  g_assert_null (state.drain_device);
  g_mutex_clear (&state.mutex);
  g_cond_clear (&state.condition);
}

static FpDevice *
new_device (void)
{
  return g_object_new (fpi_device_goodix521d_get_type (), NULL);
}

static FpDevice *
prepare_action (Action action)
{
  FpDevice *device = new_device ();
  FpiDeviceGoodix521d *self = SELF (device);
  self->seen_frames = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (action != ACTION_OPEN)
    self->session = new_session ();
  if (action == ACTION_VERIFY || action == ACTION_IDENTIFY) {
    self->templates = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
    g_ptr_array_add (self->templates, g_bytes_new_static ("template", 8));
  }
  begin_action (device, action, 60000);
  g_source_set_ready_time (self->deadline, -1);
  return device;
}

static void
start_processing (FpDevice *device)
{
  GxImage image = {0};
  image.valid = TRUE;
  state.block_worker = TRUE;
  acquired_process (device, &image, NULL);
  g_assert_true (SELF (device)->processing);
  g_mutex_lock (&state.mutex);
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!state.worker_entered)
    g_assert_true (g_cond_wait_until (&state.condition, &state.mutex, deadline));
  g_mutex_unlock (&state.mutex);
}

static void
release_processing (void)
{
  g_mutex_lock (&state.mutex);
  state.release_worker = TRUE;
  g_cond_signal (&state.condition);
  g_mutex_unlock (&state.mutex);
}

static void
wait_completed (Action action, guint count)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (state.completed[action] < count) {
    g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
  }
  while (g_main_context_iteration (NULL, FALSE));
}

static void
assert_failure_once_with_session (FpDevice *device, Action action, GIOErrorEnum code,
                                  OcSession *expected_session)
{
  wait_completed (action, 1);
  g_assert_cmpuint (state.completed[action], ==, 1);
  g_assert_cmpuint (state.succeeded[action], ==, 0);
  g_assert_error (state.error, G_IO_ERROR, code);
  g_assert_cmpuint (state.exports, ==, 0);
  g_assert_cmpuint (state.progress, ==, 0);
  g_assert_cmpuint (state.reports, ==, 0);
  g_assert_cmpint (SELF (device)->action, ==, ACTION_NONE);
  g_assert_false (SELF (device)->processing);
  g_assert_true (SELF (device)->session == expected_session);
  g_assert_null (SELF (device)->templates);
  g_assert_null (SELF (device)->deadline);
  /* Duplicate/stale failures cannot complete an idle action a second time. */
  operation_error (device, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "late error"));
  cancel_operation (device);
  while (g_main_context_iteration (NULL, FALSE));
  g_assert_cmpuint (state.completed[action], ==, 1);
}

static void
assert_failure_once (FpDevice *device, Action action, GIOErrorEnum code)
{
  assert_failure_once_with_session (device, action, code, NULL);
}

static void
run_cancel_worker (Action action, gboolean through_cancellable)
{
  reset_state ();
  state.enroll_complete = TRUE;
  FpDevice *device = prepare_action (action);
  start_processing (device);
  if (through_cancellable)
    g_cancellable_cancel (state.cancellable);
  else
    cancel_operation (device);
  /* Worker owns its session until it has returned, even after cancellation. */
  g_assert_cmpuint (state.completed[action], ==, 0);
  g_assert_cmpuint (state.sessions_freed, ==, 0);
  release_processing ();
  assert_failure_once (device, action, G_IO_ERROR_CANCELLED);
  finish_state (device);
}

static void
test_cancel_worker (gconstpointer data)
{
  run_cancel_worker (GPOINTER_TO_INT (data), FALSE);
}

static void
test_cancellable_worker (gconstpointer data)
{
  run_cancel_worker (GPOINTER_TO_INT (data), TRUE);
}

static void
test_expired_worker (gconstpointer data)
{
  Action action = GPOINTER_TO_INT (data);
  reset_state ();
  state.enroll_complete = TRUE;
  FpDevice *device = prepare_action (action);
  start_processing (device);
  /* Do not call operation_timeout: the timeout source has not dispatched. */
  SELF (device)->deadline_us = g_get_monotonic_time () - 1;
  release_processing ();
  assert_failure_once (device, action, G_IO_ERROR_TIMED_OUT);
  finish_state (device);
}

static void
test_progress_cancel (void)
{
  reset_state ();
  state.cancel_progress = TRUE;
  FpDevice *device = prepare_action (ACTION_ENROLL);
  start_processing (device);
  release_processing ();
  wait_completed (ACTION_ENROLL, 1);
  g_assert_cmpuint (state.progress, ==, 1);
  g_assert_cmpuint (state.starts, ==, 0);
  g_assert_cmpuint (state.captures, ==, 0);
  g_assert_cmpuint (SELF (device)->contacts, ==, 0);
  g_assert_cmpuint (state.completed[ACTION_ENROLL], ==, 1);
  g_assert_error (state.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  finish_state (device);
}

static void
test_report_cancel (gconstpointer data)
{
  Action action = GPOINTER_TO_INT (data);
  reset_state ();
  state.cancel_report = TRUE;
  FpDevice *device = prepare_action (action);
  start_processing (device);
  release_processing ();
  wait_completed (action, 1);
  g_assert_cmpuint (state.reports, ==, 1);
  g_assert_cmpuint (state.completed[action], ==, 1);
  g_assert_cmpuint (state.succeeded[action], ==, 0);
  g_assert_error (state.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  finish_state (device);
}

static void
test_success_once (gconstpointer data)
{
  Action action = GPOINTER_TO_INT (data);
  reset_state ();
  FpDevice *device = prepare_action (action);
  start_processing (device);
  release_processing ();
  wait_completed (action, 1);
  g_assert_cmpuint (state.completed[action], ==, 1);
  g_assert_cmpuint (state.succeeded[action], ==, 1);
  g_assert_cmpuint (state.reports, ==, action == ACTION_OPEN ? 0 : 1);
  g_assert_no_error (state.error);
  operation_error (device, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "late error"));
  cancel_operation (device);
  while (g_main_context_iteration (NULL, FALSE));
  g_assert_cmpuint (state.completed[action], ==, 1);
  finish_state (device);
}

static void
test_open_failure_reopen (void)
{
  reset_state ();
  FpDevice *device = new_device ();
  state.fail_init = TRUE;
  dev_open (device);
  wait_completed (ACTION_OPEN, 1);
  g_assert_cmpuint (state.acquisitions_created, ==, 1);
  g_assert_cmpuint (state.succeeded[ACTION_OPEN], ==, 0);
  g_assert_error (state.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_null (SELF (device)->deadline);
  g_assert_false (SELF (device)->transport_open);
  state.fail_init = FALSE;
  dev_open (device);
  g_assert_cmpuint (state.acquisitions_created, ==, 2);
  g_assert_cmpuint (state.acquisitions_freed, ==, 1);
  g_assert_cmpuint (state.starts, ==, 2);
  g_assert_cmpuint (state.captures, ==, 1);
  g_assert_nonnull (state.capture_callback);
  GxImage image = {0};
  image.valid = TRUE;
  state.capture_callback (device, &image, state.capture_data, NULL);
  wait_completed (ACTION_OPEN, 2);
  g_assert_cmpuint (state.succeeded[ACTION_OPEN], ==, 1);
  g_assert_no_error (state.error);
  g_assert_nonnull (SELF (device)->session);
  g_assert_false (SELF (device)->transport_open);
  dev_close (device);
  g_assert_cmpuint (state.close_completed, ==, 1);
  g_assert_null (SELF (device)->session);
  g_assert_null (SELF (device)->acquisition);
  g_assert_null (SELF (device)->seen_frames);
  g_assert_null (SELF (device)->input_dir);
  g_assert_null (SELF (device)->key_path);
  finish_state (device);
}

static void
test_open_missing_input (void)
{
  reset_state ();
  FpDevice *device = new_device ();
  state.fail_input = TRUE;
  dev_open (device);
  wait_completed (ACTION_OPEN, 1);
  g_assert_error (state.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpuint (state.starts, ==, 0);
  g_assert_cmpuint (state.acquisitions_created, ==, 0);
  g_assert_null (SELF (device)->deadline);
  finish_state (device);
}

static void
test_queued_capture_cancel (void)
{
  reset_state ();
  FpDevice *device = prepare_action (ACTION_ENROLL);
  GxImage image = {0};
  image.valid = TRUE;
  acquired (device, &image, NULL, NULL);
  cancel_operation (device);
  assert_failure_once (device, ACTION_ENROLL, G_IO_ERROR_CANCELLED);
  g_assert_false (state.worker_entered);
  g_assert_cmpuint (state.starts, ==, 0);
  finish_state (device);
}

static void
test_cleanup_failure_blocks_reopen (void)
{
  reset_state ();
  FpDevice *device = prepare_action (ACTION_VERIFY);
  SELF (device)->transport_open = TRUE;
  state.fail_cleanup = TRUE;
  operation_error (device, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic operation failure"));
  wait_completed (ACTION_VERIFY, 1);
  g_assert_true (SELF (device)->transport_open);
  g_assert_cmpuint (state.stops, ==, 1);
  dev_open (device);
  g_assert_cmpuint (state.completed[ACTION_OPEN], ==, 1);
  g_assert_error (state.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  g_assert_cmpuint (state.starts, ==, 0);
  g_assert_true (SELF (device)->transport_open);
  state.fail_cleanup = FALSE;
  dev_close (device);
  g_assert_cmpuint (state.close_completed, ==, 1);
  g_assert_cmpuint (state.stops, ==, 2);
  g_assert_false (SELF (device)->transport_open);
  g_assert_no_error (state.error);
  finish_state (device);
}

static void
test_cancel_unwind_completes_once (void)
{
  reset_state ();
  FpDevice *device = prepare_action (ACTION_VERIFY);
  SELF (device)->transport_open = TRUE;
  OcSession *session = SELF (device)->session;
  state.cancel_unwinds_error = TRUE;
  cancel_operation (device);
  g_assert_cmpuint (state.stops, ==, 0);
  g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, 0);
  state.cancel_unwinds_error = FALSE;
  assert_failure_once_with_session (device, ACTION_VERIFY, G_IO_ERROR_CANCELLED, session);
  g_assert_cmpuint (state.stops, ==, 1);
  g_assert_false (SELF (device)->transport_open);
  finish_state (device);
}

/* fprintd performs duplicate identification before the first enrollment,
 * including when the gallery is empty. This must retain the ordinary capture,
 * quality, cancellation and deadline paths without inventing a match. */
static void
test_empty_identify (gconstpointer data)
{
  guint mode = GPOINTER_TO_UINT (data);
  reset_state ();
  FpDevice *device = prepare_action (ACTION_IDENTIFY);
  clear_deadline (SELF (device));
  SELF (device)->action = ACTION_NONE;
  g_clear_pointer (&SELF (device)->templates, g_ptr_array_unref);
  g_ptr_array_set_size (state.prints, 0);
  dev_identify (device);
  g_assert_cmpuint (state.captures, ==, 1);
  g_assert_cmpuint (state.completed[ACTION_IDENTIFY], ==, 0);
  if (mode == 1) state.matcher_reject = OC_REJECT_NO_FEATURES;
  if (mode == 2) state.matcher_fail = TRUE;
  start_processing (device);
  if (mode == 3) cancel_operation (device);
  if (mode == 4) {
    SELF (device)->deadline_us = g_get_monotonic_time () - 1;
    g_source_set_ready_time (SELF (device)->deadline, -1);
  }
  release_processing ();
  wait_completed (ACTION_IDENTIFY, 1);
  g_assert_cmpuint (state.matcher_calls, ==, 1);
  g_assert_cmpuint (state.matcher_count, ==, 0);
  g_assert_cmpuint (state.identify_matches, ==, 0);
  g_assert_cmpuint (state.exports, ==, 0);
  if (mode < 2) {
    g_assert_no_error (state.error);
    g_assert_cmpuint (state.succeeded[ACTION_IDENTIFY], ==, 1);
    g_assert_cmpuint (state.reports, ==, 1);
    g_assert_cmpint (state.report_retry, ==, mode == 1);
    g_assert_nonnull (SELF (device)->session);
  } else {
    GIOErrorEnum code = mode == 2 ? G_IO_ERROR_FAILED :
                        mode == 3 ? G_IO_ERROR_CANCELLED : G_IO_ERROR_TIMED_OUT;
    assert_failure_once (device, ACTION_IDENTIFY, code);
  }
  finish_state (device);
}

static void
test_invalid_identify_gallery (gconstpointer data)
{
  gboolean oversized = GPOINTER_TO_INT (data);
  reset_state ();
  FpDevice *device = prepare_action (ACTION_IDENTIFY);
  clear_deadline (SELF (device));
  SELF (device)->action = ACTION_NONE;
  g_clear_pointer (&SELF (device)->templates, g_ptr_array_unref);
  SELF (device)->identity = (GxPrintIdentity) { .backend = "openchicago", .backend_version = 1,
    .profile = 17, .preprocess_version = 1 };
  FpPrint *invalid = NULL;
  if (oversized) g_ptr_array_set_size (state.prints, 101);
  else {
    invalid = fp_print_new (device);
    state.prints->pdata[0] = invalid;
  }
  dev_identify (device);
  wait_completed (ACTION_IDENTIFY, 1);
  g_assert_nonnull (state.error);
  g_assert_cmpuint (state.succeeded[ACTION_IDENTIFY], ==, 0);
  g_assert_cmpuint (state.captures, ==, 0);
  g_assert_cmpuint (state.reports, ==, 0);
  g_clear_object (&invalid);
  finish_state (device);
}

static FpDevice *
new_public_device (void)
{
  g_autoptr(GUsbDevice) usb = g_object_new (G_USB_TYPE_DEVICE, NULL);
  state.public_api = TRUE;
  state.usb_owner = g_object_new (fpi_device_goodix521d_get_type (),
                                "fpi-usb-device", usb, NULL);
  return state.usb_owner;
}

static void
public_open_done (GObject *object, GAsyncResult *result, gpointer data)
{
  g_clear_error (&state.public_error);
  state.public_success = fp_device_open_finish (FP_DEVICE (object), result,
                                                &state.public_error);
  state.public_callbacks++;
}

static void
wait_public_callbacks (guint count)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (state.public_callbacks < count) {
    g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
  }
  g_assert_cmpuint (state.public_callbacks, ==, count);
}

static void
test_public_failed_open_retries (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  fp_device_open (device, NULL, public_open_done, NULL);
  g_assert_true (state.usb_open);
  g_assert_true (SELF (device)->transport_open);
  g_assert_nonnull (state.capture_callback);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic FDT stability failure"));
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_false (fp_device_is_open (device));
  g_assert_cmpuint (state.stops, ==, 1);

  /* The second real fp_device_open must reach this same driver's open vfunc;
   * without failed-open USB cleanup it ends at GUsb's ALREADY_OPEN check. */
  state.capture_callback = NULL;
  fp_device_open (device, NULL, public_open_done, NULL);
  g_assert_nonnull (state.capture_callback);
  g_assert_cmpuint (state.usb_opens, ==, 2);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  GxImage image = {0};
  image.valid = TRUE;
  state.capture_callback (device, &image, state.capture_data, NULL);
  wait_public_callbacks (2);
  g_assert_true (state.public_success);
  g_assert_no_error (state.public_error);
  g_assert_true (fp_device_is_open (device));
  g_assert_true (state.usb_open);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_assert_cmpuint (state.usb_closes, ==, 2);
  g_assert_false (state.usb_open);
  finish_state (device);
}

static void
test_public_failed_open_retains_usb_on_cleanup_failure (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  fp_device_open (device, NULL, public_open_done, NULL);
  state.fail_cleanup = TRUE;
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic acquisition failure"));
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_true (SELF (device)->transport_open);
  g_assert_true (state.usb_open);
  g_assert_cmpuint (state.usb_closes, ==, 0);
  /* The failed-open API state cannot expose close; release the test fixture's
   * retained resources explicitly, without pretending recovery succeeded. */
  state.fail_cleanup = FALSE;
  g_autoptr(GError) cleanup_error = NULL;
  g_assert_true (stop_transport (SELF (device), &cleanup_error));
  g_assert_true (g_usb_device_close (fpi_device_get_usb_device (device), &cleanup_error));
  g_assert_no_error (cleanup_error);
  finish_state (device);
}

static void
public_enroll_done (GObject *object, GAsyncResult *result, gpointer data)
{
  g_clear_error (&state.public_error);
  g_autoptr(FpPrint) print = fp_device_enroll_finish (FP_DEVICE (object), result,
                                                    &state.public_error);
  g_assert_null (print);
  state.public_callbacks++;
}

static void
test_public_enroll_error_keeps_usb_open (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  fp_device_open (device, NULL, public_open_done, NULL);
  GxImage image = {0};
  image.valid = TRUE;
  state.capture_callback (device, &image, state.capture_data, NULL);
  wait_public_callbacks (1);
  g_assert_true (state.public_success);
  g_assert_true (fp_device_is_open (device));
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  fp_device_enroll (device, print, NULL, NULL, NULL, NULL, public_enroll_done, NULL);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic enrollment failure"));
  wait_public_callbacks (2);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (fp_device_is_open (device));
  g_assert_true (state.usb_open);
  g_assert_cmpuint (state.usb_closes, ==, 0);
  g_clear_error (&state.public_error);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  g_clear_object (&print);
  finish_state (device);
}

static void
test_public_failed_open_waits_for_rx (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  state.hold_drain = TRUE;
  fp_device_open (device, NULL, public_open_done, NULL);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic FDT stability failure"));
  while (g_main_context_iteration (NULL, FALSE));
  g_assert_cmpuint (state.drain_waits, ==, 1);
  g_assert_cmpuint (state.public_callbacks, ==, 0);
  g_assert_cmpuint (state.completed[ACTION_OPEN], ==, 0);
  g_assert_cmpuint (state.usb_closes, ==, 0);
  g_assert_true (state.usb_open);
  release_read_drain (NULL);
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  g_assert_false (state.usb_open);
  state.hold_drain = FALSE;
  fp_device_open (device, NULL, public_open_done, NULL);
  GxImage image = {0};
  image.valid = TRUE;
  state.capture_callback (device, &image, state.capture_data, NULL);
  wait_public_callbacks (2);
  g_assert_true (state.public_success);
  g_assert_true (fp_device_is_open (device));
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_assert_cmpuint (state.usb_closes, ==, 2);
  finish_state (device);
}

static void
test_public_failed_open_drain_timeout (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  state.fail_drain = TRUE;
  fp_device_open (device, NULL, public_open_done, NULL);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic FDT stability failure"));
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (state.public_error->message, "RX drain timeout"));
  g_assert_nonnull (strstr (state.public_error->message, "FDT stability failure"));
  g_assert_true (state.usb_open);
  g_assert_cmpuint (state.usb_closes, ==, 0);
  fp_device_open (device, NULL, public_open_done, NULL);
  wait_public_callbacks (2);
  g_assert_error (state.public_error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_ALREADY_OPEN);
  g_assert_cmpuint (state.completed[ACTION_OPEN], ==, 1);
  /* No driver retry/reuse happened. Simulate the eventual transport owner
   * release, then release only the fixture's retained USB handle. */
  g_assert_cmpuint (state.starts, ==, 1);
  g_autoptr(GError) cleanup_error = NULL;
  g_assert_true (g_usb_device_close (fpi_device_get_usb_device (device), &cleanup_error));
  g_assert_no_error (cleanup_error);
  finish_state (device);
}

static void
test_public_early_open_failure (gconstpointer data)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  state.fail_input = GPOINTER_TO_INT (data) == 0;
  state.fail_init = !state.fail_input;
  fp_device_open (device, NULL, public_open_done, NULL);
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR,
                  state.fail_input ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED);
  g_assert_cmpuint (state.usb_opens, ==, 1);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  g_assert_cmpuint (state.drain_waits, ==, 1);
  g_assert_cmpuint (state.captures, ==, 0);
  g_assert_cmpuint (state.stops, ==, 0);
  g_assert_false (state.usb_open);
  finish_state (device);
}

static void
test_public_cancel_during_failed_open_drain (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  state.hold_drain = TRUE;
  fp_device_open (device, NULL, public_open_done, NULL);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "synthetic FDT stability failure"));
  while (g_main_context_iteration (NULL, FALSE));
  g_assert_nonnull (state.drain_callback);
  cancel_operation (device);
  cancel_operation (device);
  g_assert_cmpuint (state.completed[ACTION_OPEN], ==, 0);
  g_assert_cmpuint (state.usb_closes, ==, 0);
  release_read_drain (NULL);
  wait_public_callbacks (1);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (state.public_error->message, "FDT stability failure"));
  g_assert_cmpuint (state.completed[ACTION_OPEN], ==, 1);
  g_assert_cmpuint (state.usb_closes, ==, 1);
  g_assert_false (fp_device_is_open (device));
  finish_state (device);
}

/* These tests exercise public FpDevice actions and real print import/storage,
 * but the algorithm and sensor remain synthetic. They establish lifecycle and
 * template immutability, not real matching/calibration accuracy. */
static void
open_with_reference (FpDevice *device)
{
  guint next_callback = state.public_callbacks + 1;
  state.block_worker = FALSE;
  state.worker_entered = state.release_worker = FALSE;
  fp_device_open (device, NULL, public_open_done, NULL);
  g_assert_nonnull (state.capture_callback);
  GxImage reference = {0};
  reference.valid = TRUE;
  state.capture_callback (device, &reference, state.capture_data, NULL);
  wait_public_callbacks (next_callback);
  g_assert_true (state.public_success);
  g_assert_no_error (state.public_error);
  g_assert_nonnull (SELF (device)->session);
}

static FpPrint *
make_verification_print (FpDevice *device)
{
  g_autoptr(GoodixChicagoEnrollment) enrollment = goodix_chicago_enrollment_new ();
  GoodixChicagoFeatureRecord record = {
    .foreground = 1, .refined_x = 20, .refined_y = 25, .strength = 200,
  };
  GoodixChicagoEnrollmentResult inserted;
  GoodixChicagoMetricData metrics = {0};
  g_autoptr(GError) error = NULL;
  g_assert_true (goodix_chicago_enrollment_insert_first (enrollment, &record, 1,
                 1, 70, 90, &metrics, &inserted, &error));
  g_assert_no_error (error);
  state.expected_template = goodix_chicago_enrollment_pack (enrollment, &error);
  g_assert_no_error (error);
  g_autoptr(GVariant) envelope = gx_print_envelope_export (&SELF (device)->identity,
                               TRUE, state.expected_template, &error);
  g_assert_no_error (error);
  FpPrint *print = g_object_ref_sink (fp_print_new (device));
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", envelope, NULL);
  return print;
}

static GBytes *
serialized_print (FpPrint *print)
{
  guint8 *data = NULL;
  gsize length = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true (fp_print_serialize (print, &data, &length, &error));
  g_assert_no_error (error);
  return g_bytes_new_take (data, length);
}

static void
assert_print_unchanged (FpPrint *print, GBytes *original)
{
  g_autoptr(GBytes) serialized = serialized_print (print);
  g_assert_true (g_bytes_equal (serialized, original));
}

static void
public_verify_done (GObject *object, GAsyncResult *result, gpointer unused)
{
  g_clear_error (&state.public_error);
  state.public_success = fp_device_verify_finish (FP_DEVICE (object), result,
                        &state.public_match, NULL, &state.public_error);
  state.public_callbacks++;
}

static void
public_identify_done (GObject *object, GAsyncResult *result, gpointer unused)
{
  g_autoptr(FpPrint) match = NULL;
  g_clear_error (&state.public_error);
  state.public_success = fp_device_identify_finish (FP_DEVICE (object), result,
                         &match, NULL, &state.public_error);
  state.public_match = match != NULL;
  state.public_callbacks++;
}

static void
start_public_match (FpDevice *device, FpPrint *print, Action action)
{
  if (action == ACTION_VERIFY) {
    fp_device_verify (device, print, state.cancellable, NULL, NULL, NULL,
                      public_verify_done, NULL);
  } else {
    g_ptr_array_set_size (state.prints, 0);
    g_ptr_array_add (state.prints, print);
    fp_device_identify (device, state.prints, state.cancellable, NULL, NULL, NULL,
                        public_identify_done, NULL);
  }
}

static void
assert_action_idle (FpDevice *device)
{
  FpiDeviceGoodix521d *self = SELF (device);
  g_assert_cmpint (self->action, ==, ACTION_NONE);
  g_assert_false (self->processing);
  g_assert_false (self->finishing);
  g_assert_false (self->in_cancel);
  g_assert_false (self->transport_open);
  g_assert_null (self->boot);
  g_assert_null (self->templates);
  g_assert_null (self->deadline);
  g_assert_cmpint (self->deadline_us, ==, 0);
  g_assert_null (self->deferred_error);
  g_assert_null (self->abort_error);
}

static void
run_verification (FpDevice *device, FpPrint *print, guint8 frame_tag, gboolean cancel)
{
  guint next_callback = state.public_callbacks + 1;
  guint next_completion = state.completed[ACTION_VERIFY] + 1;
  guint next_matcher = state.matcher_calls + 1;
  guint reports_before = state.reports;
  state.checked_match_device = device;
  state.expected_match_session = SELF (device)->session;
  state.expected_stops_before_match = state.stops + 1;
  state.worker_entered = state.release_worker = FALSE;
  state.block_worker = cancel;
  fp_device_verify (device, print, state.cancellable, NULL, NULL, NULL,
                    public_verify_done, NULL);
  g_assert_cmpint (SELF (device)->action, ==, ACTION_VERIFY);
  g_assert_false (SELF (device)->cancelled);
  g_assert_false (SELF (device)->finishing);
  g_assert_null (SELF (device)->abort_error);
  g_assert_null (SELF (device)->deferred_error);
  g_assert_true (SELF (device)->transport_open);
  g_assert_cmpuint (SELF (device)->contacts, ==, 1);
  GxImage image = {0};
  image.valid = TRUE;
  image.pixels[0] = frame_tag; /* A distinct capture, never a replayed frame. */
  state.capture_callback (device, &image, state.capture_data, NULL);
  if (cancel) {
    gboolean entered = FALSE;
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    while (!entered) {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      while (g_main_context_iteration (NULL, FALSE));
      g_mutex_lock (&state.mutex);
      entered = state.worker_entered;
      g_mutex_unlock (&state.mutex);
      if (!entered) g_usleep (1000);
    }
    g_cancellable_cancel (state.cancellable);
    while (g_main_context_iteration (NULL, FALSE));
    g_assert_true (SELF (device)->cancelled);
    g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, next_completion - 1);
    release_processing ();
  }
  wait_public_callbacks (next_callback);
  g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, next_completion);
  g_assert_cmpuint (state.matcher_calls, ==, next_matcher);
  g_assert_cmpuint (state.reports, ==, reports_before + (cancel ? 0 : 1));
  assert_action_idle (device);
  g_assert_true (state.usb_open);
  g_assert_true (fp_device_is_open (device));
  if (!cancel) {
    g_assert_true (SELF (device)->session == state.expected_match_session);
    g_assert_false (SELF (device)->cancelled);
  }
}

static void
test_public_repeated_verify_results (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  open_with_reference (device);
  OcSession *session = SELF (device)->session;
  g_autoptr(FpPrint) print = make_verification_print (device);
  g_autoptr(GBytes) original = serialized_print (print);
  for (guint attempt = 0; attempt < 5; attempt++) {
    state.matcher_nonmatch = attempt == 1;
    state.matcher_reject = attempt == 3 ? OC_REJECT_NO_FEATURES : OC_REJECT_NONE;
    run_verification (device, print, attempt + 1, FALSE);
    g_assert_true (SELF (device)->session == session);
    assert_print_unchanged (print, original);
    if (attempt == 3) {
      g_assert_false (state.public_success);
      g_assert_error (state.public_error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
      g_assert_cmpint (state.last_match_report, ==, FPI_MATCH_ERROR);
    } else {
      g_assert_true (state.public_success);
      g_assert_no_error (state.public_error);
      g_assert_cmpint (state.public_match, ==, attempt != 1);
      g_assert_cmpint (state.last_match_report, ==,
                       attempt == 1 ? FPI_MATCH_FAIL : FPI_MATCH_SUCCESS);
    }
  }
  g_assert_cmpuint (state.sessions_created, ==, 1);
  g_assert_cmpuint (state.sessions_freed, ==, 0);
  g_assert_cmpuint (state.exports, ==, 0);
  g_assert_cmpuint (state.starts, ==, 6); /* reference plus five captures */
  g_assert_cmpuint (state.stops, ==, 6);
  g_clear_error (&state.public_error);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_clear_object (&print);
  finish_state (device);
}

static void
test_public_cancel_verify_requires_reopen (void)
{
  reset_state ();
  FpDevice *device = new_public_device ();
  open_with_reference (device);
  g_autoptr(FpPrint) print = make_verification_print (device);
  g_autoptr(GBytes) original = serialized_print (print);
  run_verification (device, print, 1, TRUE);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (SELF (device)->session);
  g_assert_cmpuint (state.sessions_freed, ==, 1);
  assert_print_unchanged (print, original);

  /* Cancellation intentionally invalidates algorithm state. A verify on the
   * still-open object fails early rather than using that discarded session. */
  g_clear_object (&state.cancellable);
  state.cancellable = g_cancellable_new ();
  guint next_callback = state.public_callbacks + 1;
  guint starts_before = state.starts;
  fp_device_verify (device, print, state.cancellable, NULL, NULL, NULL,
                    public_verify_done, NULL);
  wait_public_callbacks (next_callback);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpuint (state.matcher_calls, ==, 1);
  g_assert_cmpuint (state.starts, ==, starts_before);
  g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, 2);
  g_assert_false (SELF (device)->cancelled);
  assert_action_idle (device);
  assert_print_unchanged (print, original);

  g_clear_error (&state.public_error);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  open_with_reference (device);
  g_assert_cmpuint (state.sessions_created, ==, 2);
  run_verification (device, print, 2, FALSE);
  g_assert_true (state.public_success);
  g_assert_true (state.public_match);
  g_assert_no_error (state.public_error);
  g_assert_cmpuint (state.completed[ACTION_VERIFY], ==, 3);
  assert_print_unchanged (print, original);
  g_assert_cmpuint (state.exports, ==, 0);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_clear_object (&print);
  finish_state (device);
}

static void
test_public_capture_error_can_match_again (gconstpointer data)
{
  Action action = GPOINTER_TO_INT (data);
  reset_state ();
  FpDevice *device = new_public_device ();
  open_with_reference (device);
  OcSession *session = SELF (device)->session;
  g_autoptr(FpPrint) print = make_verification_print (device);
  g_autoptr(GBytes) original = serialized_print (print);
  guint next_callback = state.public_callbacks + 1;
  start_public_match (device, print, action);
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                               "synthetic capture failure before matching"));
  wait_public_callbacks (next_callback);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_false (state.public_success);
  g_assert_cmpuint (state.matcher_calls, ==, 0);
  g_assert_cmpuint (state.completed[action], ==, 1);
  g_assert_true (fp_device_is_open (device));
  assert_action_idle (device);
  assert_print_unchanged (print, original);

  /* A capture failure has not touched the calibrated algorithm context. The
   * next real matching action on this same open object must reach acquisition again. */
  state.capture_callback = NULL;
  next_callback = state.public_callbacks + 1;
  start_public_match (device, print, action);
  g_assert_nonnull (state.capture_callback);
  g_assert_true (SELF (device)->session == session);
  state.checked_match_device = device;
  state.expected_match_session = session;
  state.expected_stops_before_match = state.stops + 1;
  GxImage image = {0};
  image.valid = TRUE;
  image.pixels[0] = 1;
  state.capture_callback (device, &image, state.capture_data, NULL);
  wait_public_callbacks (next_callback);
  g_assert_true (state.public_success);
  g_assert_true (state.public_match);
  g_assert_no_error (state.public_error);
  g_assert_cmpuint (state.matcher_calls, ==, 1);
  g_assert_cmpuint (state.completed[action], ==, 2);
  g_assert_cmpuint (state.sessions_created, ==, 1);
  g_assert_cmpuint (state.sessions_freed, ==, 0);
  assert_action_idle (device);
  assert_print_unchanged (print, original);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_clear_object (&print);
  finish_state (device);
}

static void
test_public_capture_cleanup_failure_discards_session (gconstpointer data)
{
  Action action = GPOINTER_TO_INT (data);
  reset_state ();
  FpDevice *device = new_public_device ();
  open_with_reference (device);
  g_autoptr(FpPrint) print = make_verification_print (device);
  g_autoptr(GBytes) original = serialized_print (print);
  guint next_callback = state.public_callbacks + 1;
  start_public_match (device, print, action);
  state.fail_cleanup = TRUE;
  state.capture_callback (device, NULL, state.capture_data,
                          g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                               "synthetic capture failure before matching"));
  wait_public_callbacks (next_callback);
  g_assert_false (state.public_success);
  g_assert_error (state.public_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_cmpuint (state.completed[action], ==, 1);
  g_assert_cmpuint (state.matcher_calls, ==, 0);
  g_assert_null (SELF (device)->session);
  g_assert_cmpuint (state.sessions_freed, ==, 1);
  g_assert_true (SELF (device)->transport_open);
  g_assert_cmpint (SELF (device)->action, ==, ACTION_NONE);
  g_assert_false (SELF (device)->processing);
  g_assert_false (SELF (device)->finishing);
  g_assert_null (SELF (device)->deferred_error);
  g_assert_true (fp_device_is_open (device));
  assert_print_unchanged (print, original);
  state.fail_cleanup = FALSE;
  g_clear_error (&state.public_error);
  g_assert_true (fp_device_close_sync (device, NULL, &state.public_error));
  g_assert_no_error (state.public_error);
  g_clear_object (&print);
  finish_state (device);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  const char *actions[] = { NULL, "preprocessing", "enroll", "verify", "identify" };
  for (Action action = ACTION_OPEN; action <= ACTION_IDENTIFY; action++) {
    g_autofree char *cancel_path = g_strdup_printf ("/goodix521d/lifecycle/cancel-during-%s", actions[action]);
    g_autofree char *deadline_path = g_strdup_printf ("/goodix521d/lifecycle/expired-%s-before-timeout-dispatch", actions[action]);
    g_autofree char *cancellable_path = g_strdup_printf ("/goodix521d/lifecycle/cancellable-during-%s", actions[action]);
    g_test_add_data_func (cancel_path, GINT_TO_POINTER (action), test_cancel_worker);
    g_test_add_data_func (cancellable_path, GINT_TO_POINTER (action), test_cancellable_worker);
    g_test_add_data_func (deadline_path, GINT_TO_POINTER (action), test_expired_worker);
    if (action == ACTION_ENROLL)
      continue;
    g_autofree char *success_path = g_strdup_printf ("/goodix521d/lifecycle/%s-completes-once", actions[action]);
    g_test_add_data_func (success_path, GINT_TO_POINTER (action), test_success_once);
    if (action == ACTION_OPEN)
      continue;
    g_autofree char *report_path = g_strdup_printf ("/goodix521d/lifecycle/cancel-in-%s-report", actions[action]);
    g_test_add_data_func (report_path, GINT_TO_POINTER (action), test_report_cancel);
  }
  g_test_add_func ("/goodix521d/lifecycle/progress-cancel-no-successor-capture", test_progress_cancel);
  g_test_add_func ("/goodix521d/lifecycle/open-failure-reopen-cleanup", test_open_failure_reopen);
  g_test_add_func ("/goodix521d/lifecycle/missing-open-input", test_open_missing_input);
  g_test_add_func ("/goodix521d/lifecycle/queued-capture-cancel", test_queued_capture_cancel);
  g_test_add_func ("/goodix521d/lifecycle/cleanup-failure-blocks-reopen", test_cleanup_failure_blocks_reopen);
  g_test_add_func ("/goodix521d/lifecycle/synchronous-cancel-unwind-completes-once", test_cancel_unwind_completes_once);
  const char *empty_modes[] = { "no-match", "retry", "matcher-error", "cancel", "deadline" };
  for (guint i = 0; i < G_N_ELEMENTS (empty_modes); i++) {
    g_autofree char *path = g_strdup_printf ("/goodix521d/identify/empty-gallery-%s", empty_modes[i]);
    g_test_add_data_func (path, GUINT_TO_POINTER (i), test_empty_identify);
  }
  g_test_add_data_func ("/goodix521d/identify/oversized-gallery", GINT_TO_POINTER (1), test_invalid_identify_gallery);
  g_test_add_data_func ("/goodix521d/identify/invalid-template", GINT_TO_POINTER (0), test_invalid_identify_gallery);
  g_test_add_func ("/goodix521d/lifecycle/public-failed-open-retries", test_public_failed_open_retries);
  g_test_add_func ("/goodix521d/lifecycle/public-cleanup-failure-retains-usb", test_public_failed_open_retains_usb_on_cleanup_failure);
  g_test_add_func ("/goodix521d/lifecycle/public-enroll-error-keeps-usb-open", test_public_enroll_error_keeps_usb_open);
  g_test_add_func ("/goodix521d/lifecycle/public-failed-open-waits-for-rx", test_public_failed_open_waits_for_rx);
  g_test_add_func ("/goodix521d/lifecycle/public-failed-open-drain-timeout", test_public_failed_open_drain_timeout);
  g_test_add_data_func ("/goodix521d/lifecycle/public-missing-input-closes-usb",
                        GINT_TO_POINTER (0), test_public_early_open_failure);
  g_test_add_data_func ("/goodix521d/lifecycle/public-claim-failure-closes-usb",
                        GINT_TO_POINTER (1), test_public_early_open_failure);
  g_test_add_func ("/goodix521d/lifecycle/public-cancel-during-failed-open-drain",
                   test_public_cancel_during_failed_open_drain);
  g_test_add_func ("/goodix521d/lifecycle/public-repeated-verify-results",
                   test_public_repeated_verify_results);
  g_test_add_func ("/goodix521d/lifecycle/public-cancel-verify-requires-reopen",
                   test_public_cancel_verify_requires_reopen);
  g_test_add_data_func ("/goodix521d/lifecycle/public-capture-error-can-verify-again",
                        GINT_TO_POINTER (ACTION_VERIFY), test_public_capture_error_can_match_again);
  g_test_add_data_func ("/goodix521d/lifecycle/public-capture-error-can-identify-again",
                        GINT_TO_POINTER (ACTION_IDENTIFY), test_public_capture_error_can_match_again);
  g_test_add_data_func ("/goodix521d/lifecycle/public-verify-cleanup-failure-discards-session",
                        GINT_TO_POINTER (ACTION_VERIFY), test_public_capture_cleanup_failure_discards_session);
  g_test_add_data_func ("/goodix521d/lifecycle/public-identify-cleanup-failure-discards-session",
                        GINT_TO_POINTER (ACTION_IDENTIFY), test_public_capture_cleanup_failure_discards_session);
  return g_test_run ();
}
