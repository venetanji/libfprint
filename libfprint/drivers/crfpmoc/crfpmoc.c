/*
 * ChromeOS Fingerprint driver for libfprint
 *
 * Copyright (C) 2024 Abhinav Baid <abhinavbaid@gmail.com>
 * Copyright (C) 2024 Felix Niederer <felix@niederer.dev>
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

  int      emul_fd;
};

G_DEFINE_TYPE (FpiDeviceCrfpMoc, fpi_device_crfpmoc, FP_TYPE_DEVICE)

static void crfpmoc_finger_up_run_state (FpiSsm   *ssm,
                                         FpDevice *device);
static void crfpmoc_clear_storage_run_state (FpiSsm   *ssm,
                                             FpDevice *device);

/* State machine private data. All state machines get this and it is
 * cleared/freed at the completion of the state machine.
 */
typedef struct
{
  FpiSsm  *parent_ssm;
  GSource *timeout;
  int      timeout_state;
  int      poll_source;
} SubSsmData;

/* Data for the enroll state machine */
typedef struct
{
  int stage;
} EnrollData;

static const FpIdEntry crfpmoc_id_table[] = {
  {.udev_types = FPI_DEVICE_UDEV_SUBTYPE_MISC, .misc_name = "cros_fp"},
  {.udev_types = 0}
};

/* Clear and free the state machine private data */
static void
subssm_data_free (void *arg)
{
  SubSsmData *data = arg;

  /* Cancel any poll source */
  fp_dbg ("crfpmoc_ssm_data_free: remove poll source %d", data->poll_source);
  g_clear_handle_id (&data->poll_source, g_source_remove);

  /* Cancel any timeout source */
  g_clear_pointer (&data->timeout, g_source_destroy);

  g_free (data);
}

/* Wrapper to start a sub-state machine after adding the private data. The data
 * can be supplied and is required to be crfpmoc_ssm_data or contain it as its
 * first member.
 */
static void
crfpmoc_ssm_start_subsm (FpiDeviceCrfpMoc *self, FpiSsm *ssm)
{
  SubSsmData *data = NULL;

  g_assert (self->task_ssm != NULL);

  data = g_new0 (SubSsmData, 1);
  data->parent_ssm = ssm;

  fpi_ssm_set_data (ssm, g_steal_pointer (&data), subssm_data_free);
  fpi_ssm_start_subsm (self->task_ssm, ssm);
}

/* Wrapper to start a state machine after adding the private data. The data
 * can be supplied and is required to be crfpmoc_ssm_data or contain it as its
 * first member.
 */
static void
crfpmoc_ssm_start (FpiSsm                 *ssm,
                   FpiSsmCompletedCallback cb)
{
  SubSsmData *data = g_new0 (SubSsmData, 1);

  fpi_ssm_set_data (ssm, g_steal_pointer (&data), subssm_data_free);
  fpi_ssm_start (ssm, cb);
}

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

static const gchar *const crfpmoc_mkbp_event_names[] = {
  "KEY_MATRIX",
  "HOST_EVENT",
  "SENSOR_FIFO",
  "BUTTON",
  "SWITCH",
  "FINGERPRINT",
  "SYSRQ",
  "HOST_EVENT64",
  "CEC_EVENT",
  "CEC_MESSAGE",
  "DP_ALT_MODE_ENTERED",
  "ONLINE_CALIBRATION",
  "PCHG",
};

static const gchar *
crfpmoc_mkbp_event_strresult (int i)
{
  if (i < 0 || i >= G_N_ELEMENTS (crfpmoc_mkbp_event_names))
    return "<unknown>";
  return crfpmoc_mkbp_event_names[i];
}

static void
crfpmoc_set_print_data (FpPrint *print,
                        guint8  *template,
                        size_t   template_size)
{
  GVariant *fpi_data = NULL;

  fp_dbg ("Setting print data");

  if (template == NULL || template_size == 0)
    {
      fp_warn ("Template is NULL or size is 0, setting empty template");
      template = NULL;
      template_size = 0;
    }

  fpi_data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                        template, template_size,
                                        sizeof (guint8));
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

static gboolean
crfpmoc_get_print_data (FpPrint *print,
                        guint8 **template,
                        size_t  *template_size,
                        GError **error)
{
  g_autoptr(GVariant) fpi_data = NULL;
  const guint8 *template_data = NULL;
  gsize template_data_size = 0;

  if (template)
    *template = NULL;
  if (template_size)
    *template_size = 0;

  g_object_get (print, "fpi-data", &fpi_data, NULL);

  if (!fpi_data)
    {
      g_set_error (error, FP_DEVICE_ERROR_GENERAL, FP_DEVICE_ERROR_GENERAL,
                   "No fpi-data found in the print object.");
      return FALSE;
    }

  template_data = g_variant_get_fixed_array (fpi_data,
                                             &template_data_size,
                                             sizeof (guint8));

  if (!template_data || template_data_size == 0)
    {
      g_set_error (error, FP_DEVICE_ERROR_GENERAL, FP_DEVICE_ERROR_GENERAL,
                   "Template data is empty in the print object.");
      return FALSE;
    }

  if (template)
    *template = g_memdup2 (template_data, template_data_size);

  if (template_size)
    *template_size = template_data_size;

  return TRUE;
}

/* Self test uses umockdev for replay of a captured sequence. At this
 * time umockdev supports cros_ec only for replay. We need to create
 * the sequence. Record all the ioctls for replay in the format
 * umockdev requires.
 */
