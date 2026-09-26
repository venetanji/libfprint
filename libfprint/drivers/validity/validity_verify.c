/*
 * Verify and Identify state machines for Validity/Synaptics VCSFW sensors
 *
 * Implements FpDevice::verify and FpDevice::identify virtual methods.
 *
 * Both operations share the same state machine since the sensor-side
 * flow is nearly identical:
 *   1. LED on
 *   2. Send capture command (IDENTIFY mode)
 *   3. Wait for finger-down interrupt (EP 0x83)
 *   4. Wait for scan-complete interrupt
 *   5. Start match (cmd 0x5E)
 *   6. Wait for match interrupt
 *   7. Get match result (cmd 0x60)
 *   8. Match cleanup (cmd 0x62)
 *   9. LED off
 *   10. Read back the matched user record (cmd 0x4A)
 *   11. Report result: a match only for the print whose user record and
 *       finger the chip matched
 *
 * The difference between verify and identify is only in how the
 * result is reported to libfprint.
 *
 * Also implements list and delete operations.
 *
 * Reference: python-validity sensor.py Sensor.identify(), Sensor.match_finger()
 *
 * Copyright (C) 2024 libfprint contributors
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

#define FP_COMPONENT "validity"

#include "drivers_api.h"
#include "validity_pack.h"
#include "fpi-print.h"
#include "validity.h"
#include "vcsfw_protocol.h"

/* ================================================================
 * Interrupt helpers (shared with enrollment)
 * ================================================================ */

static void
verify_interrupt_cb (FpiUsbTransfer *transfer,
                     FpDevice       *device,
                     gpointer        user_data,
                     GError         *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm = user_data;
  guint8 int_type;

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
          g_error_free (error);
          return;
        }
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (transfer->actual_length < 1)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  int_type = transfer->buffer[0];

  if (transfer->actual_length >= 5)
    {
      fp_dbg ("Verify interrupt: type=0x%02x bytes=[%02x %02x %02x %02x %02x] (len=%" G_GSSIZE_FORMAT ")",
              int_type, transfer->buffer[0], transfer->buffer[1],
              transfer->buffer[2], transfer->buffer[3], transfer->buffer[4],
              transfer->actual_length);
    }
  else
    {
      fp_dbg ("Verify interrupt: type=0x%02x (len=%" G_GSSIZE_FORMAT ")",
              int_type, transfer->actual_length);
    }

  /* Match result: type 3 = match found, anything else = no match.
   * On no-match, skip cmd 0x60 (get_match_result) and go to cleanup;
   * the sensor only sends one interrupt per match attempt. */
  if (fpi_ssm_get_cur_state (ssm) == VERIFY_WAIT_MATCH_INT)
    {
      if (int_type == 3)
        {
          fpi_ssm_next_state (ssm);
          return;
        }

      /* No match — clear any stale result and skip to cleanup */
      fp_info ("Match interrupt type=%d: no match", int_type);
      g_clear_pointer (&self->bulk_data, g_free);
      self->bulk_data_len = 0;
      fpi_ssm_jump_to_state (ssm, VERIFY_CLEANUP);
      return;
    }

  /* During finger wait: type 2 = finger down */
  if (fpi_ssm_get_cur_state (ssm) == VERIFY_WAIT_FINGER)
    {
      if (int_type == VALIDITY_INT_FINGER_DOWN)
        {
          fpi_ssm_next_state (ssm);
          return;
        }
      /* 0x969/0xd51 silicon omits the type-2 finger-down and signals the
       * finger with a type-3 capture event. Accept it as finger-down; if it
       * already carries the scan-complete bit, skip VERIFY_WAIT_SCAN_COMPLETE
       * (a fresh read there would hang waiting for a type-3 that already
       * arrived). */
      if (int_type == 3 && self->sensor.device_info &&
          validity_sensor_skips_finger_down (self->sensor.real_sensor_type))
        {
          if (transfer->actual_length >= 3 &&
              (transfer->buffer[2] & VALIDITY_INT_SCAN_COMPLETE))
            fpi_ssm_jump_to_state (ssm, VERIFY_GET_PRG_STATUS);
          else
            fpi_ssm_next_state (ssm);
          return;
        }
      /* type 0 = capture started, expected */
      if (int_type == 0)
        goto read_again;
    }

  /* During scan wait: type 3 with scan_complete bit */
  if (fpi_ssm_get_cur_state (ssm) == VERIFY_WAIT_SCAN_COMPLETE)
    {
      if (int_type == 3 && transfer->actual_length >= 3 &&
          (transfer->buffer[2] & VALIDITY_INT_SCAN_COMPLETE))
        {
          fpi_ssm_next_state (ssm);
          return;
        }
      if (int_type == 3 || int_type == 0)
        goto read_again;
    }

  /* Unexpected, but keep listening */
  fp_dbg ("Ignoring verify interrupt type 0x%02x in state %d",
          int_type, fpi_ssm_get_cur_state (ssm));

read_again:
  {
    FpiUsbTransfer *new_transfer = fpi_usb_transfer_new (device);
    fpi_usb_transfer_fill_interrupt (new_transfer, VALIDITY_EP_INT_IN,
                                     VALIDITY_USB_INT_DATA_SIZE);
    fpi_usb_transfer_submit (new_transfer, 0,
                             fpi_device_get_cancellable (FP_DEVICE (self)),
                             verify_interrupt_cb, ssm);
  }
}

static void
verify_start_interrupt_wait (FpiDeviceValidity *self,
                             FpiSsm            *ssm)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_interrupt (transfer, VALIDITY_EP_INT_IN,
                                   VALIDITY_USB_INT_DATA_SIZE);
  /* Use no timeout (0) for finger-wait and scan-complete states,
   * since these wait for physical user interaction. The action's
   * GCancellable (fpi_device_get_cancellable) aborts the wait on cancel. */
  fpi_usb_transfer_submit (transfer, 0,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           verify_interrupt_cb, ssm);
}

/* ================================================================
 * Match result parsing
 *
 * cmd 0x60 response: len(2LE) | dict_data[len]
 * dict_data is a TLV dictionary (python-validity parse_dict()):
 *   while data: tag(2LE) | len(2LE) | value[len]
 * In the match response:
 *   tag 1 → user_dbid(4LE)
 *   tag 3 → subtype(2LE)
 *   tag 4 → hash (variable length)
 * ================================================================ */

