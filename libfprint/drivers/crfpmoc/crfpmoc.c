/*
 * ChromeOS Fingerprint driver for libfprint
 *
 * Copyright (C) 2024 Abhinav Baid <abhinavbaid@gmail.com>
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

#define FP_COMPONENT "crfpmoc"

#include <fcntl.h>

#include <glib-unix.h>

#include "crfpmoc.h"

struct _FpiDeviceCrfpMoc
{
  FpDevice parent;
  FpiSsm  *task_ssm;

  int      poll_source;
};

G_DEFINE_TYPE (FpiDeviceCrfpMoc, fpi_device_crfpmoc, FP_TYPE_DEVICE)

typedef struct
{
  int stage;
} EnrollData;

static const FpIdEntry crfpmoc_id_table[] = {
  {.udev_types = FPI_DEVICE_UDEV_SUBTYPE_MISC, .misc_name = "cros_fp"},
  {.udev_types = 0}
};

static const gchar *const crfpmoc_meanings[] = {
  "SUCCESS",
  "INVALID_COMMAND",
  "ERROR",
  "INVALID_PARAM",
  "ACCESS_DENIED",
  "INVALID_RESPONSE",
  "INVALID_VERSION",
  "INVALID_CHECKSUM",
  "IN_PROGRESS",
  "UNAVAILABLE",
  "TIMEOUT",
  "OVERFLOW",
  "INVALID_HEADER",
  "REQUEST_TRUNCATED",
  "RESPONSE_TOO_BIG",
  "BUS_ERROR",
  "BUSY",
  "INVALID_HEADER_VERSION",
  "INVALID_HEADER_CRC",
  "INVALID_DATA_CRC",
  "DUP_UNAVAILABLE",
};

static const gchar *
crfpmoc_strresult (int i)
{
  if (i < 0 || i >= G_N_ELEMENTS (crfpmoc_meanings))
    return "<unknown>";
  return crfpmoc_meanings[i];
}

static char *
get_print_data_descriptor (FpPrint *print, gint8 template)
{
  const char *driver;
  const char *dev_id;

  driver = fp_print_get_driver (print);
  dev_id = fp_print_get_device_id (print);

  return g_strdup_printf ("%s/%s/%d", driver, dev_id, template);
}

static void
crfpmoc_set_print_data (FpPrint *print,
                        gint8    template,
                        guint8  *frame,
                        size_t   frame_size)
{
  g_autofree gchar *descr = NULL;
  GVariant *print_id_var = NULL;
  GVariant *fpi_data = NULL;
  GVariant *frame_var = NULL;

  fp_dbg ("Setting print data");

  fpi_print_set_type (print, FPI_PRINT_RAW);

  descr = get_print_data_descriptor (print, template);
  print_id_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                            descr, strlen (descr),
                                            sizeof (guint8));

  frame_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                         frame, frame_size,
                                         sizeof (guint8));

  fpi_data = g_variant_new ("(@ay@ay)", print_id_var, frame_var);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

/* required since the downloaded frame data is encrypted and 2 downloads are
 * not the same so a compare function, that ignores frame_var is required and
 * only compares on the print_id_var
 */
static gboolean
crfpmoc_print_equal (FpPrint *self, FpPrint *other)
{
  g_autoptr(GVariant) data1 = NULL;
  g_autoptr(GVariant) data2 = NULL;
  g_autoptr(GVariant) print_id_var1 = NULL;
  g_autoptr(GVariant) print_id_var2 = NULL;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (other != NULL, FALSE);

  g_object_get (self, "fpi-data", &data1, NULL);
  g_object_get (other, "fpi-data", &data2, NULL);

  if (!data1 || !data2)
    return FALSE;

  print_id_var1 = g_variant_get_child_value (data1, 0);
  print_id_var2 = g_variant_get_child_value (data2, 0);

  return g_variant_equal (print_id_var1, print_id_var2);
}