static void
crfpmoc_umockdev_record (FpiDeviceCrfpMoc *self, int res,
                         int cmd, void *arg)
{
  g_autoptr(GString) buffer = NULL;
  struct crfpmoc_cros_ec_command_v2 *s_cmd = arg;
  guchar *ptr = arg;
  gsize size;

  g_return_if_fail (fpi_device_emulation_mode_enabled (FP_DEVICE (self)));

  if (G_LIKELY (self->emul_fd == -1))
    return;

  switch (cmd)
    {
    case CRFPMOC_CROS_EC_DEV_IOCXCMD_V2:
      size = (sizeof (*s_cmd) + s_cmd->insize);
      buffer = g_string_sized_new (size * 2);
      g_string_append_printf (buffer, "CROS_EC_DEV_IOCXCMD_V2 %u ",
                              s_cmd->insize);

      /* Send the command. */
      while (size > s_cmd->insize)
        {
          g_string_append_printf (buffer, "%02X", *ptr++);
          size--;
        }

      /* Send the data. If this is a template suppress the actual data for privacy. */
      if (s_cmd->command == CRFPMOC_EC_CMD_FP_FRAME)
        {
          for (gsize i = 0; i < size; i++)
            g_string_append_printf (buffer, "%02X", (guint8) (i & 0xff));
          size = 0;
        }
      else
        {
          while (size > 0)
            {
              g_string_append_printf (buffer, "%02X", *ptr++);
              size--;
            }
        }

      g_string_append_c (buffer, '\n');

      if (write (self->emul_fd, buffer->str, buffer->len) != (gssize) buffer->len)
        fp_dbg ("emulation trace write failed");
      break;

    default:
      g_warn_if_reached ();
    }
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
  int errsv;
  int fd;
  int r;

  g_assert (outsize == 0 || outdata != NULL);
  g_assert (insize == 0 || indata != NULL);

  s_cmd = g_malloc0 (sizeof (struct crfpmoc_cros_ec_command_v2) + MAX (outsize, insize));
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
  errsv = errno;

  if (fpi_device_emulation_mode_enabled (FP_DEVICE (self)))
    crfpmoc_umockdev_record (self, r, CRFPMOC_CROS_EC_DEV_IOCXCMD_V2, s_cmd);

  if (r < 0)
    {
      fp_warn ("ioctl %d, errno %d (%s), EC result %d (%s)", r,
               errsv, strerror (errsv), s_cmd->result,
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

/* Completion callback for event reporting. Get the event and decode
 * for now. The function may be used with timeout support and if so
 * the timeout needs to be cancelled here. The state machine that
 * requested the poll is passed as an argument. Use the private data
 * to see if a timeout is active.
 */
static gboolean
crfpmoc_read_bytes (gint fd, GIOCondition condition, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  SubSsmData *data = fpi_ssm_get_data (ssm);
  struct crfpmoc_ec_response_get_next_event_v1 buffer = {0};
  int rv;

  fp_dbg ("crfpmoc_read_bytes: called");

  /* If there is an active timeout on this state machine remove it */
  g_clear_pointer (&data->timeout, g_source_destroy);

  rv = read (fd, &buffer, sizeof (buffer));
  fp_dbg ("crfpmoc_read_bytes: read %d bytes", rv);
  data->poll_source = 0;

  if (rv == 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Timeout waiting for MKBP event"));
      return G_SOURCE_REMOVE;
    }
  else if (rv < 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Error polling for MKBP event"));
      return G_SOURCE_REMOVE;
    }

  if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
    {
      /* Make sure we do not run in the callback context */
      fpi_ssm_next_state_delayed (ssm, 1);

      return G_SOURCE_REMOVE;
    }

  fp_dbg ("MKBP event %ld (%s) data, more %ld",
          buffer.event_type & CRFPMOC_EC_MKBP_EVENT_TYPE_MASK,
          crfpmoc_mkbp_event_strresult (buffer.event_type & CRFPMOC_EC_MKBP_EVENT_TYPE_MASK),
          buffer.event_type & CRFPMOC_EC_MKBP_HAS_MORE_EVENTS);

  switch (buffer.event_type)
    {
    case CRFPMOC_EC_MKBP_EVENT_FINGERPRINT:
      fp_dbg ("crfpmoc_read_bytes: fingerprint 0x%08x", buffer.data.fp_events);

      /* Enroll mode */
      if (buffer.data.fp_events & CRFPMOC_EC_MKBP_FP_ENROLL)
        {
          guint error_code = CRFPMOC_EC_MKBP_FP_ERRCODE (buffer.data.fp_events);
          const gchar *reason = NULL;

          fp_dbg ("  Enroll progress %d", CRFPMOC_EC_MKBP_FP_ENROLL_PROGRESS (buffer.data.fp_events));
          switch (error_code)
            {
            case CRFPMOC_EC_MKBP_FP_ERR_ENROLL_OK:
              reason = "OK";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_ENROLL_LOW_QUALITY:
              reason = "LOW QUALITY";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_ENROLL_IMMOBILE:
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_ENROLL_LOW_COVERAGE:
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_ENROLL_INTERNAL:
              break;
            }
          fp_dbg ("    status: %d (%s)", error_code, reason ? reason : "");
        }

      /* Match mode */
      if (buffer.data.fp_events & CRFPMOC_EC_MKBP_FP_MATCH)
        {
          guint error_code = CRFPMOC_EC_MKBP_FP_ERRCODE (buffer.data.fp_events);
          guint index = CRFPMOC_EC_MKBP_FP_MATCH_IDX (buffer.data.fp_events);
          const gchar *reason = NULL;

          fp_dbg ("  Match index %d, ok %s", index, error_code & 1 ? "yes" : "no");
          switch (error_code)
            {
            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO:
              reason = "NO";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_INTERNAL:
              reason = "INTERNAL";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_TEMPLATES:
              reason = "TEMPLATES";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_AUTH_FAIL:
              reason = "AUTH FAIL";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_LOW_QUALITY:
              reason = "LOW QUALITY";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_NO_LOW_COVERAGE:
              reason = "LOW COVERAGE";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES:
              reason = "YES";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES_UPDATED:
              reason = "UPDATED";
              break;

            case CRFPMOC_EC_MKBP_FP_ERR_MATCH_YES_UPDATE_FAILED:
              reason = "UPDATE FAILED";
              break;
            }
          fp_dbg ("    status: %d (%s)", error_code, reason ? reason : "");
        }

      /* Finger down mode */
      if (buffer.data.fp_events & CRFPMOC_EC_MKBP_FP_FINGER_DOWN)
        fp_dbg ("  Finger down");

      /* Finger up mode */
      if (buffer.data.fp_events & CRFPMOC_EC_MKBP_FP_FINGER_UP)
        fp_dbg ("  Finger up");

      /* ?? */
      if (buffer.data.fp_events & CRFPMOC_EC_MKBP_FP_IMAGE_READY)
        fp_dbg ("  Image ready");
      break;
    }

  /* Make sure we do not run in the callback context */
  fpi_ssm_next_state_delayed (ssm, 1);

  return G_SOURCE_REMOVE;
}

static void
crfpmoc_ec_pollevent (FpiSsm *ssm, unsigned long mask)
{
  SubSsmData *data = fpi_ssm_get_data (ssm);
  FpDevice *device = fpi_ssm_get_device (ssm);
  int fd;
  int rv;

  fd = fpi_device_get_udev_fd (device, FPI_DEVICE_UDEV_SUBTYPE_MISC);
  rv = ioctl (fd, CRFPMOC_CROS_EC_DEV_IOCEVENTMASK_V2, mask);
  if (rv < 0)
    {
      fp_dbg ("crfpmoc_ec_pollevent: ioctl failed %d", rv);
      fpi_ssm_next_state (ssm);
      return;
    }

  data->poll_source = g_unix_fd_add (fd, G_IO_IN, crfpmoc_read_bytes, ssm);
  fp_dbg ("crfpmoc_ec_pollevent: created poll source %d", data->poll_source);
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

/* TODO: should this be converted to a sub state machine instead of
 * the sync completion ?
 */
static gboolean
crfpmoc_fp_set_context (FpiDeviceCrfpMoc *self,
                        const char       *context,
                        GError          **error)
{
  struct crfpmoc_ec_params_fp_context_v1 p = {0};
  gint tries = 40;
  const guint base_sleep_us = 80000; /* 80ms */

  fp_dbg ("Setting context to '%s'", context);

  if (!context || strlen (context) != sizeof (p.userid))
    {
      g_set_error (error,
                   FP_DEVICE_ERROR,
                   FP_DEVICE_ERROR_PROTO,
                   "Context must be exactly %zu bytes.",
                   sizeof (p.userid));
      return FALSE;
    }

  p.action = CRFPMOC_FP_CONTEXT_ASYNC;
  memcpy (p.userid, context, sizeof (p.userid));

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_CONTEXT, 1,
                           &p, sizeof (p), NULL, 0, error))
    {
      g_prefix_error (error, "Initiating context setting failed: ");
      return FALSE;
    }

  for (int i = 0; i < tries; i++)
    {
      g_autoptr(GError) local_error = NULL;
      guint sleep_us;

      if (i < 10)
        {
          sleep_us = base_sleep_us;
        }
      else if (i < 20)
        {
          /* Linear increase from 80ms to 800ms */
          double factor = 1.0 + 9.0 * (i - 10) / 9.0;
          sleep_us = (guint) (base_sleep_us * factor);
        }
      else
        {
          sleep_us = base_sleep_us * 10;
        }

      if (i >= 10)
        fp_warn ("Context setting is taking longer than expected. Attempt %d of %d",
                 (i + 1), tries);

      g_usleep (sleep_us);

      p.action = CRFPMOC_FP_CONTEXT_GET_RESULT;
      if (crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_CONTEXT, 1,
                              &p, sizeof (p), NULL, 0, &local_error))
        {
          fp_dbg ("Context set successfully.");
          return TRUE;
        }

      if (!g_error_matches (local_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY))
        {
          fp_dbg ("Setting context failed. Error: %s", local_error->message);
          g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                      "Setting context failed: ");
          return FALSE;
        }

      fp_dbg ("Context setting is still in progress. Attempt %d of %d", (i + 1), tries);
    }

  g_set_error_literal (error,
                       FP_DEVICE_ERROR,
                       FP_DEVICE_ERROR_GENERAL,
                       "Failed to set context: operation timed out.");
  return FALSE;
}

