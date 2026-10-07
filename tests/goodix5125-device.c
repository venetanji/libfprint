/*
 * Goodix 27c6:5125 driver state machine against a fake transport
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "drivers_api.h"
#include "goodix5125-usb-async.h"
#include "goodix5125-usb.h"

/* ---------------------------------------------------------------------
 * Fake sensor: answers the FDT commands of the capture cycle, keeps the
 * finger wait (FDT-down without timeout) and GET_IMAGE pending until the
 * test completes them, and fails everything once unplugged.
 * --------------------------------------------------------------------- */

typedef struct
{
  GArray  *commands;
  GTask   *wait;
  gulong   wait_cancel_id;
  GTask   *capture;
  guint    n_waits;
  gboolean unplugged;
} FakeSensor;

static FakeSensor fake;

/* FDT reply: no touch, valid thresholds on all six channels */
static const guint8 fdt_reply_off[] = {
  0x02, 0x00, 0x00, 0x00,
  0x22, 0x01, 0x7f, 0x01, 0xbe, 0x00,
  0x5e, 0x01, 0xcf, 0x00, 0x56, 0x01,
};

/* FDT-down reply: touch on all channels */
static const guint8 fdt_reply_down[] = {
  0x02, 0x00, 0x3f, 0x00,
  0x22, 0x01, 0x7f, 0x01, 0xbe, 0x00,
  0x5e, 0x01, 0xcf, 0x00, 0x56, 0x01,
};

static void
fake_reset (void)
{
  g_assert_null (fake.wait);
  g_assert_null (fake.capture);
  g_clear_pointer (&fake.commands, g_array_unref);
  memset (&fake, 0, sizeof (fake));
  fake.commands = g_array_new (FALSE, FALSE, sizeof (guint8));
}

static void
fake_wait_return (GError       *error,
                  const guint8 *reply,
                  gsize         reply_length)
{
  g_autoptr(GTask) task = g_steal_pointer (&fake.wait);

  g_assert_nonnull (task);
  g_cancellable_disconnect (g_task_get_cancellable (task),
                            fake.wait_cancel_id);
  fake.wait_cancel_id = 0;
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, g_bytes_new (reply, reply_length),
                           (GDestroyNotify) g_bytes_unref);
}

/* The transfer is cancelled asynchronously, as by libusb */
static gboolean
fake_wait_cancelled_idle_cb (gpointer user_data)
{
  if (fake.wait)
    fake_wait_return (g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                           "Fake transfer cancelled"),
                      NULL, 0);
  return G_SOURCE_REMOVE;
}

static void
fake_wait_cancelled_cb (GCancellable *cancellable,
                        gpointer      user_data)
{
  g_idle_add (fake_wait_cancelled_idle_cb, NULL);
}

static void
fake_touch (void)
{
  fake_wait_return (NULL, fdt_reply_down, sizeof (fdt_reply_down));
}

static void
fake_capture_fail (void)
{
  g_autoptr(GTask) task = g_steal_pointer (&fake.capture);

  g_assert_nonnull (task);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Fake image transfer failed");
}

void
goodix5125_usb_command_async (FpDevice              *device,
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
                              gpointer               user_data)
{
  g_autoptr(GTask) task = g_task_new (device, cancellable, callback,
                                      user_data);
  guint8 code = command;

  g_task_set_source_tag (task, goodix5125_usb_command_async);
  g_array_append_val (fake.commands, code);

  if (fake.unplugged)
    {
      g_task_return_new_error (task, G_USB_DEVICE_ERROR,
                               G_USB_DEVICE_ERROR_NO_DEVICE,
                               "Fake device is unplugged");
      return;
    }
  if (g_task_return_error_if_cancelled (task))
    return;

  switch ((guint) command)
    {
    case GOODIX5125_COMMAND_FDT_MODE:
      g_task_return_pointer (task,
                             g_bytes_new (fdt_reply_off,
                                          sizeof (fdt_reply_off)),
                             (GDestroyNotify) g_bytes_unref);
      break;

    case GOODIX5125_COMMAND_FDT_DOWN:
      if (timeout_ms != 0)
        {
          /* the probe: no reply is the normal case */
          g_task_return_new_error (task, G_USB_DEVICE_ERROR,
                                   G_USB_DEVICE_ERROR_TIMED_OUT,
                                   "Fake transfer timed out");
          break;
        }
      g_assert_nonnull (cancellable);
      g_assert_null (fake.wait);
      fake.n_waits++;
      fake.wait = g_steal_pointer (&task);
      fake.wait_cancel_id = g_cancellable_connect (
        cancellable, G_CALLBACK (fake_wait_cancelled_cb), NULL, NULL);
      break;

    case GOODIX5125_COMMAND_QUERY_STATE:
      {
        static const guint8 state[16] = { 0 };

        g_task_return_pointer (task, g_bytes_new (state, sizeof (state)),
                               (GDestroyNotify) g_bytes_unref);
        break;
      }

    case GOODIX5125_COMMAND_GET_IMAGE:
      g_assert_null (fake.capture);
      fake.capture = g_steal_pointer (&task);
      break;

    default:
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "Fake sensor does not know command 0x%02x",
                               command);
    }
}