static gboolean
crfpmoc_ec_command (FpiDeviceCrfpMoc *self,
                    int               command,
                    int               version,
                    const void       *outdata,
                    int               outsize,
                    void             *indata,
                    int               insize,
                    GError          **error)
{
  g_autofree struct crfpmoc_cros_ec_command_v2 *s_cmd = NULL;
  int fd;
  int r;

  g_assert (outsize == 0 || outdata != NULL);
  g_assert (insize == 0 || indata != NULL);

  s_cmd = g_malloc0 (sizeof (struct crfpmoc_cros_ec_command_v2) + outsize + insize);
  g_assert (s_cmd != NULL);

  s_cmd->command = command;
  s_cmd->version = version;
  s_cmd->result = 0xff;
  s_cmd->outsize = outsize;
  s_cmd->insize = insize;

  if (outsize > 0)
    memcpy (s_cmd->data, outdata, outsize);

  fd = fpi_device_get_udev_fd (FP_DEVICE (self), FPI_DEVICE_UDEV_SUBTYPE_MISC);
  r = ioctl (fd, CRFPMOC_CROS_EC_DEV_IOCXCMD_V2, s_cmd);
  if (r < 0)
    {
      fp_warn ("ioctl %d, errno %d (%s), EC result %d (%s)", r,
               errno, strerror (errno), s_cmd->result,
               crfpmoc_strresult (s_cmd->result));
    }
  else
    {
      insize = MIN (r, insize);

      if (insize > 0 )
        memcpy (indata, s_cmd->data, MIN (r, insize));

      if (s_cmd->result != EC_RES_SUCCESS)
        {
          fp_dbg ("EC result %d (%s)",
                  s_cmd->result, crfpmoc_strresult (s_cmd->result));
          r = -CRFPMOC_EECRESULT - s_cmd->result;
        }
    }

  if (r < 0)
    {
      FpDeviceError error_code = FP_DEVICE_ERROR_GENERAL;

      if (s_cmd->result == EC_RES_BUSY)
        error_code = FP_DEVICE_ERROR_BUSY;
      if (s_cmd->result == EC_RES_ACCESS_DENIED)
        error_code = FP_DEVICE_ERROR_NOT_SUPPORTED;

      g_propagate_error (error,
                         fpi_device_error_new_msg (error_code, "%s",
                                                   crfpmoc_strresult (s_cmd->result)));
      return FALSE;
    }

  return TRUE;
}

static gboolean
crfpmoc_read_bytes (gint fd, GIOCondition condition, gpointer user_data)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (user_data);
  struct crfpmoc_ec_response_get_next_event_v1 buffer = {0};
  int rv;

  rv = read (fd, &buffer, sizeof (buffer));
  self->poll_source = 0;

  if (rv == 0)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Timeout waiting for MKBP event"));
      return G_SOURCE_REMOVE;
    }
  else if (rv < 0)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Error polling for MKBP event"));
      return G_SOURCE_REMOVE;
    }

  fp_dbg ("MKBP event %d data", buffer.event_type);
  fpi_ssm_next_state (self->task_ssm);

  return G_SOURCE_REMOVE;
}