static gboolean
crfpmoc_cmd_fp_info (FpiDeviceCrfpMoc *self,
                     guint16          *enrolled_templates,
                     GError          **error)
{
  struct crfpmoc_ec_response_fp_info r = {0};
  gchar vendor[5];
  const gchar *model = NULL;

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_INFO, 1,
                           NULL, 0, &r, sizeof (r), error))
    return FALSE;

  vendor[0] = r.vendor_id;
  vendor[1] = r.vendor_id >> 8;
  vendor[2] = r.vendor_id >> 16;
  vendor[3] = r.vendor_id >> 24;
  vendor[4] = '\0';

  if (strcmp (vendor, "FPC ") == 0)
    {
      switch ((r.model_id >> 4) & 0xfff)
        {
        case 0x021:
          model = "FPC1025";
          break;

        case 0x011:
          model = "FPC1035";
          break;

        case 0x140:
          model = "FPC1145";
          break;
        }
    }
  fp_dbg ("Fingerprint sensor: vendor %s product %x model %x (%s) version %x",
          vendor, r.product_id, r.model_id, model ? model : "", r.version);
  fp_dbg ("Image: size %dx%d %d bpp", r.width, r.height, r.bpp);
  fp_dbg ("Templates: version %d size %d count %d/%d dirty bitmap %x",
          r.template_version, r.template_size, r.template_valid,
          r.template_max, r.template_dirty);

  *enrolled_templates = r.template_valid;
  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_stats (FpiDeviceCrfpMoc *self, gint8 *template_idx, GError **error)
{
  struct crfpmoc_ec_response_fp_stats r = {0};

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_STATS, 0,
                           NULL, 0, &r, sizeof (r), error))
    return FALSE;

  fp_dbg ("crfpmoc_cmd_fp_stats: ts %llu us, cap %d us, match %d us (finger %d), overall %d us",
          (long long unsigned int) ((uint64_t) r.overall_t0.hi << 32 | r.overall_t0.lo),
          r.capture_time_us,
          r.matching_time_us,
          r.template_matched,
          r.overall_time_us);

  fp_dbg ("crfpmoc_cmd_fp_stats: invalid (0x%02x) %s",
          r.timestamps_invalid,
          r.timestamps_invalid & CRFPMOC_FPSTATS_MATCHING_INV ? "true" : "false");

  if (r.timestamps_invalid & CRFPMOC_FPSTATS_MATCHING_INV)
    {
      fp_dbg ("Last matching time: Invalid");
      *template_idx = -1;
    }
  else
    {
      fp_dbg ("Last matching time: %d us (finger: %d)",
              r.matching_time_us, r.template_matched);
      *template_idx = r.template_matched;
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

  fp_dbg ("crfpmoc_cmd_fp_enc_status: FPMCU encryption status: %d", resp.status);
  fp_dbg ("crfpmoc_cmd_fp_enc_status: valid flags: %d", resp.valid_flags);

  if (resp.status == CRFPMOC_FP_ENC_STATUS_SEED_SET)
    fp_dbg ("crfpmoc_cmd_fp_enc_status: seed is already set");

  *status = resp.status;

  return TRUE;
}

