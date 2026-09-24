/*
 * Egis 0570 press driver. LGPL-2.1-or-later.
 * USB protocol and initialization derived from the existing egis0570 driver
 * by Maxim Kolesnikov and Saeed/Ali Rk, and the Acer Swift 1 patch.
 */
#define FP_COMPONENT "egis0570"
#include "drivers_api.h"
#include "egis0570touch-proto.h"
#include "egis0570touch-match.h"

#define FRAME_PIXELS (EGIS0570_IMGWIDTH * EGIS0570_IMGHEIGHT)
#define MAX_FRAMES 50
#define AUTH_FRAMES 10
#define ENROLL_STAGES 12
#define MIN_FEATURES 20
#define RELEASE_FRAMES 5
/* Provisional research threshold, not a measured false-accept guarantee. */
#define MATCH_THRESHOLD 12

struct _FpDeviceEgis0570Touch
{
  FpDevice   parent;
  FpiSsm    *ssm;
  guint      command;
  gboolean   initializing;
  gboolean   calibrated;
  gboolean   present;
  gboolean   wait_for_removal;
  guint      absent_frames;
  guint      frame_count;
  guint16    background[FRAME_PIXELS];
  guint16   *frames;
  double     scores[MAX_FRAMES];
  guint8     packet[EGIS0570_INPSIZE];
  GPtrArray *blobs;
  gboolean   finished;
};
G_DECLARE_FINAL_TYPE (FpDeviceEgis0570Touch, fpi_device_egis0570touch,
                      FPI, DEVICE_EGIS0570TOUCH, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis0570Touch, fpi_device_egis0570touch, FP_TYPE_DEVICE);

enum { SEND_COMMAND, RECEIVE_REPLY, RECEIVE_FRAMES, PROCESS_FRAMES, MATCH_FRAMES, N_STATES };

/* Owned by a GTask. Templates are immutable snapshots; only the main thread
 * accesses FpPrint objects or reports results to libfprint. */
typedef struct
{
  FpiDeviceAction action;
  GPtrArray      *templates;
  GBytes         *enrollment;
} MatchData;

static void
match_data_free (MatchData *data)
{
  g_ptr_array_unref (data->templates);
  g_clear_pointer (&data->enrollment, g_bytes_unref);
  g_free (data);
}

static guint
compare_print (Egis0570TouchFeatures *probe, GVariant *data, gboolean stop_on_match,
               GCancellable *cancellable)
{
  g_autoptr(GVariant) samples = NULL;
  guint version;
  guint best = 0;
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(uaay)")))
    return 0;
  g_variant_get (data, "(u@aay)", &version, &samples);
  if (version != 1 || g_variant_n_children (samples) != ENROLL_STAGES)
    return 0;
  for (gsize i = 0; i < g_variant_n_children (samples); i++)
    {
      if (g_cancellable_is_cancelled (cancellable))
        break;
      g_autoptr(GVariant) sample = g_variant_get_child_value (samples, i);
      gsize size;
      const guint8 *bytes = g_variant_get_fixed_array (sample, &size, 1);
      g_autoptr(GBytes) blob = g_bytes_new (bytes, size);
      g_autoptr(Egis0570TouchFeatures) gallery = fpi_egis0570touch_features_deserialise (blob);
      guint forward = fpi_egis0570touch_features_compare (probe, gallery);
      guint reverse = fpi_egis0570touch_features_compare (gallery, probe);
      guint score = MIN (forward, reverse);
      fp_dbg ("Sample %zu score %u (experimental threshold %u)", i, score, MATCH_THRESHOLD);
      best = MAX (best, score);
      if (stop_on_match && best >= MATCH_THRESHOLD)
        break;
    }
  return best;
}

static void
report_retry (FpDevice *dev)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);
  GError *error = fpi_device_retry_new (self->present ? FP_DEVICE_RETRY_REMOVE_FINGER :
                                        FP_DEVICE_RETRY_GENERAL);

  switch ((gint) fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      fpi_device_enroll_progress (dev, self->blobs->len, NULL, error);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
      self->finished = TRUE;
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
      fpi_device_identify_report (dev, NULL, NULL, error);
      self->finished = TRUE;
      break;

    default:
      g_error_free (error);
      g_assert_not_reached ();
    }
}