/* MatchResult is now ValidityMatchResult in validity_db.h */
typedef ValidityMatchResult MatchResult;

static void
match_result_clear (MatchResult *r)
{
  validity_match_result_clear (r);
}

/**
 * parse_match_result:
 * @data: response payload from cmd 0x60 (after status word)
 * @data_len: length of @data
 * @result: (out): parsed match result
 *
 * Parses the TLV dictionary from a get_match_result (cmd 0x60) response.
 *
 * python-validity reference (sensor.py match_finger()):
 *   rsp = self.parse_dict(rsp)
 *   usrid, subtype, hsh = rsp[1], rsp[3], rsp[4]
 *
 * where parse_dict() is:
 *   while len(x) > 0:
 *       (t, l), x = unpack('<HH', x[:4]), x[4:]
 *       rc[t], x = x[:l], x[l:]
 *
 * Returns: %TRUE if parsing succeeded (result may still be !matched
 *          if the dict was empty), %FALSE on malformed data.
 */
gboolean
validity_parse_match_result (const guint8        *data,
                             gsize                data_len,
                             ValidityMatchResult *result)
{
  FpiByteReader reader;
  guint16 total_len;

  memset (result, 0, sizeof (*result));

  if (data_len < 2)
    return FALSE;

  fpi_byte_reader_init (&reader, data, data_len);

  if (!fpi_byte_reader_get_uint16_le (&reader, &total_len))
    return FALSE;

  gsize remaining = MIN (total_len, data_len - 2);
  const guint8 *dict_data;

  if (!fpi_byte_reader_get_data (&reader, remaining, &dict_data))
    return FALSE;

  /* Parse TLV entries: tag(2LE) | len(2LE) | value[len] */
  FpiByteReader dict_reader;
  fpi_byte_reader_init (&dict_reader, dict_data, remaining);

  while (fpi_byte_reader_get_remaining (&dict_reader) >= 4)
    {
      guint16 tag, entry_len;
      const guint8 *entry_data;

      if (!fpi_byte_reader_get_uint16_le (&dict_reader, &tag))
        break;
      if (!fpi_byte_reader_get_uint16_le (&dict_reader, &entry_len))
        break;
      if (!fpi_byte_reader_get_data (&dict_reader, entry_len, &entry_data))
        break;

      switch (tag)
        {
        case 1: /* user_dbid (4 bytes LE) */
          {
            FpiByteReader entry_reader;
            fpi_byte_reader_init (&entry_reader, entry_data, entry_len);
            if (fpi_byte_reader_get_uint32_le (&entry_reader, &result->user_dbid))
              result->matched = TRUE;
          }
          break;

        case 3: /* subtype (2 bytes LE) */
          {
            FpiByteReader entry_reader;
            fpi_byte_reader_init (&entry_reader, entry_data, entry_len);
            fpi_byte_reader_get_uint16_le (&entry_reader, &result->subtype);
          }
          break;

        case 4: /* hash (variable) */
          if (entry_len > 0)
            {
              result->hash = g_memdup2 (entry_data, entry_len);
              result->hash_len = entry_len;
            }
          break;

        default:
          fp_dbg ("parse_match_result: ignoring unknown tag %u (len=%u)",
                  tag, entry_len);
          break;
        }
    }

  return TRUE;
}

/* TRUE if @user's identity is @print's fpi-data id string, compared by length
 * and bytes. Every enrollment creates its own user record under the id string
 * that enroll also stores as the print's fpi-data, so this is what ties a
 * print to its chip record. A print with no or an empty id string, or a
 * record without a readable identity, belongs to nothing. */
static gboolean
print_owns_record (FpPrint            *print,
                   const ValidityUser *user)
{
  g_autoptr(GVariant) data = NULL;
  const gchar *print_id;
  gsize print_id_len;
  const guint8 *record_id;
  gsize record_id_len;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE_STRING))
    return FALSE;

  print_id = g_variant_get_string (data, &print_id_len);
  if (print_id_len == 0)
    return FALSE;

  return validity_db_user_get_id (user, &record_id, &record_id_len) &&
         record_id_len == print_id_len &&
         memcmp (record_id, print_id, print_id_len) == 0;
}

/**
 * validity_print_matches_record:
 * @print: an enrolled print (from enroll, list, or deserialised)
 * @match: the chip's match result (cmd 0x60)
 * @user: the user record @match names, read back with cmd 0x4a
 *
 * The match command searches every template on the chip (python-validity
 * match_finger(): stg_id = usr_id = 0), so a match only says that *some*
 * enrolled finger matched. Decide whether it was @print's.
 *
 * Every enrollment creates its own user record whose identity is the id
 * string that enroll also stores as the print's fpi-data, so the match is
 * @print's only if all of these hold:
 *   - @user is the record the chip named (@match->user_dbid),
 *   - the record's identity equals @print's fpi-data id string,
 *   - the matched finger subtype is @print's finger, and the record holds
 *     a finger of that subtype.
 * Anything short of that, including a print with no id string, is not a
 * match. python-validity's VerifyStart makes the same check on the user
 * record (usr.dbid == usrid).
 *
 * Returns: %TRUE if the chip's match is a match for @print
 */
gboolean
validity_print_matches_record (FpPrint                   *print,
                               const ValidityMatchResult *match,
                               const ValidityUser        *user)
{
  gboolean has_finger = FALSE;

  if (!print || !match || !match->matched || !user)
    return FALSE;

  if (user->dbid != match->user_dbid)
    return FALSE;

  if (!print_owns_record (print, user))
    return FALSE;

  if (validity_finger_to_subtype (fp_print_get_finger (print)) != match->subtype)
    return FALSE;

  for (guint16 i = 0; i < user->finger_count; i++)
    if (user->fingers[i].subtype == match->subtype)
      has_finger = TRUE;

  return has_finger;
}

/**
 * validity_find_gallery_match:
 * @gallery: (element-type FpPrint): array of gallery prints
 * @match: the chip's match result (cmd 0x60)
 * @user: the user record @match names
 *
 * Find the gallery print the chip's match belongs to, as decided by
 * validity_print_matches_record(). The chip can match a finger that is not
 * in the gallery (another user's print, or one the caller left out), so
 * there is no fallback: an uncorrelated match is no match.
 *
 * Returns: (nullable): the matching FpPrint, or %NULL if none corresponds
 */