static gboolean
crfpmoc_cmd_fp_ensure_seed (FpiDeviceCrfpMoc *self, const char * seed, GError **error)
{
  guint32 status;

  fp_dbg ("crfpmoc_cmd_fp_ensure_seed: Checking if seed is set");
  if (!crfpmoc_cmd_fp_enc_status (self, &status, error))
    return FALSE;

  fp_dbg ("crfpmoc_cmd_fp_ensure_seed: FPMCU encryption status: %d", status);

  if (status == 0)
    {
      fp_dbg ("crfpmoc_cmd_fp_ensure_seed: seed is not set, setting seed");

      if (!crfpmoc_cmd_fp_seed (self, seed, error))
        return FALSE;
    }
  else
    {
      fp_dbg ("crfpmoc_cmd_fp_ensure_seed: seed is already set");
    }

  return TRUE;
}

static void
crfpmoc_show_proto_info (struct crfpmoc_ec_response_get_protocol_info *protocol_info)
{
  gsize len = 0;
  gsize buflen = 256;
  g_autofree gchar *buffer = g_malloc (buflen);

  for (int i = 0; i < 32; i++)
    if (protocol_info->protocol_versions & (1ULL << i))
      len += snprintf (&buffer[len], buflen - len, "%s%d", len == 0 ? "" : ",", i);

  buffer[buflen - 1] = '\0';
  fp_dbg ("crfpmoc_ec_max_outsize: vers %s, max_out %d, max_in %d, flags 0x%08x",
          buffer,
          protocol_info->max_request_packet_size,
          protocol_info->max_response_packet_size,
          protocol_info->flags);
}

static gboolean
crfpmoc_ec_max_insize (FpiDeviceCrfpMoc *self, guint16 *max_insize, GError **error)
{
  struct crfpmoc_ec_response_get_protocol_info protocol_info = {0};

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_GET_PROTOCOL_INFO, 0,
                           NULL, 0, &protocol_info, sizeof (protocol_info), error))
    return FALSE;

  crfpmoc_show_proto_info (&protocol_info);
  *max_insize = protocol_info.max_response_packet_size -
                sizeof (struct crfpmoc_ec_host_response);

  return TRUE;
}

static gboolean
crfpmoc_ec_max_outsize (FpiDeviceCrfpMoc *self, guint16 *max_outsize, GError **error)
{
  struct crfpmoc_ec_response_get_protocol_info protocol_info;

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_GET_PROTOCOL_INFO, 0,
                           NULL, 0, &protocol_info, sizeof (protocol_info), error))
    return FALSE;

  crfpmoc_show_proto_info (&protocol_info);
  *max_outsize = protocol_info.max_request_packet_size -
                 sizeof (struct crfpmoc_ec_host_request);

  return TRUE;
}

/* Stolen BSD sum algorithm */
static uint16_t
crfpmoc_sum (guint16 sum, guint8 * buffer, gsize len)
{
  int checksum = sum;             /* The checksum mod 2^16. */
  int i;

  for (i = 0; i < len; i++)
    {
      checksum = (checksum >> 1) + ((checksum & 1) << 15);
      checksum += buffer[i];
      checksum &= 0xffff;       /* Keep it within bounds. */
    }
  return (guint16) checksum;
}

/* Download the last loaded template
 *
 * Issue the EC_CMD_FP_FRAME command to read the template. This is
 * done in ec_max_insize chunks. The command will return EC_RES_BUSY
 * until the last chunk is read at which point it will return
 * EC_REC_SUCCESS. Any other response is considered an error.
 */