static void
match_frames (GTask *task, gpointer source_object, gpointer task_data,
              GCancellable *cancellable)
{
  FpDeviceEgis0570Touch *self = source_object;
  MatchData *data = task_data;

  g_autoptr(Egis0570TouchFeatures) features = NULL;
  gint winner = -1;
  guint best = MATCH_THRESHOLD - 1;

  /* The USB state machine is paused until this task finishes. */
  if (g_task_return_error_if_cancelled (task))
    return;
  if (self->frame_count >= AUTH_FRAMES)
    features = fpi_egis0570touch_features_extract (self->frames, self->frame_count,
                                                   self->background);
  if (fpi_egis0570touch_features_keypoint_count (features) < MIN_FEATURES)
    {
      g_task_return_error (task, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }
  if (data->action == FPI_DEVICE_ACTION_ENROLL)
    {
      data->enrollment = fpi_egis0570touch_features_serialise (features);
      if (!data->enrollment)
        {
          g_task_return_error (task, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
          return;
        }
    }
  for (guint i = 0; i < data->templates->len; i++)
    {
      if (g_task_return_error_if_cancelled (task))
        return;
      guint score = compare_print (features, g_ptr_array_index (data->templates, i),
                                   data->action == FPI_DEVICE_ACTION_VERIFY, cancellable);
      if (score > best)
        {
          best = score;
          winner = i;
        }
    }
  g_task_return_int (task, winner);
}

static void
match_done (GObject *source_object, GAsyncResult *result, gpointer unused)
{
  FpDevice *dev = FP_DEVICE (source_object);
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);
  MatchData *data = g_task_get_task_data (G_TASK (result));

  g_autoptr(GError) error = NULL;
  gint winner = g_task_propagate_int (G_TASK (result), &error);

  if (error)
    {
      if (error->domain != FP_DEVICE_RETRY)
        {
          fpi_ssm_mark_failed (self->ssm, g_steal_pointer (&error));
          return;
        }
      report_retry (dev);
    }
  else
    {
      switch ((gint) data->action)
        {
        case FPI_DEVICE_ACTION_ENROLL:
          g_ptr_array_add (self->blobs, g_steal_pointer (&data->enrollment));
          fpi_device_enroll_progress (dev, self->blobs->len, NULL, NULL);
          self->finished = self->blobs->len == ENROLL_STAGES;
          break;

        case FPI_DEVICE_ACTION_VERIFY:
          fpi_device_verify_report (dev, winner >= 0 ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    NULL, NULL);
          self->finished = TRUE;
          break;

        case FPI_DEVICE_ACTION_IDENTIFY:
          {
            GPtrArray *prints;
            fpi_device_get_identify_data (dev, &prints);
            fpi_device_identify_report (dev, winner >= 0 ? g_ptr_array_index (prints, winner) : NULL,
                                        NULL, NULL);
            self->finished = TRUE;
            break;
          }

        default:
          g_assert_not_reached ();
        }
    }
  self->frame_count = 0;
  if (self->finished)
    {
      fpi_ssm_mark_completed (self->ssm);
    }
  else
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      self->initializing = FALSE;
      self->command = 0;
      fpi_ssm_jump_to_state (self->ssm, SEND_COMMAND);
    }
}