FpPrint *
validity_find_gallery_match (GPtrArray                 *gallery,
                             const ValidityMatchResult *match,
                             const ValidityUser        *user)
{
  if (!gallery)
    return NULL;

  for (guint i = 0; i < gallery->len; i++)
    {
      FpPrint *candidate = g_ptr_array_index (gallery, i);

      if (validity_print_matches_record (candidate, match, user))
        return candidate;
    }

  return NULL;
}

/**
 * validity_match_decide:
 * @identify: %TRUE for identify, %FALSE for verify
 * @print: (nullable): the print being verified (verify)
 * @gallery: (nullable) (element-type FpPrint): the gallery (identify)
 * @match: (nullable): this attempt's get_match_result (cmd 0x60), %NULL if
 *   there was none or it could not be parsed
 * @user: the user record @match names, as read back this attempt; empty if
 *   it was not read
 * @out_gallery_match: (out) (transfer none) (optional): identify: the
 *   gallery print that matched, or %NULL
 *
 * The verify/identify decision verify_ssm_done reports. A chip match is a
 * match only for the print its user record and finger belong to (see
 * validity_print_matches_record); no match, or a match whose record was
 * not read back, is no match.
 *
 * Returns: %FPI_MATCH_SUCCESS if @print (verify) or a gallery print
 *   (identify) matched, %FPI_MATCH_FAIL otherwise
 */
FpiMatchResult
validity_match_decide (gboolean                   identify,
                       FpPrint                   *print,
                       GPtrArray                 *gallery,
                       const ValidityMatchResult *match,
                       const ValidityUser        *user,
                       FpPrint                  **out_gallery_match)
{
  FpPrint *gallery_match = NULL;
  gboolean matched = FALSE;

  if (match && match->matched)
    {
      if (identify)
        {
          gallery_match = validity_find_gallery_match (gallery, match, user);
          matched = (gallery_match != NULL);
        }
      else
        {
          matched = validity_print_matches_record (print, match, user);
        }
    }

  if (out_gallery_match)
    *out_gallery_match = gallery_match;

  return matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
}

/**
 * validity_match_user_to_read:
 * @data: (nullable): the stored get_match_result (cmd 0x60) response
 * @data_len: length of @data
 * @out_dbid: (out): the user record to read back
 *
 * Returns: %TRUE if @data reports a match, naming a user record that
 *   get_user (cmd 0x4a) can address, in @out_dbid
 */
gboolean
validity_match_user_to_read (const guint8 *data,
                             gsize         data_len,
                             guint16      *out_dbid)
{
  MatchResult match = { 0 };
  gboolean have_match = FALSE;

  if (data && data_len > 0)
    if (validity_parse_match_result (data, data_len, &match))
      have_match = match.matched;

  if (have_match && match.user_dbid <= G_MAXUINT16)
    *out_dbid = (guint16) match.user_dbid;
  else
    have_match = FALSE;

  match_result_clear (&match);
  return have_match;
}

/**
 * validity_verify_clear_match_state:
 * @self: the device
 *
 * Drop the stored match result and the matched user record, so that a
 * verify or identify decides only on its own get_match_result and
 * get_user responses. Run when an attempt starts and when it ends.
 */
void
validity_verify_clear_match_state (FpiDeviceValidity *self)
{
  g_clear_pointer (&self->bulk_data, g_free);
  self->bulk_data_len = 0;
  validity_user_clear (&self->verify_match_user);
}

/* ================================================================
 * Verify/Identify SSM
 * ================================================================ */