static gboolean
crfpmoc_fp_download_template (FpiDeviceCrfpMoc                   *self,
                              struct crfpmoc_ec_response_fp_info *info,
                              gint                                index,
                              guint8                            **out_buffer,
                              GError                            **error)
{
  struct crfpmoc_ec_params_fp_template p = {0};
  gsize stride, size;
  g_autofree guint8 *buffer = NULL;
  guint8 *ptr;
  gint cmdver = 1;
  gsize rsize = sizeof (*info);
  const gint max_attempts = 3;
  gint num_attempts;
  guint16 ec_max_insize;
  gint template_idx = index + CRFPMOC_FP_FRAME_INDEX_TEMPLATE;
  guint16 sum = 0;

  *out_buffer = NULL;

  if (!crfpmoc_ec_max_insize (self, &ec_max_insize, error))
    return FALSE;

  if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_INFO, cmdver,
                           NULL, 0, info, rsize, error))
    return FALSE;

  size = info->template_size;
  fp_dbg ("crfpmoc_fp_download_template: template size %zd", size);
  g_assert (size != 0);

  buffer = g_malloc0 (size);
  ptr = buffer;
  p.offset = template_idx << CRFPMOC_FP_FRAME_INDEX_SHIFT;

  while (size > 0)
    {
      stride = MIN (CRFPMOC_EC_PROTO2_MAX_PARAM_SIZE, size);
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
        }

      if (!g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
        sum = crfpmoc_sum (sum, ptr, stride);

      p.offset += stride;
      size -= stride;
      ptr += stride;
    }

  fp_dbg ("crfpmoc_fp_download_template: done, %lu bytes read, sum %d",
          ptr - buffer, sum);

  *out_buffer = g_steal_pointer (&buffer);
  return TRUE;
}

static gboolean
crfpmoc_fp_upload_template (FpiDeviceCrfpMoc *self,
                            const guint8     *template_data,
                            gsize             template_size,
                            guint16           ec_max_outsize,
                            GError          **error)
{
  guint32 offset = 0;
  guint32 tlen;
  gsize struct_size;
  guint32 max_chunk;
  guint16 sum = 0;

  max_chunk = CRFPMOC_EC_PROTO2_MAX_PARAM_SIZE -
              offsetof (struct crfpmoc_ec_params_fp_template, data) - 4;

  while (template_size > 0)
    {
      g_autofree struct crfpmoc_ec_params_fp_template *p = NULL;

      tlen = MIN (max_chunk, template_size);
      struct_size = offsetof (struct crfpmoc_ec_params_fp_template, data) + tlen;
      p = g_malloc0 (struct_size);
      p->offset = offset;
      p->size = tlen;

      template_size -= tlen;

      if (!template_size)
        p->size |= CRFPMOC_FP_TEMPLATE_COMMIT;

      memcpy (p->data, template_data + offset, tlen);

      if (!g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
        sum = crfpmoc_sum (sum, p->data, tlen);

      if (!crfpmoc_ec_command (self, CRFPMOC_EC_CMD_FP_TEMPLATE, 0,
                               p, struct_size, NULL, 0, error))
        {
          g_prefix_error (error, "Failed to upload template chunk at offset %u: ",
                          offset);
          return FALSE;
        }

      offset += tlen;
    }

  fp_dbg ("crfpmoc_fp_upload_template: uploaded %d bytes, sum %d",
          offset, sum);

  return TRUE;
}


static void
crfpmoc_cmd_wait_event_fingerprint (FpiSsm *ssm)
{
  long event_type = CRFPMOC_EC_MKBP_EVENT_FINGERPRINT;

  crfpmoc_ec_pollevent (ssm, 1 << event_type);
}

/* Generic task completion callback to report any errors */
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
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  const char *device_path;

  device_path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_MISC);
  fp_dbg ("Opening device %s", device_path);

  /* For testing we need to create the umockdev trace file ourselves
   * since there is no record support, while there is playback support.
   */
  if (fpi_device_emulation_mode_enabled (device) &&
      g_getenv ("FPI_CRFPMOC_IOCTL_RECORD"))
    {
      g_autofree char *ioctl_file = NULL;

      self->emul_fd = g_file_open_tmp ("crfpmoc-XXXXXX.ioctl", &ioctl_file, &error);
      if (self->emul_fd < 0)
        {
          fpi_device_open_complete (device, g_steal_pointer (&error));
          return;
        }

      fp_dbg ("Created temporary ioctl file for recording: %s", ioctl_file);
    }

  /* Just for debug messages */
  guint16 ec_max_outsize;
  if (!crfpmoc_ec_max_outsize (self, &ec_max_outsize, &error))
    {
      fp_err ("Failed to get max outsize: %s", error->message);
      g_clear_error (&error);
    }

  /* Just for debug messages */
  guint16 ec_max_insize;
  if (!crfpmoc_ec_max_insize (self, &ec_max_insize, &error))
    {
      fp_err ("Failed to get max insize: %s", error->message);
      g_clear_error (&error);
    }

  fp_dbg ("crfpmoc_open: open_complete");
  fpi_device_open_complete (device, NULL);
}

static gboolean
crfpmoc_set_keys (FpiDeviceCrfpMoc *self, GError **error)
{
  if (!crfpmoc_cmd_fp_ensure_seed (self, CRFPMOC_DEFAULT_SEED, error))
    return FALSE;

  if (!crfpmoc_fp_set_context (self, CRFPMOC_DEFAULT_CONTEXT, error))
    return FALSE;

  return TRUE;
}

static void
crfpmoc_cancel (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Cancel");

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
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("Closing device");

  crfpmoc_cancel (device);
  g_clear_fd (&self->emul_fd, &error);

  fpi_device_close_complete (device, g_steal_pointer (&error));
}