static void
crfpmoc_ec_pollevent (FpiDeviceCrfpMoc *self, unsigned long mask)
{
  int fd;
  int rv;

  fd = fpi_device_get_udev_fd (FP_DEVICE (self), FPI_DEVICE_UDEV_SUBTYPE_MISC);
  rv = ioctl (fd, CRFPMOC_CROS_EC_DEV_IOCEVENTMASK_V2, mask);
  if (rv < 0)
    {
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  self->poll_source = g_unix_fd_add (fd, G_IO_IN, crfpmoc_read_bytes, self);
}

static gboolean
crfpmoc_cmd_fp_mode (FpiDeviceCrfpMoc *self, guint32 inmode,
                     guint32 *outmode, GError **error)
{
  struct crfpmoc_ec_params_fp_mode p = {0};
  struct crfpmoc_ec_response_fp_mode r = {0};

  p.mode = inmode;
  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_MODE, 0,
                           &p, sizeof (p), &r, sizeof (r), error))
    return FALSE;

  fp_dbg ("FP mode: (0x%x)", r.mode);
  if (outmode != NULL)
    *outmode = r.mode;

  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_seed (FpiDeviceCrfpMoc *self,
                     const char       *seed,
                     GError          **error)
{
  struct crfpmoc_ec_params_fp_seed p = {0};

  fp_dbg ("Setting seed '%s'", seed);

  if (!seed || strlen (seed) != CRFPMOC_FP_CONTEXT_TPM_BYTES)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "Seed length should be %d", CRFPMOC_FP_CONTEXT_TPM_BYTES);
      return FALSE;
    }

  p.struct_version = CRFPMOC_FP_TEMPLATE_FORMAT_VERSION;
  memset (p.seed, 0, CRFPMOC_FP_CONTEXT_TPM_BYTES);
  memcpy (p.seed, seed, CRFPMOC_FP_CONTEXT_TPM_BYTES);

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_SEED, 0,
                           &p, sizeof (p), NULL, 0, error))
    return FALSE;

  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_info (FpiDeviceCrfpMoc *self,
                     guint16          *enrolled_templates,
                     GError          **error)
{
  struct crfpmoc_ec_response_fp_info r = {0};

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_INFO, 1,
                           NULL, 0, &r, sizeof (r), error))
    return FALSE;

  fp_dbg ("Fingerprint sensor: vendor %x product %x model %x version %x",
          r.vendor_id, r.product_id, r.model_id, r.version);
  fp_dbg ("Image: size %dx%d %d bpp", r.width, r.height, r.bpp);
  fp_dbg ("Templates: version %d size %d count %d/%d dirty bitmap %x",
          r.template_version, r.template_size, r.template_valid,
          r.template_max, r.template_dirty);

  *enrolled_templates = r.template_valid;
  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_stats (FpiDeviceCrfpMoc *self, gint8 *template, GError **error)
{
  struct crfpmoc_ec_response_fp_stats r = {0};

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_STATS, 0,
                           NULL, 0, &r, sizeof (r), error))
    return FALSE;

  if (r.timestamps_invalid & CRFPMOC_FPSTATS_MATCHING_INV)
    {
      fp_dbg ("Last matching time: Invalid");
      *template = -1;
    }
  else
    {
      fp_dbg ("Last matching time: %d us (finger: %d)", r.matching_time_us, r.template_matched);
      *template = r.template_matched;
    }

  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_enc_status (FpiDeviceCrfpMoc *self, guint32 *status, GError **error)
{
  struct crfpmoc_ec_response_fp_encryption_status resp = {0};

  g_return_val_if_fail (status != NULL, FALSE);

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_ENC_STATUS, 0,
                           NULL, 0, &resp, sizeof (resp), error))
    return FALSE;

  fp_dbg ("FPMCU encryption status: %d", resp.status);
  fp_dbg ("Valid flags: %d", resp.valid_flags);

  if (resp.status == CRFPMOC_FP_ENC_STATUS_SEED_SET)
    fp_dbg ("Seed is already set");

  *status = resp.status;

  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_ensure_seed (FpiDeviceCrfpMoc *self, const char * seed, GError **error)
{
  guint32 status;

  fp_dbg ("Checking if seed is set");
  if (!crfpmoc_cmd_fp_enc_status (self, &status, error))
    return FALSE;

  fp_dbg ("FPMCU encryption status: %d", status);

  /* It's unclear know why the encryption status is 5 after using verify.
   * Since it should be 1. It also returns to 1 after using enroll
   * we're going to assume that 5 means that the seed is set,
   * since setting the seed twice locks the device
   */
  if (status == 0)
    {
      fp_dbg ("Seed is not set, setting seed");
      if (!crfpmoc_cmd_fp_seed (self, seed, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
crfpmoc_ec_max_insize (FpiDeviceCrfpMoc *self, guint32 *max_insize, GError **error)
{
  struct crfpmoc_ec_response_get_protocol_info protocol_info = {0};

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_GET_PROTOCOL_INFO, 0,
                           NULL, 0, &protocol_info, sizeof (protocol_info), error))
    return FALSE;

  *max_insize = protocol_info.max_response_packet_size - sizeof (struct crfpmoc_ec_host_response);

  return TRUE;
}

static gboolean
crfpmoc_fp_download_frame (FpiDeviceCrfpMoc                   *self,
                           struct crfpmoc_ec_response_fp_info *info,
                           gint                                index,
                           guint8                            **out_buffer,
                           GError                            **error)
{
  struct crfpmoc_ec_params_fp_frame p = {0};
  gsize stride, size;
  g_autofree guint8 *buffer = NULL;
  guint8 *ptr;
  gint cmdver = 1;
  gsize rsize = sizeof (*info);
  const gint max_attempts = 3;
  gint num_attempts;
  guint32 ec_max_insize;
  gint template_idx = index + CRFPMOC_FP_FRAME_INDEX_TEMPLATE;

  *out_buffer = NULL;

  if (!crfpmoc_ec_max_insize (self, &ec_max_insize, error))
    return FALSE;

  usleep (10000);

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_INFO, cmdver,
                           NULL, 0, info, rsize, error))
    return FALSE;

  size = info->template_size;
  g_assert (size != 0);

  buffer = g_malloc0 (size);
  ptr = buffer;
  p.offset = template_idx << CRFPMOC_FP_FRAME_INDEX_SHIFT;

  while (size > 0)
    {
      stride = MIN (ec_max_insize, size);
      p.size = stride;
      num_attempts = 0;

      while (num_attempts < max_attempts)
        {
          g_autoptr(GError) local_error = NULL;

          num_attempts++;
          fp_dbg ("crfpmoc_fp_download_template [%d]: read %zd bytes at %lu, "
                  "off 0x%04x", num_attempts, stride, ptr - buffer, p.offset);

          if (crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_FRAME, 0,
                                  &p, sizeof (p),
                                  ptr, stride,
                                  &local_error))
            {
              fp_dbg ("crfpmoc_fp_download_template: frame done");
              break;
            }

          if (g_error_matches (local_error, FP_DEVICE_ERROR,
                               FP_DEVICE_ERROR_BUSY))
            {
              fp_dbg ("crfpmoc_fp_download_template: retry frame");
            }
          else if (g_error_matches (local_error, FP_DEVICE_ERROR,
                                    FP_DEVICE_ERROR_NOT_SUPPORTED))
            {
              fp_dbg ("Access denied, stopping retrying");
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          fp_dbg ("Failed to download template chunk at offset %u, "
                  "attempt %d of %d: %s",
                  p.offset, num_attempts, max_attempts,
                  local_error->message);

          if (num_attempts >= max_attempts)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          usleep (100000);
        }

      p.offset += stride;
      size -= stride;
      ptr += stride;
    }

  *out_buffer = g_steal_pointer (&buffer);
  return TRUE;
}


