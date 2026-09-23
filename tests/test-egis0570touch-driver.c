/* Synthetic capture/worker tests, not biometric accuracy tests. LGPL-2.1-or-later.
 * Include the driver to exercise its private state without exporting test APIs.
 * USB entry points are replaced; no physical reader or fingerprint data is used.
 */
#include "drivers/egis0570touch.c"

static void
fake_open (FpDevice *dev)
{
  fpi_device_open_complete (dev, NULL);
}
static void
fake_close (FpDevice *dev)
{
  fpi_device_close_complete (dev, NULL);
}
static void
fake_start (FpDevice *dev)
{
}

static FpDeviceEgis0570Touch *
new_device (void)
{
  g_autoptr(FpDeviceClass) klass = g_type_class_ref (fpi_device_egis0570touch_get_type ());
  klass->type = FP_DEVICE_TYPE_VIRTUAL;
  klass->open = fake_open;
  klass->close = fake_close;
  klass->verify = fake_start;
  klass->enroll = fake_start;
  FpDeviceEgis0570Touch *self = g_object_new (fpi_device_egis0570touch_get_type (), NULL);
  g_assert_true (fp_device_open_sync (FP_DEVICE (self), NULL, NULL));
  return self;
}

static void
discard_operation (GObject *object, GAsyncResult *result, gpointer user_data)
{
  gboolean *done = user_data;

  g_autoptr(GError) error = NULL;
  g_assert_false (g_task_propagate_boolean (G_TASK (result), &error));
  g_assert_nonnull (error);
  *done = TRUE;
}

static void
capture (gconstpointer action_ptr)
{
  FpiDeviceAction action = GPOINTER_TO_INT (action_ptr);

  g_autoptr(FpDeviceEgis0570Touch) self = new_device ();
  FpDevice *dev = FP_DEVICE (self);
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (dev));
  gboolean done = FALSE;
  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      fp_device_enroll (dev, print, NULL, NULL, NULL, NULL, discard_operation, &done);
    }
  else
    {
      fpi_print_set_type (print, FPI_PRINT_RAW);
      fp_device_verify (dev, print, NULL, NULL, NULL, NULL, discard_operation, &done);
    }
  g_assert_cmpint (fpi_device_get_current_action (dev), ==, action);

  /* First empty packet calibrates; two foreground packets yield ten frames. */
  memset (self->packet, 0, sizeof (self->packet));
  g_assert_false (process_packet (dev));
  memset (self->packet, 100, sizeof (self->packet));
  g_assert_false (process_packet (dev));
  g_assert_cmpuint (self->frame_count, ==, 5);
  g_assert_cmpint (process_packet (dev), ==, action != FPI_DEVICE_ACTION_ENROLL);
  g_assert_cmpuint (self->frame_count, ==, AUTH_FRAMES);
  g_assert_true (self->present);

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      for (guint i = 0; i < 20; i++)
        g_assert_false (process_packet (dev));
      g_assert_cmpuint (self->frame_count, ==, MAX_FRAMES);
    }
  else
    {
      /* A subsequent operation must not collect the same held touch again. */
      self->wait_for_removal = self->present;
      self->frame_count = 0;
      for (guint i = 0; i < 3; i++)
        g_assert_false (process_packet (dev));
      g_assert_cmpuint (self->frame_count, ==, 0);
    }
  memset (self->packet, 0, sizeof (self->packet));
  g_assert_cmpint (process_packet (dev), ==, action == FPI_DEVICE_ACTION_ENROLL);
  g_assert_false (self->present);
  g_assert_false (self->wait_for_removal);
  self->frame_count = 0;
  memset (self->packet, 100, sizeof (self->packet));
  g_assert_false (process_packet (dev));
  g_assert_cmpuint (self->frame_count, ==, 5);

  fpi_device_action_error (dev, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
  while (!done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (fp_device_close_sync (dev, NULL, NULL));
}

static void
worker_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
  GAsyncResult **saved = user_data;

  *saved = g_object_ref (result);
}