static void
handle_enroll_sensor_enroll (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (fpi_ssm_get_device (ssm));
  guint32 mode;

  if (!crfpmoc_cmd_fp_mode (self,
                            CRFPMOC_FP_MODE_ENROLL_IMAGE |
                            CRFPMOC_FP_MODE_ENROLL_SESSION,
                            &mode, &error))
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  else
    fpi_ssm_next_state (ssm);
}

static void
handle_enroll_wait_enroll_complete (FpiSsm *ssm)
{
  FpDevice *device = fpi_ssm_get_device (ssm);

  /* Sensor awaits the finger down */
  fpi_device_report_finger_status_changes (device,
                                           FP_FINGER_STATUS_NEEDED,
                                           FP_FINGER_STATUS_NONE);
  crfpmoc_cmd_wait_event_fingerprint (ssm);
}

static void
crfpmoc_ssm_timeout (FpDevice *device, gpointer user_data)
{
  SubSsmData *data = user_data;

  fp_dbg ("crfpmoc_ssm_timeout: move to state %d",
          data->timeout_state);

  /* Timeout should be cancelled, make sure cleanup does not see it */
  data->timeout = NULL;

  /* If there is an active poller on this state machine remove it */
  fp_dbg ("crfpmoc_ssm_timeout: remove poll source %d", data->poll_source);
  g_clear_handle_id (&data->poll_source, g_source_remove);

  /* Is it ok to call the state machine from here ? If not make sure we exit first */
  fpi_ssm_jump_to_state_delayed (data->parent_ssm, data->timeout_state, 1);
}

static void
handle_wait_finger_up (FpiSsm *ssm,
                       int     state,
                       int     timeout)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  SubSsmData *data = fpi_ssm_get_data (ssm);
  guint32 mode;

  data->timeout_state = state;
  data->timeout = fpi_device_add_timeout (device, timeout,
                                          crfpmoc_ssm_timeout, data, NULL);

  fp_dbg ("handle_wait_finger_up: start, added timeout %p", data->timeout);

  /* Tell the upper layers the used should remove finger. This does
   * not currently seem to do much. To actually get the upper layers
   * to prompt the user it seems a retry error needs to be returned
   * in a report.
   */
  fpi_device_report_finger_status_changes (device,
                                           FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_NEEDED);

  if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_FINGER_UP, &mode, &error))
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));

  crfpmoc_cmd_wait_event_fingerprint (ssm);
}

static void
handle_enroll_sensor_check (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  EnrollData *enroll_data = fpi_ssm_get_data (ssm);
  guint32 mode;

  if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_DONT_CHANGE, &mode, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  if (mode & CRFPMOC_FP_MODE_ENROLL_SESSION)
    {
      if (mode & CRFPMOC_FP_MODE_ENROLL_IMAGE)
        {
          fpi_ssm_jump_to_state (ssm, ENROLL_WAIT_ENROLL_COMPLETE);
        }
      else
        {
          fpi_device_report_finger_status_changes (device,
                                                   FP_FINGER_STATUS_PRESENT,
                                                   FP_FINGER_STATUS_NONE);

          fpi_device_enroll_progress (device, enroll_data->stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                "FP mode:  (0x%x)",
                                                                mode));

          fpi_ssm_jump_to_state (ssm, ENROLL_SENSOR_ENROLL);
        }
    }
  else if (mode == 0)
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);

      fpi_ssm_next_state (ssm);
    }
  else
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);

      fpi_device_enroll_progress (device, enroll_data->stage, NULL,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                            "FP mode: (0x%x)", mode));

      fpi_ssm_jump_to_state (ssm, ENROLL_SENSOR_ENROLL);
    }
}

static void
handle_enroll_commit (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  guint16 enrolled_templates = 0;
  guint16 ec_max_outsize = 0;
  g_autofree guint8 *template = NULL;
  g_autofree gchar *user_id = NULL;
  struct crfpmoc_ec_response_fp_info info;
  FpPrint *print;
  int template_index;

  if (!crfpmoc_cmd_fp_info (self, &enrolled_templates, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fp_dbg ("Number of enrolled templates is: %d", enrolled_templates);

  template_index = enrolled_templates - 1;
  fpi_device_get_enroll_data (device, &print);

  if (!crfpmoc_ec_max_outsize (self, &ec_max_outsize, &error))
    {
      fp_err ("Failed to get max outsize");
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Failed to get max outsize: %s",
                                                          error->message));
      return;
    }

  if (!crfpmoc_fp_download_template (self, &info, template_index, &template, &error))
    {
      fp_err ("Failed to download template");
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Failed to download template: %s",
                                                          error->message));
      return;
    }

  user_id = fpi_print_generate_user_id (print);
  fp_dbg ("New fingerprint ID: %s", user_id);

  g_object_set (print, "description", user_id, NULL);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  crfpmoc_set_print_data (print, template, info.template_size);

  fp_info ("handle_enroll_commit: enroll_complete");
  fpi_device_enroll_complete (device, g_object_ref (print), NULL);

  fpi_ssm_mark_completed (ssm);
}