static void
crfpmoc_cmd_wait_event_fingerprint (FpiDeviceCrfpMoc *self)
{
  long event_type = CRFPMOC_EC_MKBP_EVENT_FINGERPRINT;

  crfpmoc_ec_pollevent (self, 1 << event_type);
}

static void
crfpmoc_task_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Task SSM done");
  g_assert (!self->task_ssm || self->task_ssm == ssm);
  self->task_ssm = NULL;

  if (error)
    fpi_device_action_error (device, error);
}

static void
crfpmoc_open (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  const char *device_path;

  device_path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_MISC);
  fp_dbg ("Opening device %s", device_path);

  if (!crfpmoc_cmd_fp_ensure_seed (FPI_DEVICE_CRFPMOC (device),
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to ensure seed");
      fpi_device_open_complete (device, g_steal_pointer (&error));
      return;
    }

  fpi_device_open_complete (device, NULL);
}

static void
crfpmoc_cancel (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Cancel");

  /* Cancel any poll source */
  g_clear_handle_id (&self->poll_source, g_source_remove);

  /* Issue reset */
  crfpmoc_cmd_fp_mode (self, 0, NULL, NULL);
}

static void
crfpmoc_suspend (FpDevice *device)
{
  fp_dbg ("Suspend");

  crfpmoc_cancel (device);

  fp_dbg ("crfpmoc_suspend: suspend_complete");
  fpi_device_suspend_complete (device, NULL);
}