GBytes *
goodix5125_usb_command_finish (FpDevice     *device,
                               GAsyncResult *result,
                               GError      **error)
{
  g_assert_true (g_task_is_valid (result, device));
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* The sensor is never initialised here: the tests mark the device as
 * initialised, the synchronous commands must not be reached. */

static GBytes *
fake_sync_unexpected (GError **error)
{
  g_test_message ("unexpected synchronous Goodix command");
  g_test_fail ();
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Unexpected synchronous Goodix command");
  return NULL;
}

GBytes *
goodix5125_usb_command_sync (FpDevice              *device,
                             Goodix5125Command      command,
                             Goodix5125ChecksumMode request_checksum_mode,
                             Goodix5125ChecksumMode reply_checksum_mode,
                             const guint8          *payload,
                             gsize                  payload_length,
                             gboolean               expect_reply,
                             Goodix5125OuterFlag    reply_flag,
                             guint                  timeout_ms,
                             GError               **error)
{
  return fake_sync_unexpected (error);
}

gboolean
goodix5125_usb_drain_sync (FpDevice *device,
                           guint     timeout_ms,
                           GError  **error)
{
  return fake_sync_unexpected (error) != NULL;
}

gboolean
goodix5125_usb_nop_sync (FpDevice *device,
                         guint     timeout_ms,
                         GError  **error)
{
  return fake_sync_unexpected (error) != NULL;
}

gboolean
goodix5125_usb_outer_write_sync (FpDevice           *device,
                                 Goodix5125OuterFlag flag,
                                 const guint8       *payload,
                                 gsize               payload_length,
                                 guint               timeout_ms,
                                 GError            **error)
{
  return fake_sync_unexpected (error) != NULL;
}

GBytes *
goodix5125_usb_outer_read_sync (FpDevice           *device,
                                Goodix5125OuterFlag expected_flag,
                                guint               timeout_ms,
                                GError            **error)
{
  return fake_sync_unexpected (error);
}

/* The device is virtual here: no GUsbDevice to claim or release. */

static GUsbDevice *
fake_get_usb_device (FpDevice *device)
{
  return NULL;
}

static gboolean
fake_claim_interface (GUsbDevice                   *device,
                      gint                          interface,
                      GUsbDeviceClaimInterfaceFlags flags,
                      GError                      **error)
{
  return TRUE;
}

static gboolean
fake_release_interface (GUsbDevice                   *device,
                        gint                          interface,
                        GUsbDeviceClaimInterfaceFlags flags,
                        GError                      **error)
{
  if (!fake.unplugged)
    return TRUE;
  g_set_error_literal (error, G_USB_DEVICE_ERROR,
                       G_USB_DEVICE_ERROR_NO_DEVICE,
                       "Fake device is unplugged");
  return FALSE;
}

#define fpi_device_get_usb_device fake_get_usb_device
#define g_usb_device_claim_interface fake_claim_interface
#define g_usb_device_release_interface fake_release_interface

/* ---------------------------------------------------------------------
 * Scripted matcher: the power-button tests decide per frame whether it is
 * unusable, matches nothing or matches the first print.
 * --------------------------------------------------------------------- */

#include "goodix5125-algo.h"

typedef enum {
  FAKE_MATCH_RETRY,
  FAKE_MATCH_NONE,
  FAKE_MATCH_FIRST,
} FakeMatch;

static FakeMatch fake_matches[3];   /* GOODIX5125_POV_MAX_FRAMES */
static guint n_fake_matches;
static guint n_fake_match_calls;

static gboolean
fake_algo_match (Goodix5125Algo      *algo,
                 const guint16        frame[OC_FRAME_PIXELS],
                 GVariant *const     *prints,
                 guint                n_prints,
                 Goodix5125MatchStep *step,
                 GError             **error)
{
  g_assert_cmpuint (n_fake_match_calls, <, n_fake_matches);
  memset (step, 0, sizeof (*step));
  step->matched = -1;
  switch (fake_matches[n_fake_match_calls++])
    {
    case FAKE_MATCH_RETRY:
      step->retry = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
      break;

    case FAKE_MATCH_NONE:
      break;

    case FAKE_MATCH_FIRST:
      step->matched = 0;
      break;
    }
  return TRUE;
}

#define goodix5125_algo_match fake_algo_match
#include "drivers/goodix5125/goodix5125.c"

G_STATIC_ASSERT (G_N_ELEMENTS (fake_matches) == GOODIX5125_POV_MAX_FRAMES);

/* ---------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

typedef struct
{
  gboolean done;
  GError  *error;
} Result;

static gboolean timed_out;

static gboolean
deadline_cb (gpointer user_data)
{
  timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

/* Runs the main loop until *@flag or *@pointer is set */
static void
iterate_until (const gboolean *flag,
               gpointer const *pointer)
{
  guint deadline_id = g_timeout_add_seconds (5, deadline_cb, NULL);

  while (!(flag && *flag) && !(pointer && *pointer) && !timed_out)
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (timed_out);
  g_source_remove (deadline_id);
}

static void
iterate_pending (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    continue;
}

static void
verify_cb (GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  Result *res = user_data;

  fp_device_verify_finish (FP_DEVICE (source_object), result, NULL, NULL,
                           &res->error);
  res->done = TRUE;
}

static void
suspend_cb (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data)
{
  Result *res = user_data;

  fp_device_suspend_finish (FP_DEVICE (source_object), result, &res->error);
  res->done = TRUE;
}

static void
removed_cb (FpDevice *device,
            gpointer  user_data)
{
  *(gboolean *) user_data = TRUE;
}

static FpDevice *
open_device (void)
{
  g_autoptr(GError) error = NULL;
  FpDeviceClass *cls;
  FpDevice *device;

  fake_reset ();
  n_fake_matches = 0;
  n_fake_match_calls = 0;

  cls = g_type_class_ref (fpi_device_goodix5125_get_type ());
  cls->type = FP_DEVICE_TYPE_VIRTUAL;
  device = g_object_new (fpi_device_goodix5125_get_type (), NULL);
  g_type_class_unref (cls);

  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* the cycle under test only uses the FDT and image commands */
  FPI_DEVICE_GOODIX5125 (device)->initialized = TRUE;

  return device;
}

static void
close_device (FpDevice *device)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_object_unref (device);
  g_assert_null (fake.wait);
  g_assert_null (fake.capture);
}