static gboolean
process_packet (FpDevice *dev)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);

  if (!self->calibrated)
    {
      for (guint i = 0; i < FRAME_PIXELS; i++)
        {
          self->background[i] = 255;
          for (guint k = 0; k < EGIS0570_IMGCOUNT; k++)
            self->background[i] = MIN (self->background[i], self->packet[k * FRAME_PIXELS + i]);
        }
      self->calibrated = TRUE;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      return FALSE;
    }
  for (guint k = 0; k < EGIS0570_IMGCOUNT && !self->finished; k++)
    {
      const guint8 *frame = self->packet + k * FRAME_PIXELS;
      double mean = 0;
      for (guint i = 20 * EGIS0570_IMGWIDTH; i < 37 * EGIS0570_IMGWIDTH; i++)
        mean += MAX (0, (int) frame[i] - self->background[i]);
      mean /= 17 * EGIS0570_IMGWIDTH;
      /* Do not treat one held finger as several operations after a retry. */
      if (self->wait_for_removal)
        {
          if (mean > EGIS0570_MIN_MEAN)
            {
              self->absent_frames = 0;
            }
          else if (++self->absent_frames >= RELEASE_FRAMES)
            {
              self->wait_for_removal = FALSE;
              self->present = FALSE;
              fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
            }
          continue;
        }
      if (mean > EGIS0570_MIN_MEAN)
        {
          self->absent_frames = 0;
          if (!self->present)
            {
              self->present = TRUE;
              fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
            }
          guint slot = self->frame_count;
          if (slot == MAX_FRAMES)
            {
              slot = 0;
              for (guint i = 1; i < MAX_FRAMES; i++)
                if (self->scores[i] < self->scores[slot])
                  slot = i;
              if (mean <= self->scores[slot])
                continue;
            }
          else
            {
              self->frame_count++;
            }
          self->scores[slot] = mean;
          for (guint i = 0; i < FRAME_PIXELS; i++)
            self->frames[slot * FRAME_PIXELS + i] = frame[i];
          /* Authentication uses one batch; enrollment waits for removal. */
          if (self->frame_count == AUTH_FRAMES &&
              fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL)
            return TRUE;
        }
      else if (self->present && ++self->absent_frames >= RELEASE_FRAMES)
        {
          self->present = FALSE;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
          return TRUE;
        }
    }
  return FALSE;
}

static void
receive_frames_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer unused, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);
  memcpy (self->packet, transfer->buffer, sizeof (self->packet));
  fpi_ssm_next_state (transfer->ssm);
}

static void
run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);
  FpiUsbTransfer *transfer;
  guint total = self->initializing ? EGIS0570_INIT_TOTAL : EGIS0570_REPEAT_TOTAL;

  /* Drain each complete request/response cycle before honoring cancellation. */
  if ((fpi_ssm_get_cur_state (ssm) == PROCESS_FRAMES ||
       (fpi_ssm_get_cur_state (ssm) == SEND_COMMAND && self->command == 0)) &&
      fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled"));
      return;
    }
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SEND_COMMAND:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPOUT, EGIS0570_PKTSIZE);
      memcpy (transfer->buffer, self->initializing ? init_pkts[self->command] : repeat_pkts[self->command], EGIS0570_PKTSIZE);
      fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, fpi_ssm_usb_transfer_cb, NULL);
      break;

    case RECEIVE_REPLY:
      if (self->command == total - 1)
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          transfer = fpi_usb_transfer_new (dev);
          transfer->ssm = ssm;
          transfer->short_is_error = TRUE;
          fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPIN, EGIS0570_PKTSIZE);
          fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, fpi_ssm_usb_transfer_cb, NULL);
        }
      break;

    case RECEIVE_FRAMES:
      if (self->command < total - 1)
        {
          self->command++;
          fpi_ssm_jump_to_state (ssm, SEND_COMMAND);
          break;
        }
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk (transfer, EGIS0570_EPIN, EGIS0570_INPSIZE);
      fpi_usb_transfer_submit (transfer, EGIS0570_TIMEOUT, NULL, receive_frames_cb, NULL);
      break;

    case PROCESS_FRAMES:
      if (process_packet (dev))
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          self->initializing = FALSE;
          self->command = 0;
          fpi_ssm_jump_to_state (ssm, SEND_COMMAND);
        }
      break;

    case MATCH_FRAMES:
      {
        MatchData *data = g_new0 (MatchData, 1);
        g_autoptr(GTask) task = g_task_new (dev, fpi_device_get_cancellable (dev),
                                            match_done, NULL);
        g_autoptr(GPtrArray) single = g_ptr_array_new ();
        GPtrArray *prints = single;
        data->action = fpi_device_get_current_action (dev);
        data->templates = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
        if (data->action == FPI_DEVICE_ACTION_VERIFY)
          {
            FpPrint *print;
            fpi_device_get_verify_data (dev, &print);
            g_ptr_array_add (single, print);
          }
        else if (data->action == FPI_DEVICE_ACTION_IDENTIFY)
          {
            fpi_device_get_identify_data (dev, &prints);
          }
        for (guint i = 0; i < prints->len; i++)
          {
            GVariant *template = NULL;
            g_object_get (g_ptr_array_index (prints, i), "fpi-data", &template, NULL);
            /* Keep indices even for an old or invalid template. */
            if (!template)
              template = g_variant_ref_sink (g_variant_new_tuple (NULL, 0));
            g_ptr_array_add (data->templates, template);
          }
        g_task_set_task_data (task, data, (GDestroyNotify) match_data_free);
        g_task_run_in_thread (task, match_frames);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
operation_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);

  self->ssm = NULL;
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
  if (error)
    {
      fpi_device_action_error (dev, error);
      return;
    }
  switch ((gint) fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      {
        FpPrint *print;
        GVariantBuilder builder;
        fpi_device_get_enroll_data (dev, &print);
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
        for (guint i = 0; i < self->blobs->len; i++)
          {
            gsize size;
            const void *data = g_bytes_get_data (g_ptr_array_index (self->blobs, i), &size);
            g_variant_builder_add_value (&builder, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, data, size, 1));
          }
        fpi_print_set_type (print, FPI_PRINT_RAW);
        fpi_print_set_device_stored (print, FALSE);
        g_object_set (print, "fpi-data", g_variant_new ("(uaay)", 1u, &builder), NULL);
        fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
        break;
      }

    case FPI_DEVICE_ACTION_VERIFY:
      fpi_device_verify_complete (dev, NULL);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
      fpi_device_identify_complete (dev, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
start_operation (FpDevice *dev)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);

  self->command = 0;
  self->initializing = TRUE;
  self->wait_for_removal = self->present;
  self->finished = FALSE;
  self->absent_frames = 0;
  self->frame_count = 0;
  if (self->calibrated)
    fpi_device_report_finger_status (dev, self->wait_for_removal ? FP_FINGER_STATUS_PRESENT :
                                     FP_FINGER_STATUS_NEEDED);
  g_ptr_array_set_size (self->blobs, 0);
  self->ssm = fpi_ssm_new (dev, run_state, N_STATES);
  fpi_ssm_start (self->ssm, operation_complete);
}