static void
worker (gconstpointer cancel_ptr)
{
  gboolean cancel = GPOINTER_TO_INT (cancel_ptr);

  g_autoptr(FpDeviceEgis0570Touch) self = new_device ();
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GTask) task = g_task_new (self, cancellable, worker_done, &result);
  g_autoptr(GError) error = NULL;
  MatchData *data = g_new0 (MatchData, 1);
  data->action = FPI_DEVICE_ACTION_VERIFY;
  data->templates = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_task_set_task_data (task, data, (GDestroyNotify) match_data_free);
  self->frame_count = AUTH_FRAMES;
  memset (self->frames, 0, AUTH_FRAMES * FRAME_PIXELS * sizeof (*self->frames));
  if (cancel)
    g_cancellable_cancel (cancellable);
  g_task_run_in_thread (task, match_frames);
  while (!result)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (g_task_propagate_int (G_TASK (result), &error), ==, -1);
  if (cancel)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else
    g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  g_assert_true (fp_device_close_sync (FP_DEVICE (self), NULL, NULL));
}

static void
worker_match (gconstpointer identify_ptr)
{
  gboolean identify = GPOINTER_TO_INT (identify_ptr);

  g_autoptr(FpDeviceEgis0570Touch) self = new_device ();
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GTask) task = g_task_new (self, NULL, worker_done, &result);
  MatchData *data = g_new0 (MatchData, 1);
  data->action = identify ? FPI_DEVICE_ACTION_IDENTIFY : FPI_DEVICE_ACTION_VERIFY;
  data->templates = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_task_set_task_data (task, data, (GDestroyNotify) match_data_free);
  guint32 state = 123456789;
  for (guint i = 0; i < FRAME_PIXELS; i++)
    {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      self->frames[i] = state & 255;
    }
  for (guint i = 1; i < AUTH_FRAMES; i++)
    memcpy (self->frames + i * FRAME_PIXELS, self->frames, FRAME_PIXELS * sizeof (*self->frames));
  self->frame_count = AUTH_FRAMES;
  g_autoptr(Egis0570TouchFeatures) features =
    fpi_egis0570touch_features_extract (self->frames, 1, self->background);
  g_assert_nonnull (features);
  g_autoptr(GBytes) blob = fpi_egis0570touch_features_serialise (features);
  g_assert_nonnull (blob);
  gsize size;
  gconstpointer bytes = g_bytes_get_data (blob, &size);
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < ENROLL_STAGES; i++)
    g_variant_builder_add_value (&builder, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, bytes, size, 1));
  if (identify)
    g_ptr_array_add (data->templates, g_variant_ref_sink (g_variant_new_tuple (NULL, 0)));
  g_ptr_array_add (data->templates, g_variant_ref_sink (g_variant_new ("(uaay)", 1u, &builder)));
  g_task_run_in_thread (task, match_frames);
  while (!result)
    g_main_context_iteration (NULL, TRUE);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint (g_task_propagate_int (G_TASK (result), &error), ==, identify ? 1 : 0);
  g_assert_no_error (error);
  g_assert_true (fp_device_close_sync (FP_DEVICE (self), NULL, NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/egis0570touch/held-touch", GINT_TO_POINTER (FPI_DEVICE_ACTION_VERIFY), capture);
  g_test_add_data_func ("/egis0570touch/enroll-release", GINT_TO_POINTER (FPI_DEVICE_ACTION_ENROLL), capture);
  g_test_add_data_func ("/egis0570touch/worker-empty", GINT_TO_POINTER (FALSE), worker);
  g_test_add_data_func ("/egis0570touch/worker-cancelled", GINT_TO_POINTER (TRUE), worker);
  g_test_add_data_func ("/egis0570touch/worker-verify", GINT_TO_POINTER (FALSE), worker_match);
  g_test_add_data_func ("/egis0570touch/worker-identify", GINT_TO_POINTER (TRUE), worker_match);
  return g_test_run ();
}