static void
start_verify (FpDevice     *device,
              GCancellable *cancellable,
              Result       *verify)
{
  g_autoptr(FpPrint) print = fp_print_new (device);

  fp_device_verify (device, print, cancellable, NULL, NULL, NULL,
                    verify_cb, verify);
  iterate_until (NULL, (gpointer *) &fake.wait);
  g_assert_false (verify->done);
}

static void
assert_commands (guint first, ...)
{
  va_list args;
  guint i = first;
  gint command;

  va_start (args, first);
  while ((command = va_arg (args, gint)) >= 0)
    {
      g_assert_cmpuint (i, <, fake.commands->len);
      g_assert_cmphex (g_array_index (fake.commands, guint8, i), ==, command);
      i++;
    }
  va_end (args);
  g_assert_cmpuint (i, ==, fake.commands->len);
}

/* ---------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

static void
test_suspend_resume_finger_wait (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  FpDevice *device = open_device ();
  guint n_commands;

  start_verify (device, cancellable, &verify);
  assert_commands (0, GOODIX5125_COMMAND_FDT_MODE,
                   GOODIX5125_COMMAND_FDT_DOWN,
                   GOODIX5125_COMMAND_QUERY_STATE,
                   GOODIX5125_COMMAND_FDT_DOWN, -1);

  /* the finger wait ends, the verify stays active over the suspend */
  g_assert_true (fp_device_suspend_sync (device, NULL, &error));
  g_assert_no_error (error);
  iterate_pending ();
  g_assert_null (fake.wait);
  g_assert_false (verify.done);
  n_commands = fake.commands->len;

  /* resume waits for the lift, then arms the finger detection again */
  g_assert_true (fp_device_resume_sync (device, NULL, &error));
  g_assert_no_error (error);
  iterate_until (NULL, (gpointer *) &fake.wait);
  g_assert_cmpuint (fake.n_waits, ==, 2);
  g_assert_false (verify.done);
  assert_commands (n_commands, GOODIX5125_COMMAND_FDT_MODE,
                   GOODIX5125_COMMAND_FDT_MODE,
                   GOODIX5125_COMMAND_FDT_DOWN,
                   GOODIX5125_COMMAND_QUERY_STATE,
                   GOODIX5125_COMMAND_FDT_DOWN, -1);

  g_cancellable_cancel (cancellable);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&verify.error);

  close_device (device);
}