static void
handle_ensure_keys (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  if (!crfpmoc_set_keys (self, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
crfpmoc_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(GError) error = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_ENSURE_KEYS:
      handle_ensure_keys (ssm);
      break;

    case ENROLL_SENSOR_ENROLL:
      handle_enroll_sensor_enroll (ssm);
      break;

    case ENROLL_WAIT_ENROLL_COMPLETE:
      handle_enroll_wait_enroll_complete (ssm);
      break;

#ifdef WAIT_ON_ENROLL
    /* During self test recording following an enroll with a verify
     * without lifting the finger seems to randomly fail. This
     * solves the issue but leaves the default enroll screen waiting
     * for the finger up with no user prompt. It will only complete
     * when the finger is removed. Seems to be a poor user interaction.
     */
    case ENROLL_WAIT_FINGER_UP:
      handle_wait_finger_up (ssm);
      break;
#endif

    case ENROLL_SENSOR_CHECK:
      handle_enroll_sensor_check (ssm);
      break;

    case ENROLL_COMMIT:
      handle_enroll_commit (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
crfpmoc_enroll (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  EnrollData *enroll_data = NULL;

  fp_dbg ("Enroll");

  enroll_data = g_new0 (EnrollData, 1);
  enroll_data->stage = 0;

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, crfpmoc_enroll_run_state, ENROLL_STATES);
  fpi_ssm_set_data (self->task_ssm, g_steal_pointer (&enroll_data), g_free);
  fpi_ssm_start (self->task_ssm, crfpmoc_task_ssm_done);
}

static void
handle_verify_upload_template (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  guint16 ec_max_outsize = 0;
  g_autofree guint8 *template = NULL;
  size_t template_size = 0;
  FpPrint *print = NULL;

  if (!crfpmoc_ec_max_outsize (self, &ec_max_outsize, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *prints = NULL;

      fpi_device_get_identify_data (device, &prints);

      for (guint i = 0; i < prints->len; i++)
        {
          print = g_ptr_array_index (prints, i);

          if (!crfpmoc_get_print_data (print, &template, &template_size, &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              return;
            }

          if (!ec_max_outsize)
            {
              /*  TODO: If this issue can't be fixed, we should only get the max
               * outsize once and use it for all templates.
               */
              fp_warn ("Max outsize is not set, getting it from the device");

              if (!crfpmoc_ec_max_outsize (self, &ec_max_outsize, &error))
                {
                  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
                  return;
                }
            }

          if (!crfpmoc_fp_upload_template (self, template, template_size,
                                           ec_max_outsize, &error))
            {
              fp_warn ("Failed to upload template nr %d: %s", i,
                       error->message);
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              return;
            }

          fp_dbg ("uploaded template %u, %zd bytes", i, template_size);
        }
    }
  else
    {
      fpi_device_get_verify_data (device, &print);

      if (!crfpmoc_get_print_data (print, &template, &template_size, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          return;
        }

      if (!ec_max_outsize)
        {
          fp_warn ("Max outsize is not set, getting it from the device");
          if (!crfpmoc_ec_max_outsize (self, &ec_max_outsize, &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              return;
            }
        }

      if (!crfpmoc_fp_upload_template (self, template, template_size,
                                       ec_max_outsize, &error))
        {
          fp_warn ("Failed to upload template: %s", error->message);
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          return;
        }

      fp_dbg ("uploaded template, %zd bytes", template_size);
    }

  fpi_ssm_next_state (ssm);
}

static void
handle_verify_sensor_match (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  guint32 mode;

  if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_MATCH, &mode, &error))
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  else
    fpi_ssm_next_state (ssm);
}

static void
handle_verify_wait_match_complete (FpiSsm *ssm)
{
  FpDevice *device = fpi_ssm_get_device (ssm);

  /* Sensor awaits the finger down */
  fpi_device_report_finger_status_changes (device,
                                           FP_FINGER_STATUS_NEEDED,
                                           FP_FINGER_STATUS_NONE);

  crfpmoc_cmd_wait_event_fingerprint (ssm);
}

static void
handle_verify_sensor_check (FpiSsm *ssm)
{
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  g_autoptr(GError) error = NULL;
  guint32 mode;

  if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_DONT_CHANGE, &mode, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  if (mode & CRFPMOC_FP_MODE_MATCH)
    {
      fpi_ssm_jump_to_state (ssm, VERIFY_WAIT_MATCH_COMPLETE);
    }
  else if (mode == 0)
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
      fpi_ssm_next_state (ssm);
    }
  else
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);

      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "FP mode: (0x%x)", mode));
    }
}

static void
handle_verify_check (FpiSsm *ssm)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = fpi_ssm_get_device (ssm);
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);
  gint8 template_idx = -1;
  GPtrArray *prints = NULL;
  FpPrint *print = NULL;
  guint16 enrolled_templates = 0;

  if (!crfpmoc_cmd_fp_info (self, &enrolled_templates, &error))
    {
      fp_dbg ("handle_verify_check: no info");
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fp_dbg ("handle_verify_check: enrolled templates: %d", enrolled_templates);

  if (!crfpmoc_cmd_fp_stats (self, &template_idx, &error))
    {
      fp_dbg ("handle_verify_check: no stats");
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  if (template_idx < 0)
    {
      fp_info ("Print was not identified by the device");

      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
        fpi_device_identify_report (device, NULL, NULL, NULL);
      else
        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);
    }
  else
    {
      fp_info ("Identify successful for template %d", template_idx);

      print = fp_print_new (device);
      fpi_print_set_type (print, FPI_PRINT_RAW);

      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
        {
          fpi_device_get_identify_data (device, &prints);
          if (prints && template_idx < (gint) prints->len)
            {
              FpPrint *matched_print = g_ptr_array_index (prints, template_idx);
              g_autofree guint8 *matched_template = NULL;
              size_t matched_template_size = 0;

              if (!crfpmoc_get_print_data (matched_print,
                                           &matched_template, &matched_template_size,
                                           &error))
                {
                  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
                  return;
                }

              crfpmoc_set_print_data (print, matched_template, matched_template_size);
              fpi_device_identify_report (device, matched_print, print, NULL);
            }
          else
            {
              /* This should not occur as we can only match prints
               * from the gallery
               */
              fp_warn ("Template index %d is out of range", template_idx);
              fpi_device_identify_report (device, NULL, print, NULL);
            }
        }
      else if (template_idx == 0)
        {
          FpPrint *verify_print = NULL;

          fpi_device_get_verify_data (device, &verify_print);
          fp_info ("Verifying against: %s",
                   fp_print_get_description (verify_print));

          g_autofree guint8 *matched_template = NULL;
          size_t matched_template_size = 0;

          if (!crfpmoc_get_print_data (verify_print,
                                       &matched_template, &matched_template_size,
                                       &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              return;
            }

          crfpmoc_set_print_data (print, matched_template, matched_template_size);
          fpi_device_verify_report (device, FPI_MATCH_SUCCESS, print, NULL);
        }
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      fp_dbg ("handle_verify_check: identify_complete");
      fpi_device_identify_complete (device, NULL);
    }
  else
    {
      fp_dbg ("handle_verify_check: verify_complete");
      fpi_device_verify_complete (device, NULL);
    }

  fpi_ssm_mark_completed (ssm);
}

static void
crfpmoc_cmd_wait_for_it (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (fpi_ssm_get_device (ssm));
  guint mode;

  if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_DONT_CHANGE, &mode, &error))
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  else if (mode == 0)
    fpi_ssm_next_state (ssm);
  else
    fpi_ssm_jump_to_state_delayed (ssm, fpi_ssm_get_cur_state (ssm), 100);
}