static void
verify_build_capture (FpiSsm            *ssm,
                      FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd = validity_capture_build_cmd_02 (&self->capture,
                                               self->sensor.type_info,
                                               VALIDITY_CAPTURE_IDENTIFY,
                                               &cmd_len);

  if (!cmd)
    {
      fp_warn ("Failed to build identify capture command");
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
verify_capture_recv (FpiSsm            *ssm,
                     FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("Capture (identify) failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
verify_match_start_recv (FpiSsm            *ssm,
                         FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("match_finger failed: status=0x%04x",
               self->cmd_response_status);
      /* No match — continue to cleanup */
      fpi_ssm_jump_to_state (ssm, VERIFY_CLEANUP);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
verify_get_result_recv (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_info ("No match found (status=0x%04x)",
               self->cmd_response_status);
      /* Store no-match indicator */
      g_clear_pointer (&self->bulk_data, g_free);
      self->bulk_data_len = 0;
    }
  else if (self->cmd_response_data && self->cmd_response_len > 0)
    {
      /* Store match result for later reporting */
      g_clear_pointer (&self->bulk_data, g_free);
      self->bulk_data = g_memdup2 (self->cmd_response_data,
                                   self->cmd_response_len);
      self->bulk_data_len = self->cmd_response_len;
    }

  fpi_ssm_next_state (ssm);
}

static void
verify_get_match_user (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  /* The chip names the matched user record but not which print it belongs
   * to; read the record back so verify_ssm_done can compare its identity
   * with the print(s) being matched. Nothing to read on a no-match. */
  guint16 user_dbid;

  if (!validity_match_user_to_read (self->bulk_data, self->bulk_data_len,
                                    &user_dbid))
    {
      fpi_ssm_jump_to_state (ssm, VERIFY_DONE);
      return;
    }

  {
    gsize cmd_len;
    guint8 *cmd = validity_db_build_cmd_get_user (user_dbid, &cmd_len);
    vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
    g_free (cmd);
  }
}

static void
verify_get_match_user_recv (FpiSsm            *ssm,
                            FpiDeviceValidity *self)
{
  /* On failure verify_match_user stays empty, which correlates with no
   * print: the match is reported as no match. */
  if (self->cmd_response_status != VCSFW_STATUS_OK ||
      !self->cmd_response_data ||
      !validity_db_parse_user (self->cmd_response_data,
                               self->cmd_response_len,
                               &self->verify_match_user))
    fp_warn ("Could not read the matched user record (status=0x%04x)",
             self->cmd_response_status);

  fpi_ssm_next_state (ssm);
}

static void
verify_run_state (FpiSsm   *ssm,
                  FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VERIFY_LED_ON:
      self->verify_capture_retries = 0;
      self->verify_recapture = FALSE;
      /* The match decision must come from this attempt's get_match_result
       * alone; drop anything an earlier action left behind. */
      validity_verify_clear_match_state (self);
      {
        gsize cmd_len;
        const guint8 *cmd = validity_capture_glow_start_cmd (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case VERIFY_LED_ON_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_BUILD_CAPTURE:
      verify_build_capture (ssm, self);
      break;

    case VERIFY_CAPTURE_SEND:
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_CAPTURE_RECV:
      verify_capture_recv (ssm, self);
      break;

    case VERIFY_WAIT_FINGER:
      verify_start_interrupt_wait (self, ssm);
      break;

    case VERIFY_WAIT_SCAN_COMPLETE:
      verify_start_interrupt_wait (self, ssm);
      break;

    case VERIFY_GET_PRG_STATUS:
      {
        const guint8 cmd[] = { 0x51, 0x00, 0x20, 0x00, 0x00 };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case VERIFY_GET_PRG_STATUS_RECV:
      /* Capture status: len(4) | x(2) | y(2) | w1(2) | w2(2) | error(4).
       * A real finger reports non-zero geometry and error==0. An empty
       * "phantom" capture (no finger on the sensor) reports x=0/y=0 and a
       * non-zero error. python-validity raises on this and its identify loop
       * re-captures; mirror that so verify waits for a real finger instead of
       * running a match that can only fail. */
      {
        gboolean empty = TRUE;

        if (self->cmd_response_data && self->cmd_response_len >= 16)
          {
            const guint8 *d = self->cmd_response_data;
            guint16 x   = d[4]  | (d[5] << 8);
            guint16 y   = d[6]  | (d[7] << 8);
            guint16 w1  = d[8]  | (d[9] << 8);
            guint16 w2  = d[10] | (d[11] << 8);
            guint32 err = d[12] | (d[13] << 8) | (d[14] << 16) | ((guint32) d[15] << 24);
            fp_dbg ("Verify capture status: x=%u y=%u w1=%u w2=%u error=0x%08x",
                    x, y, w1, w2, err);
            empty = (err != 0 || x == 0 || y == 0);
          }

        if (empty)
          {
            /* No finger on the sensor yet. Re-arm and wait. Like every other
             * finger-wait in this driver, this is unbounded and terminated by
             * the operation's GCancellable (fprintd's timeout or a user
             * cancel) rather than a fixed driver-side timeout. */
            self->verify_recapture = TRUE;
            fp_dbg ("Verify: empty capture (no finger yet) -- re-arming (attempt %u)",
                    ++self->verify_capture_retries);
          }
        else
          {
            self->verify_capture_retries = 0;
          }
      }
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_CAPTURE_STOP:
      {
        const guint8 cmd[] = { 0x04 };
        vcsfw_tls_cmd_send (self, ssm, cmd, sizeof (cmd), NULL);
      }
      break;

    case VERIFY_CAPTURE_STOP_RECV:
      /* Cleanup status doesn't matter. If the last capture was empty (no
       * finger), loop back to re-arm; a short delay avoids busy-looping the
       * sensor while waiting for the user to place a finger. */
      if (self->verify_recapture)
        {
          self->verify_recapture = FALSE;
          fpi_ssm_jump_to_state_delayed (ssm, VERIFY_BUILD_CAPTURE, 300);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case VERIFY_MATCH_START:
      {
        gsize cmd_len;
        guint8 *cmd = validity_pack_new (&cmd_len, "bbbhhhhh",
                                         VCSFW_CMD_MATCH_FINGER,
                                         0x02, 0xFF, 0, 0, 1, 0, 0);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case VERIFY_MATCH_START_RECV:
      verify_match_start_recv (ssm, self);
      break;

    case VERIFY_WAIT_MATCH_INT:
      /* Wait for interrupt type 3 indicating match result ready */
      verify_start_interrupt_wait (self, ssm);
      break;

    case VERIFY_GET_RESULT:
      {
        gsize cmd_len;
        guint8 *cmd = validity_pack_new (&cmd_len, "bw",
                                         VCSFW_CMD_GET_MATCH_RESULT,
                                         (guint32) 0);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case VERIFY_GET_RESULT_RECV:
      verify_get_result_recv (ssm, self);
      break;

    case VERIFY_CLEANUP:
      {
        gsize cmd_len;
        guint8 *cmd = validity_pack_new (&cmd_len, "bw",
                                         VCSFW_CMD_MATCH_CLEANUP,
                                         (guint32) 0);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case VERIFY_CLEANUP_RECV:
      /* Cleanup status doesn't matter */
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_LED_OFF:
      {
        gsize cmd_len;
        const guint8 *cmd = validity_capture_glow_end_cmd (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
      }
      break;

    case VERIFY_LED_OFF_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case VERIFY_GET_MATCH_USER:
      verify_get_match_user (ssm, self);
      break;

    case VERIFY_GET_MATCH_USER_RECV:
      verify_get_match_user_recv (ssm, self);
      break;

    case VERIFY_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
verify_reconnect_done (FpiDeviceValidity *self,
                       gboolean           success,
                       gpointer           user_data)
{
  FpDevice *dev = FP_DEVICE (self);
  gboolean identify = GPOINTER_TO_INT (user_data);

  if (success)
    {
      fp_info ("Reconnected -- retrying %s", identify ? "identify" : "verify");
      self->recovery_redispatch = TRUE;
      if (identify)
        validity_identify (dev);
      else
        validity_verify (dev);
      return;
    }

  fp_warn ("Reconnect failed -- reporting %s removed",
           identify ? "identify" : "verify");
  if (identify)
    fpi_device_identify_complete (dev,
                                  fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
  else
    fpi_device_verify_complete (dev,
                                fpi_device_error_new (FP_DEVICE_ERROR_REMOVED));
}

static void
verify_ssm_done (FpiSsm   *ssm,
                 FpDevice *dev,
                 GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  if (error)
    {
      validity_verify_clear_match_state (self);

      /* Recoverable mid-operation reset? Reconnect and retry instead of
       * failing the action (bounded; a true removal falls through). */
      if (validity_recover_or_fail (self, error, verify_reconnect_done,
                                    GINT_TO_POINTER (self->identify_mode)))
        {
          g_error_free (error);
          return;
        }

      error = validity_map_removed_error (error);
      if (self->identify_mode)
        fpi_device_identify_complete (dev, error);
      else
        fpi_device_verify_complete (dev, error);
      return;
    }

  /* Parse stored match result */
  MatchResult match = { 0 };
  gboolean have_match = FALSE;
  FpPrint *print = NULL;
  GPtrArray *gallery = NULL;
  FpPrint *gallery_match = NULL;
  FpiMatchResult result;

  if (self->bulk_data && self->bulk_data_len > 0)
    if (validity_parse_match_result (self->bulk_data, self->bulk_data_len, &match))
      have_match = match.matched;

  if (self->identify_mode)
    fpi_device_get_identify_data (dev, &gallery);
  else
    fpi_device_get_verify_data (dev, &print);

  /* The chip matched against every template it holds, so a match is only a
   * match for the print(s) being checked if the matched user record belongs
   * to one of them (see validity_match_decide). */
  result = validity_match_decide (self->identify_mode, print, gallery,
                                  have_match ? &match : NULL,
                                  &self->verify_match_user, &gallery_match);

  if (self->identify_mode)
    {
      if (have_match)
        {
          if (gallery_match)
            fp_info ("Identify matched: user_dbid=%u subtype=%u",
                     match.user_dbid, match.subtype);
          else
            fp_info ("Identify: chip matched user_dbid=%u subtype=%u, "
                     "which is no print in the gallery -- no match",
                     match.user_dbid, match.subtype);
        }

      fpi_device_identify_report (dev, gallery_match, NULL, NULL);
      fpi_device_identify_complete (dev, NULL);
    }
  else
    {
      /* Verify mode */
      if (result == FPI_MATCH_SUCCESS)
        fp_info ("Verify matched: user_dbid=%u subtype=%u",
                 match.user_dbid, match.subtype);
      else if (have_match)
        fp_info ("Verify: chip matched user_dbid=%u subtype=%u, "
                 "which is not the print being verified -- no match",
                 match.user_dbid, match.subtype);
      else
        fp_info ("Verify: no match");

      fpi_device_verify_report (dev, result, NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
    }

  match_result_clear (&match);
  validity_verify_clear_match_state (self);
}

/* ================================================================
 * Emulation mode: virtual verify / identify / list / delete / clear
 * ================================================================ */

static void
emulation_verify (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);

  if (self->emulation_prints->len > 0)
    fpi_device_verify_report (device, FPI_MATCH_SUCCESS, NULL, NULL);
  else
    fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);

  fpi_device_verify_complete (device, NULL);
}

static void
emulation_identify (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  GPtrArray *gallery = NULL;

  fpi_device_get_identify_data (device, &gallery);

  if (self->emulation_prints->len > 0 && gallery && gallery->len > 0)
    {
      fpi_device_identify_report (device,
                                  g_ptr_array_index (gallery, 0),
                                  g_ptr_array_index (self->emulation_prints, 0),
                                  NULL);
    }
  else
    {
      fpi_device_identify_report (device, NULL, NULL, NULL);
    }

  fpi_device_identify_complete (device, NULL);
}

static void
emulation_list (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  GPtrArray *prints = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < self->emulation_prints->len; i++)
    g_ptr_array_add (prints,
                     g_object_ref (g_ptr_array_index (self->emulation_prints, i)));

  fpi_device_list_complete (device, prints, NULL);
}

static void
emulation_delete (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpPrint *print = NULL;

  fpi_device_get_delete_data (device, &print);

  FpFinger target_finger = fp_print_get_finger (print);

  for (guint i = 0; i < self->emulation_prints->len; i++)
    {
      FpPrint *stored = g_ptr_array_index (self->emulation_prints, i);

      if (fp_print_get_finger (stored) == target_finger)
        {
          g_ptr_array_remove_index (self->emulation_prints, i);
          break;
        }
    }

  fpi_device_delete_complete (device, NULL);
}

static void
emulation_clear_storage (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);

  g_ptr_array_set_size (self->emulation_prints, 0);
  fpi_device_clear_storage_complete (device, NULL);
}

void
validity_verify (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_verify (device);
      return;
    }

  self->identify_mode = FALSE;
  if (self->recovery_redispatch)
    self->recovery_redispatch = FALSE;   /* internal retry -- keep the budget */
  else
    self->reconnect_attempts = 0;        /* fresh action -- reset the budget */

  ssm = fpi_ssm_new (device, verify_run_state, VERIFY_NUM_STATES);
  fpi_ssm_start (ssm, verify_ssm_done);
}

void
validity_identify (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_identify (device);
      return;
    }

  self->identify_mode = TRUE;
  if (self->recovery_redispatch)
    self->recovery_redispatch = FALSE;
  else
    self->reconnect_attempts = 0;

  ssm = fpi_ssm_new (device, verify_run_state, VERIFY_NUM_STATES);
  fpi_ssm_start (ssm, verify_ssm_done);
}

/* ================================================================
 * List prints — enumerate enrolled fingerprints from sensor DB
 * ================================================================ */

static void
list_get_storage (FpiSsm            *ssm,
                  FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_get_user_storage (
    VALIDITY_STORAGE_NAME, &cmd_len);

  self->list_user_idx = 0;
  memset (&self->list_storage, 0, sizeof (self->list_storage));
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
list_get_storage_recv (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_info ("No user storage found (status=0x%04x)",
               self->cmd_response_status);
      fpi_ssm_jump_to_state (ssm, LIST_DONE);
      return;
    }

  if (!self->cmd_response_data ||
      !validity_db_parse_user_storage (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &self->list_storage))
    {
      fp_info ("Failed to parse user storage — no enrolled prints");
      fpi_ssm_jump_to_state (ssm, LIST_DONE);
      return;
    }

  fp_info ("Storage '%s': %u users",
           self->list_storage.name ? self->list_storage.name : "",
           self->list_storage.user_count);

  if (self->list_storage.user_count == 0)
    {
      fpi_ssm_jump_to_state (ssm, LIST_DONE);
      return;
    }

  self->list_user_idx = 0;
  fpi_ssm_next_state (ssm);
}

static void
list_get_user (FpiSsm            *ssm,
               FpiDeviceValidity *self)
{
  if (self->list_user_idx >= self->list_storage.user_count)
    {
      fpi_ssm_jump_to_state (ssm, LIST_DONE);
      return;
    }

  guint16 user_dbid = self->list_storage.user_dbids[self->list_user_idx];

  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_get_user (user_dbid, &cmd_len);
  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
list_get_user_recv (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);
  GPtrArray *prints_array = fpi_ssm_get_data (ssm);

  if (self->cmd_response_status == VCSFW_STATUS_OK &&
      self->cmd_response_data)
    {
      ValidityUser user = { 0 };

      if (validity_db_parse_user (self->cmd_response_data,
                                  self->cmd_response_len,
                                  &user))
        {
          for (guint16 i = 0; i < user.finger_count; i++)
            {
              FpPrint *print = fp_print_new (dev);
              gint finger = validity_subtype_to_finger (
                user.fingers[i].subtype);

              fpi_print_set_type (print, FPI_PRINT_RAW);
              fpi_print_set_device_stored (print, TRUE);
              if (finger >= 0)
                fp_print_set_finger (print, (FpFinger) finger);

              /* Set fpi-data to the identity id string, matching what
               * enroll stores (g_variant_new_string(user_id)), so the print
               * serialises and fp_print_equal() can match it against a saved
               * print. */
              {
                const guint8 *id_bytes = NULL;
                gsize id_len = 0;

                if (validity_db_user_get_id (&user, &id_bytes, &id_len))
                  {
                    g_autofree gchar *id =
                      g_strndup ((const gchar *) id_bytes, id_len);
                    /* The id comes straight off the device. g_variant_new_string
                     * aborts (g_critical) on non-UTF-8 input, so validate first;
                     * a print with unset fpi-data simply won't match, which is the
                     * right outcome for a garbage record. */
                    if (g_utf8_validate (id, -1, NULL))
                      g_object_set (print, "fpi-data",
                                    g_variant_new_string (id), NULL);
                    else
                      fp_warn ("Skipping non-UTF-8 identity in stored print");
                  }
              }

              g_ptr_array_add (prints_array, print);
            }

          validity_user_clear (&user);
        }
    }

  self->list_user_idx++;

  if (self->list_user_idx < self->list_storage.user_count)
    fpi_ssm_jump_to_state (ssm, LIST_GET_USER);
  else
    fpi_ssm_next_state (ssm);
}

static void
list_run_state (FpiSsm   *ssm,
                FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LIST_GET_STORAGE:
      list_get_storage (ssm, self);
      break;

    case LIST_GET_STORAGE_RECV:
      list_get_storage_recv (ssm, self);
      break;

    case LIST_GET_USER:
      list_get_user (ssm, self);
      break;

    case LIST_GET_USER_RECV:
      list_get_user_recv (ssm, self);
      break;

    case LIST_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
list_ssm_done (FpiSsm   *ssm,
               FpDevice *dev,
               GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);
  GPtrArray *prints_array = fpi_ssm_get_data (ssm);

  validity_user_storage_clear (&self->list_storage);

  if (error)
    {
      fpi_device_list_complete (dev, NULL, validity_map_removed_error (error));
      return;
    }

  /* The ssm still owns a reference via its g_ptr_array_unref destroy notify
   * (set with fpi_ssm_set_data), so hand list_complete its own reference --
   * otherwise the array is unreffed twice and freed while the caller still
   * iterates it (use-after-free). */
  fpi_device_list_complete (dev, g_ptr_array_ref (prints_array), NULL);
}

void
validity_list (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;
  GPtrArray *prints_array;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_list (device);
      return;
    }

  prints_array = g_ptr_array_new_with_free_func (g_object_unref);

  ssm = fpi_ssm_new (device, list_run_state, LIST_NUM_STATES);
  fpi_ssm_set_data (ssm, prints_array, (GDestroyNotify) g_ptr_array_unref);
  fpi_ssm_start (ssm, list_ssm_done);
}

/* ================================================================
 * Delete print — remove a fingerprint record from the sensor DB
 * ================================================================ */

/**
 * validity_delete_find_finger:
 * @print: the print to delete
 * @user: a user record read back with cmd 0x4a
 * @out_finger_dbid: (out): the finger record to delete
 * @out_leaves_empty: (out): %TRUE if that is the only finger in @user
 *
 * Decide whether @user is the chip record that holds @print: its identity
 * must be @print's id string (see print_owns_record) and it must hold a
 * finger of @print's subtype. Another record with the same finger, which
 * may belong to another OS user or to a python-validity enrollment, is not
 * @print's and is never selected.
 *
 * python-validity scopes deletes the same way: DeleteEnrolledFingers looks
 * up the record for the caller's identity (db.lookup_user) and deletes only
 * that record.
 *
 * Returns: %TRUE if @user holds @print; the out parameters are set only then
 */
gboolean
validity_delete_find_finger (FpPrint            *print,
                             const ValidityUser *user,
                             guint16            *out_finger_dbid,
                             gboolean           *out_leaves_empty)
{
  guint16 subtype;

  if (!print || !user || !print_owns_record (print, user))
    return FALSE;

  subtype = validity_finger_to_subtype (fp_print_get_finger (print));
  if (subtype == 0)
    return FALSE;

  for (guint16 i = 0; i < user->finger_count; i++)
    {
      if (user->fingers[i].subtype == subtype)
        {
          *out_finger_dbid = user->fingers[i].dbid;
          *out_leaves_empty = (user->finger_count == 1);
          return TRUE;
        }
    }

  return FALSE;
}

/**
 * validity_delete_owner_is_empty:
 * @print: the print just deleted
 * @owner_dbid: the user record validity_delete_find_finger() selected
 * @user: that record, read back after the finger was deleted
 *
 * After the finger is gone, its user record is deleted too if it is now
 * empty. Deleting a user record also deletes every finger under it (it is
 * how python-validity's DeleteEnrolledFingers removes all of a user's
 * prints), so re-check on the fresh read that it is the same record, still
 * @print's identity, and holds no finger at all.
 *
 * Returns: %TRUE if @user may be deleted
 */
gboolean
validity_delete_owner_is_empty (FpPrint            *print,
                                guint16             owner_dbid,
                                const ValidityUser *user)
{
  if (!print || !user)
    return FALSE;

  return user->dbid == owner_dbid &&
         user->finger_count == 0 &&
         print_owns_record (print, user);
}


static void
delete_get_storage (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_get_user_storage (
    VALIDITY_STORAGE_NAME, &cmd_len);

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
delete_get_storage_recv (FpiSsm            *ssm,
                         FpiDeviceValidity *self)
{
  FpDevice *dev = FP_DEVICE (self);

  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }

  /* Parse into list_storage (shared with list SSM, not concurrent) */
  validity_user_storage_clear (&self->list_storage);
  if (!self->cmd_response_data ||
      !validity_db_parse_user_storage (self->cmd_response_data,
                                       self->cmd_response_len,
                                       &self->list_storage))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
      return;
    }

  self->delete_storage_dbid = self->list_storage.dbid;

  /* Extract finger subtype from the print to delete */
  {
    FpPrint *print = NULL;
    fpi_device_get_delete_data (dev, &print);

    FpFinger finger = fp_print_get_finger (print);
    self->delete_finger_subtype = validity_finger_to_subtype (finger);
  }

  self->list_user_idx = 0;
  fpi_ssm_next_state (ssm);
}

static void
delete_lookup_user (FpiSsm            *ssm,
                    FpiDeviceValidity *self)
{
  /* Scan every user: locate the record and finger that hold the print (by
   * identity, as python-validity's db.lookup_user(identity) does, then by
   * finger) and tally the total number of fingers on the chip, so we can
   * tell whether this delete empties it. */
  if (self->list_user_idx >= self->list_storage.user_count)
    {
      if (!self->delete_found)
        {
          /* No user record holds this print. Report that rather than
           * succeed: fprintd still removes its own copy of the print,
           * and an error keeps its storage-full clean-up (delete a device
           * print it does not know, then retry the enroll) from looping
           * on a print nothing was deleted for. If a record could not be
           * read, the print may be in it: a read failure, not not-found. */
          if (self->delete_scan_ok)
            {
              fp_info ("Delete: no user record holds this print (subtype=%u)",
                       self->delete_finger_subtype);
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND));
            }
          else
            {
              fp_warn ("Delete: print not found, and a user record could not be read");
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
            }
          return;
        }

      /* If the print being deleted is the only finger left on the chip,
       * deleting it empties the DB. Flag it so delete_ssm_done erases the
       * partition and reclaims the flash (del_record alone never compacts).
       * Skip the reclaim if any get_user failed -- an undercount could erase a
       * finger we simply could not read. */
      self->delete_reclaim = (self->delete_scan_ok &&
                              self->delete_total_fingers == 1);
      fpi_ssm_jump_to_state (ssm, DELETE_DEL_RECORD);
      return;
    }

  {
    guint16 user_dbid = self->list_storage.user_dbids[self->list_user_idx];
    gsize cmd_len;
    guint8 *cmd = validity_db_build_cmd_get_user (user_dbid, &cmd_len);
    vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
    g_free (cmd);
  }
}

static void
delete_lookup_user_recv (FpiSsm            *ssm,
                         FpiDeviceValidity *self)
{
  /* Tally this user's fingers and, until located, look for the print to delete.
   * We keep scanning every user (rather than stopping at the match) so that
   * delete_total_fingers reflects the whole chip. */
  if (self->cmd_response_status == VCSFW_STATUS_OK &&
      self->cmd_response_data)
    {
      ValidityUser user = { 0 };
      if (validity_db_parse_user (self->cmd_response_data,
                                  self->cmd_response_len,
                                  &user))
        {
          self->delete_total_fingers += user.finger_count;

          if (!self->delete_found)
            {
              FpPrint *print = NULL;

              fpi_device_get_delete_data (FP_DEVICE (self), &print);
              if (validity_delete_find_finger (print, &user,
                                               &self->delete_finger_dbid,
                                               &self->delete_owner_empties))
                {
                  self->delete_owner_dbid = user.dbid;
                  self->delete_found = TRUE;
                }
            }
          validity_user_clear (&user);
        }
      else
        {
          self->delete_scan_ok = FALSE;   /* unparseable user -- count unreliable */
        }
    }
  else
    {
      self->delete_scan_ok = FALSE;       /* get_user failed -- count unreliable */
    }

  /* Continue scanning; delete_lookup_user decides once all users are seen. */
  self->list_user_idx++;
  fpi_ssm_jump_to_state (ssm, DELETE_LOOKUP_USER);
}

static void
delete_del_record (FpiSsm            *ssm,
                   FpiDeviceValidity *self)
{
  /* Delete the finger record via cmd 0x48
   * python-validity: db.del_record(dbid) */
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_del_record (
    self->delete_finger_dbid, &cmd_len);

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
delete_del_record_recv (FpiSsm            *ssm,
                        FpiDeviceValidity *self)
{
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    {
      fp_warn ("del_record failed: status=0x%04x",
               self->cmd_response_status);
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  fp_info ("Deleted finger record: dbid=%u", self->delete_finger_dbid);
  self->delete_finger_deleted = TRUE;

  /* del_record of a finger leaves its user record behind. Remove that too
   * if the print was its only finger -- except when this delete empties the
   * chip, where the partition erase in delete_ssm_done removes it anyway and
   * the path to that erase stays as it was. */
  if (self->delete_owner_empties && !self->delete_reclaim)
    fpi_ssm_next_state (ssm);
  else
    fpi_ssm_jump_to_state (ssm, DELETE_DONE);
}

static void
delete_get_owner (FpiSsm            *ssm,
                  FpiDeviceValidity *self)
{
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_get_user (self->delete_owner_dbid,
                                                &cmd_len);

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
delete_get_owner_recv (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  ValidityUser user = { 0 };
  FpPrint *print = NULL;
  gboolean empty = FALSE;

  fpi_device_get_delete_data (FP_DEVICE (self), &print);

  if (self->cmd_response_status == VCSFW_STATUS_OK &&
      self->cmd_response_data &&
      validity_db_parse_user (self->cmd_response_data,
                              self->cmd_response_len,
                              &user))
    {
      empty = validity_delete_owner_is_empty (print, self->delete_owner_dbid,
                                              &user);
      if (!empty)
        fp_warn ("Delete: user record %u still holds %u finger(s) -- keeping it",
                 self->delete_owner_dbid, user.finger_count);
      validity_user_clear (&user);
    }
  else
    {
      fp_warn ("Delete: could not re-read user record %u (status=0x%04x) -- keeping it",
               self->delete_owner_dbid, self->cmd_response_status);
    }

  if (empty)
    fpi_ssm_next_state (ssm);
  else
    fpi_ssm_jump_to_state (ssm, DELETE_DONE);
}

static void
delete_del_owner (FpiSsm            *ssm,
                  FpiDeviceValidity *self)
{
  /* Same command python-validity's DeleteEnrolledFingers sends for a user
   * record: db.del_record(usr.dbid) */
  gsize cmd_len;
  guint8 *cmd = validity_db_build_cmd_del_record (self->delete_owner_dbid,
                                                  &cmd_len);

  vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
  g_free (cmd);
}

static void
delete_del_owner_recv (FpiSsm            *ssm,
                       FpiDeviceValidity *self)
{
  /* Best effort: the print itself is already deleted */
  if (self->cmd_response_status != VCSFW_STATUS_OK)
    fp_warn ("Delete: del_record of empty user record %u failed: status=0x%04x",
             self->delete_owner_dbid, self->cmd_response_status);
  else
    fp_info ("Deleted empty user record: dbid=%u", self->delete_owner_dbid);

  fpi_ssm_next_state (ssm);
}

static void
delete_run_state (FpiSsm   *ssm,
                  FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DELETE_GET_STORAGE:
      delete_get_storage (ssm, self);
      break;

    case DELETE_GET_STORAGE_RECV:
      delete_get_storage_recv (ssm, self);
      break;

    case DELETE_LOOKUP_USER:
      delete_lookup_user (ssm, self);
      break;

    case DELETE_LOOKUP_USER_RECV:
      delete_lookup_user_recv (ssm, self);
      break;

    case DELETE_DEL_RECORD:
      delete_del_record (ssm, self);
      break;

    case DELETE_DEL_RECORD_RECV:
      delete_del_record_recv (ssm, self);
      break;

    case DELETE_GET_OWNER:
      delete_get_owner (ssm, self);
      break;

    case DELETE_GET_OWNER_RECV:
      delete_get_owner_recv (ssm, self);
      break;

    case DELETE_DEL_OWNER:
      delete_del_owner (ssm, self);
      break;

    case DELETE_DEL_OWNER_RECV:
      delete_del_owner_recv (ssm, self);
      break;

    case DELETE_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void clear_run_state (FpiSsm   *ssm,
                             FpDevice *dev);

static void
delete_reclaim_done (FpiSsm   *ssm,
                     FpDevice *dev,
                     GError   *error)
{
  /* The delete itself already succeeded; the erase is a best-effort reclaim,
   * so a failure here must not fail the delete action. */
  if (error)
    {
      fp_warn ("Post-delete reclaim erase failed: %s", error->message);
      g_error_free (error);
    }
  else
    {
      fp_info ("Reclaimed DB partition flash after deleting the last print");
    }

  fpi_device_delete_complete (dev, NULL);
}

static void
delete_ssm_done (FpiSsm   *ssm,
                 FpDevice *dev,
                 GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  validity_user_storage_clear (&self->list_storage);

  /* Deleting the last enrolled print only removes the record; the on-chip DB
   * never compacts, so the flash stays occupied. When we just deleted the last
   * print, erase the partition to reclaim it (same erase as clear_storage) so
   * the user-facing "remove all prints" flow actually frees space. */
  if (!error && self->delete_reclaim)
    {
      FpiSsm *erase;

      self->delete_reclaim = FALSE;
      fp_info ("Deleted the last print -- erasing DB partition to reclaim flash");
      erase = fpi_ssm_new (dev, clear_run_state, CLEAR_NUM_STATES);
      fpi_ssm_start (erase, delete_reclaim_done);
      return;
    }

  self->delete_reclaim = FALSE;

  /* An error after the finger record was deleted can only come from the
   * empty-user clean-up: the print itself is gone, so report the delete. */
  if (error && self->delete_finger_deleted)
    {
      fp_warn ("Delete: print deleted, but removing its empty user record failed: %s",
               error->message);
      g_clear_error (&error);
    }

  fpi_device_delete_complete (dev, validity_map_removed_error (error));
}

void
validity_delete (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_delete (device);
      return;
    }

  self->delete_reclaim = FALSE;
  self->delete_total_fingers = 0;
  self->delete_found = FALSE;
  self->delete_scan_ok = TRUE;
  self->delete_owner_dbid = 0;
  self->delete_owner_empties = FALSE;
  self->delete_finger_deleted = FALSE;
  ssm = fpi_ssm_new (device, delete_run_state, DELETE_NUM_STATES);
  fpi_ssm_start (ssm, delete_ssm_done);
}

/* ================================================================
 * Clear storage — delete all fingerprint records from the sensor DB
 * python-validity: for user in db.get_user_storage(): db.del_record(user.dbid)
 * ================================================================ */

static void
clear_run_state (FpiSsm   *ssm,
                 FpDevice *dev)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CLEAR_DB_WRITE_ENABLE:
      {
        gsize blob_len;
        const guint8 *blob = validity_db_get_write_enable_blob (self, &blob_len);
        vcsfw_tls_cmd_send (self, ssm, blob, blob_len, NULL);
      }
      break;

    case CLEAR_DB_WRITE_ENABLE_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("clear_storage: db_write_enable failed: 0x%04x",
                   self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case CLEAR_ERASE:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_erase_flash (VALIDITY_DB_PARTITION,
                                                         &cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case CLEAR_ERASE_RECV:
      if (self->cmd_response_status != VCSFW_STATUS_OK)
        {
          fp_warn ("clear_storage: erase_flash(%d) failed: 0x%04x",
                   VALIDITY_DB_PARTITION, self->cmd_response_status);
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
          break;
        }
      fp_info ("clear_storage: erased DB partition %d (reclaimed flash)",
               VALIDITY_DB_PARTITION);
      fpi_ssm_next_state (ssm);
      break;

    case CLEAR_CLEANUPS:
      {
        gsize cmd_len;
        guint8 *cmd = validity_db_build_cmd_call_cleanups (&cmd_len);
        vcsfw_tls_cmd_send (self, ssm, cmd, cmd_len, NULL);
        g_free (cmd);
      }
      break;

    case CLEAR_CLEANUPS_RECV:
      fpi_ssm_next_state (ssm);
      break;

    case CLEAR_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
clear_ssm_done (FpiSsm   *ssm,
                FpDevice *dev,
                GError   *error)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (dev);

  validity_user_storage_clear (&self->list_storage);
  fpi_device_clear_storage_complete (dev, validity_map_removed_error (error));
}

void
validity_clear_storage (FpDevice *device)
{
  FpiDeviceValidity *self = FPI_DEVICE_VALIDITY (device);
  FpiSsm *ssm;

  G_DEBUG_HERE ();

  if (self->emulation_prints)
    {
      emulation_clear_storage (device);
      return;
    }

  ssm = fpi_ssm_new (device, clear_run_state, CLEAR_NUM_STATES);
  fpi_ssm_start (ssm, clear_ssm_done);
}