static void
crfpmoc_close (FpDevice *device)
{
  fp_dbg ("Closing device");

  crfpmoc_cancel (device);

  fpi_device_close_complete (device, NULL);
}

static void
crfpmoc_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  FpPrint *print = NULL;
  EnrollData *enroll_data = fpi_ssm_get_data (ssm);
  g_autofree gchar *user_id = NULL;
  g_autofree guint8 *frame = NULL;
  guint16 enrolled_templates = 0;
  guint32 mode;
  struct crfpmoc_ec_response_fp_info info;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_SENSOR_ENROLL:
      if (!crfpmoc_cmd_fp_mode (self,
                                CRFPMOC_FP_MODE_ENROLL_IMAGE |
                                CRFPMOC_FP_MODE_ENROLL_SESSION,
                                &mode, &error))
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      else
        fpi_ssm_next_state (ssm);

      break;

    case ENROLL_WAIT_FINGER:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      crfpmoc_cmd_wait_event_fingerprint (self);
      break;

    case ENROLL_SENSOR_CHECK:
      if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_DONT_CHANGE, &mode, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }

      if (mode & CRFPMOC_FP_MODE_ENROLL_SESSION)
        {
          if (mode & CRFPMOC_FP_MODE_ENROLL_IMAGE)
            {
              if (mode & CRFPMOC_FP_MODE_ENROLL_IMAGE)
                {
                  fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_FINGER);
                }
              else
                {
                  fpi_device_get_enroll_data (device, &print);
                  fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

                  enroll_data->stage++;
                  fp_info ("Partial capture successful (%d/%d).",
                           enroll_data->stage, CRFPMOC_NR_ENROLL_STAGES);

                  fpi_device_enroll_progress (device, enroll_data->stage, print, NULL);

                  fpi_ssm_jump_to_state (ssm, ENROLL_SENSOR_ENROLL);
                }
            }
          else if (mode == 0)
            {
              fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

              fpi_ssm_next_state (ssm);
            }
          else
            {
              fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

              fpi_device_enroll_progress (device, enroll_data->stage, NULL,
                                          fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                    "FP mode: (0x%x)", mode));

              fpi_ssm_jump_to_state (ssm, ENROLL_SENSOR_ENROLL);
            }
        }
      else if (mode == 0)
        {
          fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

          fpi_ssm_next_state (ssm);
        }
      else
        {
          fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

          fpi_device_enroll_progress (device, enroll_data->stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                "FP mode: (0x%x)", mode));

          fpi_ssm_jump_to_state (ssm, ENROLL_SENSOR_ENROLL);
        }
      break;

    case ENROLL_COMMIT:
      fpi_device_get_enroll_data (device, &print);

      crfpmoc_cmd_fp_info (self, &enrolled_templates, NULL);
      fp_dbg ("Number of enrolled templates is: %d", enrolled_templates);

      user_id = fpi_print_generate_user_id (print);
      fp_dbg ("New fingerprint ID: %s", user_id);

      g_object_set (print, "description", user_id, NULL);

      if (!crfpmoc_fp_download_frame (self, &info,  enrolled_templates - 1, &frame, &error))
        {
          g_warning ("Failed to download frame: %s", error->message);
          crfpmoc_set_print_data (print, enrolled_templates - 1, NULL, 0);
        }
      else
        {
          crfpmoc_set_print_data (print, enrolled_templates - 1, frame, info.template_size);
        }

      fp_info ("Enrollment was successful!");
      fpi_device_enroll_complete (device, g_object_ref (print), NULL);

      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