static void
handle_verify_clear_storage (FpiSsm *ssm)
{
  FpDevice *device = fpi_ssm_get_device (ssm);

  FpiSsm *sub = fpi_ssm_new (device, crfpmoc_clear_storage_run_state,
                             CLEAR_STORAGE_STATES);

  crfpmoc_ssm_start_subsm (FPI_DEVICE_CRFPMOC (device), sub);
}

static void
handle_verify_finger_up (FpiSsm *ssm)
{
  FpDevice *device = fpi_ssm_get_device (ssm);

  FpiSsm *sub = fpi_ssm_new (device, crfpmoc_finger_up_run_state,
                             FINGER_UP_STATES);

  crfpmoc_ssm_start_subsm (FPI_DEVICE_CRFPMOC (device), sub);
}

static void
crfpmoc_verify_run_state (FpiSsm *ssm, FpDevice *device)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* Make sure no residual templates are left in the sensor */
    case VERIFY_CLEAR_STORAGE:
      handle_verify_clear_storage (ssm);
      break;

    /* Make sure the finger is actually up */
    case VERIFY_FINGER_UP:
      handle_verify_finger_up (ssm);
      break;

    /* Set the seed and the context */
    case VERIFY_ENSURE_KEYS:
      handle_ensure_keys (ssm);
      break;

    /* Upload the supplied template(s) for match */
    case VERIFY_UPLOAD_TEMPLATE:
      handle_verify_upload_template (ssm);
      break;

    /* Switch to match mode */
    case VERIFY_SENSOR_MATCH:
      handle_verify_sensor_match (ssm);
      break;

    /* Wait for the match to report completion event */
    case VERIFY_WAIT_MATCH_COMPLETE:
      handle_verify_wait_match_complete (ssm);
      break;

    /* Wait for the match mode to indicate complete */
    case VERIFY_SENSOR_CHECK:
      handle_verify_sensor_check (ssm);
      break;

    /* Get the match status and report it */
    case VERIFY_CHECK:
      handle_verify_check (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
crfpmoc_finger_up_run_state (FpiSsm *ssm, FpDevice *device)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FINGER_UP_START:
      handle_wait_finger_up (ssm, FINGER_UP_TIMEOUT, 5000);
      break;

    case FINGER_UP_DONE:
      fpi_ssm_mark_completed (ssm);
      break;

    /* User has not lifted finger. Send a retry error to prompt them */
    case FINGER_UP_TIMEOUT:
      fpi_ssm_mark_failed (ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      break;
    }
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
      if (!crfpmoc_cmd_fp_mode (self, 0, &mode, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }

      if (!crfpmoc_cmd_fp_mode (self, CRFPMOC_FP_MODE_RESET_SENSOR, &mode, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }

      fpi_ssm_next_state (ssm);
      break;

    case CLEAR_STORAGE_SENSOR_WAIT:
      crfpmoc_cmd_wait_for_it (ssm, device);
      break;

    case CLEAR_STORAGE_SENSOR_DONE:
      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_CLEAR_STORAGE)
        {
          fp_dbg ("crfpmoc_clear_storage_run_state: clear_storage_complete");
          fpi_device_clear_storage_complete (device, NULL);
        }
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
crfpmoc_verify_ssm_done (FpiSsm *ssm, FpDevice *device,
                         GError *error)
{
  fp_dbg ("crfpmoc_verify_ssm_done");
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  g_assert (!self->task_ssm || self->task_ssm == ssm);
  self->task_ssm = NULL;

  /* If the state machine failed the report and completion was not
   * sent. Send it here.
   */
  if (error)
    {
      if (error->domain == FP_DEVICE_RETRY)
        {
          if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
            fpi_device_verify_report (device, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
          else
            fpi_device_identify_report (device, NULL, NULL, g_steal_pointer (&error));
        }

      if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
        {
          fp_dbg ("crfpmoc_verify_ssm_done: verify_complete");
          fpi_device_verify_complete (device, error);
        }
      else
        {
          fp_dbg ("crfpmoc_verify_ssm_done: identify_complete");
          fpi_device_identify_complete (device, error);
        }
    }
  crfpmoc_cancel (device);
}

static void
crfpmoc_identify_verify (FpDevice *device)
{
  FpiDeviceCrfpMoc *self = FPI_DEVICE_CRFPMOC (device);

  fp_dbg ("%s",
          fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_IDENTIFY ?
          "Identify" : "Verify");

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, crfpmoc_verify_run_state, VERIFY_STATES);
  crfpmoc_ssm_start (self->task_ssm, crfpmoc_verify_ssm_done);
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
  self->emul_fd = -1;
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