static void
test_suspend_during_capture (void)
{
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  Result suspend = { 0 };
  FpDevice *device = open_device ();

  start_verify (device, NULL, &verify);
  fake_touch ();
  iterate_until (NULL, (gpointer *) &fake.capture);

  /* the image transfer is not interrupted, the cycle stops after it */
  fp_device_suspend (device, NULL, suspend_cb, &suspend);
  iterate_pending ();
  g_assert_false (suspend.done);
  g_assert_nonnull (fake.capture);

  fake_capture_fail ();
  iterate_until (&suspend.done, NULL);
  g_assert_no_error (suspend.error);
  iterate_pending ();
  g_assert_false (verify.done);

  /* the error is reported after resume, once the finger is lifted */
  g_assert_true (fp_device_resume_sync (device, NULL, &error));
  g_assert_no_error (error);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&verify.error);
  g_assert_cmphex (g_array_index (fake.commands, guint8,
                                  fake.commands->len - 1), ==,
                   GOODIX5125_COMMAND_FDT_MODE);

  close_device (device);
}

static void
test_cancel_while_suspended (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  FpDevice *device = open_device ();

  start_verify (device, cancellable, &verify);
  g_assert_true (fp_device_suspend_sync (device, NULL, &error));
  g_assert_no_error (error);

  /* nothing is in flight while suspended, the driver ends the action */
  g_cancellable_cancel (cancellable);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&verify.error);

  g_assert_true (fp_device_resume_sync (device, NULL, &error));
  g_assert_no_error (error);
  iterate_pending ();
  g_assert_null (fake.wait);

  close_device (device);
}

static void
test_cancel_during_pending_suspend (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  Result suspend = { 0 };
  FpDevice *device = open_device ();

  start_verify (device, cancellable, &verify);
  fake_touch ();
  iterate_until (NULL, (gpointer *) &fake.capture);
  fp_device_suspend (device, NULL, suspend_cb, &suspend);
  iterate_pending ();
  g_assert_false (suspend.done);

  /* a cancelled action ends at once, the suspend completes after it */
  g_cancellable_cancel (cancellable);
  fake_capture_fail ();
  iterate_until (&verify.done, NULL);
  iterate_until (&suspend.done, NULL);
  g_assert_nonnull (verify.error);
  g_clear_error (&verify.error);
  g_assert_no_error (suspend.error);
  g_assert_null (fake.wait);

  g_assert_true (fp_device_resume_sync (device, NULL, &error));
  g_assert_no_error (error);
  iterate_pending ();
  g_assert_null (fake.wait);

  close_device (device);
}