static void
dev_open (FpDevice *dev)
{
  GError *error = NULL;
  /* Keep the empty-sensor reference across operations on one open device.
   * Recalibrating after every verification can mistake the next touch for
   * background when the user presents their finger before capture starts. */
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (dev);

  /* If the previous operation ended with a finger down, retain its reference
   * until removal rather than calibrating that held finger as background. */
  if (!self->present)
    self->calibrated = FALSE;
  g_usb_device_claim_interface (fpi_device_get_usb_device (dev), EGIS0570_INTF, 0, &error);
  fpi_device_open_complete (dev, error);
}

static void
dev_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev), EGIS0570_INTF, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
finalize (GObject *object)
{
  FpDeviceEgis0570Touch *self = FPI_DEVICE_EGIS0570TOUCH (object);

  g_free (self->frames);
  g_ptr_array_unref (self->blobs);
  G_OBJECT_CLASS (fpi_device_egis0570touch_parent_class)->finalize (object);
}

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0570 },
  { .vid = 0, .pid = 0 }
};

static void
fpi_device_egis0570touch_init (FpDeviceEgis0570Touch *self)
{
  self->frames = g_new (guint16, MAX_FRAMES * FRAME_PIXELS);
  self->blobs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
}

static void
fpi_device_egis0570touch_class_init (FpDeviceEgis0570TouchClass *klass)
{
  FpDeviceClass *dev = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = finalize;
  /* Preserve the existing driver identity so fprintd selects this as the
   * replacement implementation for 1c7a:0570. Old image templates fail the
   * raw-template type check above and must be enrolled again. */
  dev->id = "egis0570";
  dev->full_name = "Egis Technology Inc. 0570 touch sensor";
  dev->type = FP_DEVICE_TYPE_USB;
  dev->id_table = id_table;
  dev->scan_type = FP_SCAN_TYPE_PRESS;
  dev->nr_enroll_stages = ENROLL_STAGES;
  dev->open = dev_open;
  dev->close = dev_close;
  dev->enroll = start_operation;
  dev->verify = start_operation;
  dev->identify = start_operation;
  fpi_device_class_auto_initialize_features (dev);
}