crfpmoc_enroll (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  EnrollData *enroll_data = g_new0 (EnrollData, 1);

  fp_dbg ("Enroll");
  enroll_data->stage = 0;

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, crfpmoc_enroll_run_state, ENROLL_STATES);
  fpi_ssm_set_data (self->task_ssm, g_steal_pointer (&enroll_data), g_free);
  fpi_ssm_start (self->task_ssm, crfpmoc_task_ssm_done);
}

static void
crfpmoc_verify_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  FpPrint *print = NULL;
  FpPrint *verify_print = NULL;
  GPtrArray *prints;
  gboolean found = FALSE;
  guint index;
  guint32 mode;
  gint8 template = -1;


  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VERIFY_SENSOR_MATCH:
      if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_MATCH, &mode, &error))
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      else
        fpi_ssm_next_state (ssm);
      break;

    case VERIFY_WAIT_FINGER:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      crfpmoc_cmd_wait_event_fingerprint (self);
      break;

    case VERIFY_SENSOR_CHECK:
      if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_DONT_CHANGE, &mode, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }

      if (mode & CRFPMOC_FP_MODE_MATCH)
        {
          if (mode & CRFPMOC_FP_MODE_MATCH)
            {
              fpi_ssm_jump_to_state (ssm, VERIFY_WAIT_FINGER);
            }
          else if (mode == 0)
            {
              fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

              fpi_ssm_next_state (ssm);
            }
          else
            {
              fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

              fpi_ssm_mark_failed (ssm, fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL, "FP mode: (0x%x)", mode));
            }
        }
      else if (mode == 0)
        {
          fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

          if (!crfpmoc_cmd_fp_stats (self, &template, &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              break;
            }

          if (template == -1)
            {
              fp_info ("Print was not identified by the device");

              if (!crfpmoc_cmd_fp_stats (self, &template, &error))
                {
                  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
                }
              else
                {
                  if (template == -1)
                    {
                      fp_info ("Print was not identified by the device");

                      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
                        fpi_device_identify_report (device, NULL, NULL, NULL);
                      else
                        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);
                    }
                  else
                    {
                      g_autofree guint8 *frame = NULL;
                      struct crfpmoc_ec_response_fp_info info;

                      print = fp_print_new (device);

                      if (!crfpmoc_fp_download_frame (self, &info, template, &frame, &error))
                        {
                          fp_warn ("Failed to download frame: %s", error->message);
                          crfpmoc_set_print_data (print, template, NULL, 0);
                        }
                      else
                        {
                          crfpmoc_set_print_data (print, template, frame, info.template_size);
                        }

                      fp_info ("Identify successful for template %d", template);

                      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
                        {
                          fpi_device_get_identify_data (device, &prints);
                          found = g_ptr_array_find_with_equal_func (prints,
                                                                    print,
                                                                    (GEqualFunc) crfpmoc_print_equal,
                                                                    &index);

                          if (found)
                            fpi_device_identify_report (device, g_ptr_array_index (prints, index), print, NULL);
                          else
                            fpi_device_identify_report (device, NULL, print, NULL);
                        }
                      else
                        {
                          fpi_device_get_verify_data (device, &verify_print);
                          fp_info ("Verifying against: %s", fp_print_get_description (verify_print));

                          if (crfpmoc_print_equal (verify_print, print))
                            fpi_device_verify_report (device, FPI_MATCH_SUCCESS, print, NULL);
                          else
                            fpi_device_verify_report (device, FPI_MATCH_FAIL, print, NULL);
                        }
                    }
                  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
                    fpi_device_verify_complete (device, NULL);
                  else
                    fpi_device_identify_complete (device, NULL);
                  fpi_ssm_mark_completed (ssm);
                }
            }
          else
            {
              g_autofree guint8 *frame = NULL;
              struct crfpmoc_ec_response_fp_info info;

              print = fp_print_new (device);

              if (!crfpmoc_fp_download_frame (self, &info, template, &frame, &error))
                {
                  fp_warn ("Failed to download frame: %s", error->message);
                  crfpmoc_set_print_data (print, template, NULL, 0);
                }
              else
                {
                  crfpmoc_set_print_data (print, template, frame, info.template_size);
                }

              fp_info ("Identify successful for template %d", template);

              if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
                {
                  fpi_device_get_identify_data (device, &prints);



                  found = g_ptr_array_find_with_equal_func (prints,
                                                            print,
                                                            (GEqualFunc) crfpmoc_print_equal,
                                                            &index);

                  if (found)
                    fpi_device_identify_report (device, g_ptr_array_index (prints, index), print, NULL);
                  else
                    fpi_device_identify_report (device, NULL, print, NULL);
                }
              else
                {
                  fpi_device_get_verify_data (device, &verify_print);
                  fp_info ("Verifying against: %s", fp_print_get_description (verify_print));

                  if (crfpmoc_print_equal (verify_print, print))
                    fpi_device_verify_report (device, FPI_MATCH_SUCCESS, print, NULL);
                  else
                    fpi_device_verify_report (device, FPI_MATCH_FAIL, print, NULL);
                }
            }

          if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
            fpi_device_verify_complete (device, NULL);
          else
            fpi_device_identify_complete (device, NULL);

          fpi_ssm_mark_completed (ssm);
        }
      else
        {
          fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);

          if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
            {
              fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL, "FP mode: (0x%x)", mode));
              fpi_device_verify_complete (device, NULL);
            }
          else
            {
              fpi_device_identify_report (device, NULL, NULL, fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL, "FP mode: (0x%x)", mode));
              fpi_device_identify_complete (device, NULL);
            }
          fpi_ssm_mark_completed (ssm);
        }
      break;
    }
}