static void
test_remove_during_finger_wait (void)
{
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  gboolean removed = FALSE;
  FpDevice *device = open_device ();

  g_signal_connect (device, "removed", G_CALLBACK (removed_cb), &removed);
  start_verify (device, NULL, &verify);

  /* the pending transfer is not failed by the fake: the driver must end
   * the finger wait itself */
  fake.unplugged = TRUE;
  fpi_device_remove (device);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_clear_error (&verify.error);
  iterate_until (&removed, NULL);
  g_assert_null (fake.wait);

  /* close completes although the interface cannot be released */
  g_assert_false (fp_device_close_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_assert_false (fp_device_is_open (device));
  g_object_unref (device);
}

static void
test_unplug_before_removal (void)
{
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  gboolean removed = FALSE;
  FpDevice *device = open_device ();

  g_signal_connect (device, "removed", G_CALLBACK (removed_cb), &removed);
  start_verify (device, NULL, &verify);

  /* libusb fails the finger wait before the removal reaches libfprint */
  fake.unplugged = TRUE;
  fake_wait_return (g_error_new_literal (G_USB_DEVICE_ERROR,
                                         G_USB_DEVICE_ERROR_NO_DEVICE,
                                         "Fake device is unplugged"),
                    NULL, 0);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_clear_error (&verify.error);

  fpi_device_remove (device);
  iterate_until (&removed, NULL);
  g_assert_false (fp_device_close_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_assert_false (fp_device_is_open (device));
  g_object_unref (device);
}

static void
test_remove_while_suspended (void)
{
  g_autoptr(GError) error = NULL;
  Result verify = { 0 };
  gboolean removed = FALSE;
  FpDevice *device = open_device ();

  g_signal_connect (device, "removed", G_CALLBACK (removed_cb), &removed);
  start_verify (device, NULL, &verify);
  g_assert_true (fp_device_suspend_sync (device, NULL, &error));
  g_assert_no_error (error);

  /* USB re-enumeration during hibernation: the parked action fails */
  fake.unplugged = TRUE;
  fpi_device_remove (device);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_clear_error (&verify.error);
  iterate_until (&removed, NULL);

  /* Whether libfprint lets a device removed while suspended be resumed and
   * closed is up to the core, not to the driver. */
  fp_device_resume_sync (device, NULL, NULL);
  if (!fp_device_close_sync (device, NULL, &error))
    g_test_message ("close after removal while suspended: %s",
                    error->message);
  if (fp_device_is_open (device))
    g_test_expect_message ("libfprint-device", G_LOG_LEVEL_WARNING,
                           "*destroyed open device*");
  g_object_unref (device);
  g_test_assert_expected_messages ();
}

/* ---------------------------------------------------------------------
 * Power-button frames
 * --------------------------------------------------------------------- */

typedef struct
{
  gboolean done;
  gboolean match;
  FpPrint *print;
  GError  *error;
} MatchResult;

static void
verify_match_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  MatchResult *res = user_data;

  fp_device_verify_finish (FP_DEVICE (source_object), result, &res->match,
                           NULL, &res->error);
  res->done = TRUE;
}

static void
identify_match_cb (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  MatchResult *res = user_data;

  fp_device_identify_finish (FP_DEVICE (source_object), result, &res->print,
                             NULL, &res->error);
  res->done = TRUE;
}

static void
match_reported_cb (FpDevice *device,
                   FpPrint  *match,
                   FpPrint  *print,
                   gpointer  user_data,
                   GError   *error)
{
  *(gboolean *) user_data = TRUE;
}

static void
set_pov_frames (FpDevice *device,
                gint64    age_us,
                FakeMatch first,
                ...)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);
  va_list args;
  gint result = first;

  va_start (args, first);
  for (n_fake_matches = 0; result >= 0; n_fake_matches++)
    {
      fake_matches[n_fake_matches] = result;
      result = va_arg (args, gint);
    }
  va_end (args);

  self->n_pov_frames = n_fake_matches;
  for (guint f = 0; f < n_fake_matches; f++)
    for (guint i = 0; i < OC_FRAME_PIXELS; i++)
      self->pov_frames[f][i] = 1500 + (i * (f + 3)) % 400;
  self->pov_time = g_get_monotonic_time () - age_us;
}

static void
assert_pov_frames_cleared (FpDevice *device)
{
  FpDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (device);

  g_assert_cmpuint (self->n_pov_frames, ==, 0);
  for (guint f = 0; f < GOODIX5125_POV_MAX_FRAMES; f++)
    for (guint i = 0; i < OC_FRAME_PIXELS; i++)
      g_assert_cmpuint (self->pov_frames[f][i], ==, 0);
}

/* A matching frame completes the verify without a touch, but not before
 * fp_device_verify () has returned (see pov_match_report_cb ()) */
static void
test_pov_frames_verify_match (void)
{
  MatchResult verify = { 0 };
  gboolean reported = FALSE;

  g_autoptr(FpPrint) print = NULL;
  FpDevice *device = open_device ();

  print = fp_print_new (device);
  set_pov_frames (device, 0, FAKE_MATCH_RETRY, FAKE_MATCH_NONE,
                  FAKE_MATCH_FIRST, -1);
  fp_device_verify (device, print, NULL, match_reported_cb, &reported, NULL,
                    verify_match_cb, &verify);
  g_assert_false (reported);
  iterate_until (&verify.done, NULL);
  g_assert_true (reported);
  g_assert_no_error (verify.error);
  g_assert_true (verify.match);
  g_assert_cmpuint (n_fake_match_calls, ==, 3);
  g_assert_cmpuint (fake.commands->len, ==, 0);
  assert_pov_frames_cleared (device);

  close_device (device);
}

static void
test_pov_frames_identify_match (void)
{
  g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (
    g_object_unref);
  MatchResult identify = { 0 };
  FpDevice *device = open_device ();

  g_ptr_array_add (gallery, fp_print_new (device));
  g_ptr_array_add (gallery, fp_print_new (device));
  set_pov_frames (device, 0, FAKE_MATCH_FIRST, FAKE_MATCH_NONE, -1);
  fp_device_identify (device, gallery, NULL, NULL, NULL, NULL,
                      identify_match_cb, &identify);
  iterate_until (&identify.done, NULL);
  g_assert_no_error (identify.error);
  g_assert_true (identify.print == g_ptr_array_index (gallery, 0));
  g_assert_cmpuint (n_fake_match_calls, ==, 1);
  g_assert_cmpuint (fake.commands->len, ==, 0);
  assert_pov_frames_cleared (device);
  g_clear_object (&identify.print);

  close_device (device);
}

/* Frames that are unusable or do not match are not reported: the verify
 * waits for a touch as without them */
static void
test_pov_frames_no_match (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  Result verify = { 0 };
  FpDevice *device = open_device ();

  set_pov_frames (device, 0, FAKE_MATCH_RETRY, FAKE_MATCH_NONE, -1);
  start_verify (device, cancellable, &verify);
  g_assert_cmpuint (n_fake_match_calls, ==, 2);
  assert_pov_frames_cleared (device);
  assert_commands (0, GOODIX5125_COMMAND_FDT_MODE,
                   GOODIX5125_COMMAND_FDT_DOWN,
                   GOODIX5125_COMMAND_QUERY_STATE,
                   GOODIX5125_COMMAND_FDT_DOWN, -1);

  g_cancellable_cancel (cancellable);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&verify.error);
  close_device (device);
}

/* Frames that are too old are dropped without a match */
static void
test_pov_frames_expired (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  Result verify = { 0 };
  FpDevice *device = open_device ();

  set_pov_frames (device, GOODIX5125_POV_VALID_US + 1, FAKE_MATCH_FIRST, -1);
  start_verify (device, cancellable, &verify);
  g_assert_cmpuint (n_fake_match_calls, ==, 0);
  assert_pov_frames_cleared (device);

  g_cancellable_cancel (cancellable);
  iterate_until (&verify.done, NULL);
  g_assert_error (verify.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&verify.error);
  close_device (device);
}

int
main (int argc, char **argv)
{
  int result;

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix5125/device/suspend-resume-finger-wait",
                   test_suspend_resume_finger_wait);
  g_test_add_func ("/goodix5125/device/suspend-during-capture",
                   test_suspend_during_capture);
  g_test_add_func ("/goodix5125/device/cancel-while-suspended",
                   test_cancel_while_suspended);
  g_test_add_func ("/goodix5125/device/cancel-during-pending-suspend",
                   test_cancel_during_pending_suspend);
  g_test_add_func ("/goodix5125/device/remove-during-finger-wait",
                   test_remove_during_finger_wait);
  g_test_add_func ("/goodix5125/device/unplug-before-removal",
                   test_unplug_before_removal);
  g_test_add_func ("/goodix5125/device/remove-while-suspended",
                   test_remove_while_suspended);
  g_test_add_func ("/goodix5125/device/pov-frames-verify-match",
                   test_pov_frames_verify_match);
  g_test_add_func ("/goodix5125/device/pov-frames-identify-match",
                   test_pov_frames_identify_match);
  g_test_add_func ("/goodix5125/device/pov-frames-no-match",
                   test_pov_frames_no_match);
  g_test_add_func ("/goodix5125/device/pov-frames-expired",
                   test_pov_frames_expired);
  result = g_test_run ();
  g_clear_pointer (&fake.commands, g_array_unref);
  return result;
}