static void
crfpmoc_identify_verify (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Identify or Verify");

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, crfpmoc_verify_run_state, VERIFY_STATES);
  fpi_ssm_start (self->task_ssm, crfpmoc_task_ssm_done);
}

static void
crfpmoc_clear_storage_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  guint32 mode;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CLEAR_STORAGE_SENSOR_RESET:
      if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_RESET_SENSOR, &mode, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }

      fpi_device_clear_storage_complete (device, NULL);
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
crfpmoc_clear_storage (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Clear storage");

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, crfpmoc_clear_storage_run_state, CLEAR_STORAGE_STATES);
  fpi_ssm_start (self->task_ssm, crfpmoc_task_ssm_done);
}

static void
fpi_device_crfpmoc_init (FpiDeviceCrfpMoc *self)
{
  G_DEBUG_HERE ();
}

static void
fpi_device_crfpmoc_class_init (FpiDeviceCrfpMocClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = CRFPMOC_DRIVER_FULLNAME;

  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = crfpmoc_id_table;
  dev_class->nr_enroll_stages = CRFPMOC_NR_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = crfpmoc_open;
  dev_class->cancel = crfpmoc_cancel;
  dev_class->suspend = crfpmoc_suspend;
  dev_class->close = crfpmoc_close;
  dev_class->enroll = crfpmoc_enroll;
  dev_class->identify = crfpmoc_identify_verify;
  dev_class->verify = crfpmoc_identify_verify;
  dev_class->clear_storage = crfpmoc_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);
}
