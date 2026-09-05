/*
 * OMS Microelectronics Match-On-Chip fingerprint driver for libfprint
 * Copyright (C) 2026 Liu Sen
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

/*
 * The module is a simple slave: it never generates unsolicited data, so
 * every interaction is a strict request/response sequence.  However, the
 * physical USB layer is *wrapped*: one "logical" exchange is not one USB
 * transfer but a sequence of three (see below).
 *
 * ---------------------------------------------------------------------
 * Design overview / layers
 * ---------------------------------------------------------------------
 *
 *   oms_proto.{c,h}   Pure protocol layer.  Builds the 64 byte command
 *                     frames (0xEF 0x01 ...) and parses/validates the
 *                     response frames, plus helpers for the enrolment
 *                     bitmap.  No USB, no state machines.
 *
 *   omsmoc.c          Driver logic, split into three further layers:
 *
 *   1. Transport layer: "logical read / logical write".
 *        Every logical operation is implemented by a small, transient
 *        state machine (OmsIoState) with three physical USB steps:
 *
 *          Logical write:  OUT 8B envelope, OUT 64B command, IN "USBS".
 *          Logical read :  OUT 8B envelope, IN response,    IN "USBS".
 *
 *        The 8 byte envelope ("USBC") tells the module whether the
 *        payload that follows is a write (dir 0x00) or a read request
 *        (dir 0x80) and how long it is.  The trailing "USBS" packet is a
 *        fixed acknowledgement.
 *
 *   2. Command layer:  every module instruction is written once and its
 *        replies are then *polled* until a terminal result is seen:
 *
 *          AutoIdentify        : write, then poll until match/no-match
 *          ReadIndexTable      : write, read (single reply)
 *          GetValidTemplateNum : write, read (single reply)
 *          AutoEnrollCmd       : write, then poll the status replies
 *                                (round / finalisation) until the module
 *                                reports the template as stored
 *          DeleteChar          : write, read (single reply)
 *
 *        The number of reads is NOT fixed: the module skips intermediate
 *        statuses whenever they do not apply (e.g. AutoIdentify directly
 *        returns "no match" when the module has no templates) and it may
 *        answer a read with a "not ready" frame (all zeros) when polled
 *        faster than it can produce the next status.
 *
 *   3. Task layer:  one "task" state machine per libfprint operation
 *        (enroll / identify / list / delete / clear_storage).  States
 *        come in three kinds:
 *          - write states   : send one instruction
 *          - read states    : perform ONE logical read
 *          - decide states  : inspect the reply; jump back to the read
 *                             state for intermediate statuses, or act on
 *                             a terminal result / error.
 *
 * The nesting between the layers is intentionally flat:
 *
 *   task state machine  --(runs one step)-->  logical I/O machine
 *                                              (3 physical USB steps)
 *
 * A logical I/O machine is created per logical operation.  It simply
 * "continues" the task state machine that started it (fpi_ssm_next_state()
 * on success, fpi_ssm_mark_failed() on error) and transparently repeats a
 * logical read whose payload came back "not ready" (all zeros).  This
 * keeps the transport reusable while the task machines stay readable.
 */

/*
 * ---------------------------------------------------------------------
 * Print identity
 * ---------------------------------------------------------------------
 *
 * The module only stores templates, addressed by a 16 bit "fingerprint
 * ID".  It cannot return any user chosen description.  Therefore:
 *
 *   - The fpi-data of every FpPrint only encodes the module ID:
 *         "(q)"  (guint16)
 *     so that two prints compare equal (fp_print_equal()) iff they refer
 *     to the same slot on the module.  Username / description are plain
 *     metadata and are *not* part of the identity.
 *
 *   - The driver keeps a per-session registry mapping module ID -> print
 *     (filled on successful enroll) so that list/identify/delete can
 *     reconstruct a meaningful description when it is known.  After a
 *     reboot only the bare ID is known, which is inherent to the module.
 */

#define FP_COMPONENT "omsmoc"

#include "glib.h"
#include "drivers_api.h"

#include "omsmoc.h"
#include "oms_proto.h"

/* ------------------------------------------------------------------ *
* Constants                                                           *
* ------------------------------------------------------------------ */

/* USB transfer timeouts (ms). */
#define OMS_IO_HEADER_TIMEOUT 1000
#define OMS_IO_WRITE_TIMEOUT 1000
#define OMS_IO_READ_TIMEOUT 1000
#define OMS_IO_TRAILER_TIMEOUT 1000

/* Fixed 8 byte USB envelope sent before every logical operation:
 *   0x55 0x53 0x42 0x43 <direction> <len> 0x00 0x00
 * byte 4 = direction, byte 5 = payload length. */
#define OMS_USB_ENVELOPE_LEN 8u
#define OMS_USB_DIR_WRITE 0x00
#define OMS_USB_DIR_READ 0x80
#define OMS_USB_TRAILER_LEN 4u

/* When a logical read is answered before the module has produced its next
 * status the module returns a "not ready" reply (typically a frame full of
 * zeros).  Such replies are not protocol errors: the whole logical read is
 * simply repeated after a short delay.
 *
 * "short" reads repeat at most OMS_READ_NOT_READY_RETRIES times. */
#define OMS_READ_NOT_READY_RETRIES 600u
#define OMS_READ_NOT_READY_DELAY_MS 50u

/* Delay between every logical read/write */
#define OMS_TASK_SEND_READ_DELAY 50u

/* ------------------------------------------------------------------ *
* AutoIdentify / AutoEnroll reply classification                      *
* ------------------------------------------------------------------ */

/* Result of one AutoIdentify reply read while polling. */
typedef enum {
  OMS_AI_PROGRESS,    /* intermediate status, keep reading              */
  OMS_AI_MATCH,       /* terminal: matched, @id is valid                */
  OMS_AI_NO_MATCH,    /* terminal: valid scan, nothing matched          */
  OMS_AI_SCAN_ERROR,  /* terminal: the module rejected the scan         */
} OmsAutoIdentifyResult;

/* Result of one AutoEnrollCmd status reply read while polling. */
typedef enum {
  OMS_AE_PROGRESS,    /* intermediate status, keep reading              */
  OMS_AE_ROUND,       /* one capture round was stored (@round valid)    */
  OMS_AE_DONE,        /* terminal: template stored on the module        */
  OMS_AE_ERROR,       /* the module reported an error                   */
} OmsAutoEnrollResult;

/* Tasks */
typedef enum {
  OMS_TASK_NONE = 0,
  OMS_TASK_IDENTIFY,
  OMS_TASK_ENROLL,
  OMS_TASK_LIST,
  OMS_TASK_DELETE,
  OMS_TASK_CLEAR,
  OMS_TASK_CANCEL,
} OmsTaskType;

/* ------------------------------------------------------------------ *
* Private type                                                        *
* ------------------------------------------------------------------ */

struct _FpiDeviceOmsMoc
{
  FpDevice parent;

  /* Currently running "task" state machine (one operation at a time). */
  FpiSsm *task_ssm;

  /* Cancel*/
  gboolean cancel_requested;
  guint    task_type;                  /* current task */
  guint    last_task_type;             /* last task before cancel task */

  /* Buffers shared by the transport and the running task. */
  guint8 io_cmd[OMS_CMD_FRAME_LEN];               /* command frame to send */
  guint8 io_resp[OMS_CMD_FRAME_LEN];              /* last frame received   */
  gsize  io_resp_len;

  /* Registry: module fingerprint ID -> FpPrint (strong reference).
   * Used to remember user descriptions within a session only. */
  GHashTable *print_registry;            /* key: GINT_TO_POINTER (id + 1) */

  /* --- per-operation context, only valid while task_ssm runs --------- */
  guint16               enroll_id;       /* free module slot chosen         */
  OmsAutoIdentifyResult ai_result;       /* last AutoIdentify outcome       */
  guint16               ai_id;           /* matched module ID, if any       */
  OmsIndexTable         table;           /* last ReadIndexTable result      */
  GPtrArray            *list_result;     /* list() output                   */
  OmsIndexTable         clear_table;     /* clear_storage snapshot          */
  guint16               clear_pending_id; /* ID currently being deleted      */
  gboolean              clear_initialized; /* clear_table already parsed      */
  FpPrint              *print_to_delete; /* borrowed; delete() target       */
};

G_DEFINE_TYPE (FpiDeviceOmsMoc, fpi_device_omsmoc, FP_TYPE_DEVICE)

/* ------------------------------------------------------------------ *
* Forward declarations                                                *
* ------------------------------------------------------------------ */

static void oms_io_transfer_cb (FpiUsbTransfer *transfer,
                                FpDevice       *device,
                                gpointer        user_data,
                                GError         *error);
static void oms_task_send_cmd (FpiDeviceOmsMoc *self);
static void oms_task_read_resp (FpiDeviceOmsMoc *self);
static void oms_start_cancel (FpiDeviceOmsMoc *self);
static void oms_enroll_sm_run_state (FpiSsm   *ssm,
                                     FpDevice *device);
static void oms_enroll_ssm_done (FpiSsm   *ssm,
                                 FpDevice *device,
                                 GError   *error);
static void oms_identify_sm_run_state (FpiSsm   *ssm,
                                       FpDevice *device);
static void oms_identify_ssm_done (FpiSsm   *ssm,
                                   FpDevice *device,
                                   GError   *error);
static void oms_list_sm_run_state (FpiSsm   *ssm,
                                   FpDevice *device);
static void oms_list_ssm_done (FpiSsm   *ssm,
                               FpDevice *device,
                               GError   *error);
static void oms_delete_sm_run_state (FpiSsm   *ssm,
                                     FpDevice *device);
static void oms_delete_ssm_done (FpiSsm   *ssm,
                                 FpDevice *device,
                                 GError   *error);
static void oms_clear_sm_run_state (FpiSsm   *ssm,
                                    FpDevice *device);
static void oms_clear_ssm_done (FpiSsm   *ssm,
                                FpDevice *device,
                                GError   *error);

/* ================================================================== *
* Small helpers                                                       *
* ================================================================== */

/* Mark the currently running task machine as failed. */
static G_GNUC_PRINTF (2, 3) void
oms_task_fail (FpiDeviceOmsMoc *self,
               const gchar     *format,
               ...)
{
  va_list args;
  GError *error;

  va_start (args, format);
  error = g_error_new_valist (FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                              format, args);
  va_end (args);

  fpi_ssm_mark_failed (self->task_ssm, error);
}

/* ------------------------------------------------------------------ *
* FpPrint helpers / module ID registry                                *
* ------------------------------------------------------------------ */

static void
oms_print_set_id (FpPrint *print, guint16 id)
{
  GVariant *data;

  g_return_if_fail (print);

  data = g_variant_new ("(q)", id);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
}

static gboolean
oms_print_get_id (FpPrint *print, guint16 *id)
{
  g_autoptr(GVariant) data = NULL;

  g_return_val_if_fail (print, FALSE);
  g_return_val_if_fail (id, FALSE);

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(q)")))
    return FALSE;

  g_variant_get (data, "(q)", id);
  return TRUE;
}

/* Create a new FpPrint (floating) describing the given module ID.  If the
 * module ID is known from this session (e.g. a successful enroll) the
 * remembered description is reused. */
static FpPrint *
oms_print_new (FpiDeviceOmsMoc *self, guint16 id)
{
  FpPrint *print;
  FpPrint *known;

  print = fp_print_new (FP_DEVICE (self));
  oms_print_set_id (print, id);

  known = g_hash_table_lookup (self->print_registry, GINT_TO_POINTER (id + 1));
  if (known && fp_print_get_description (known))
    fp_print_set_description (print, fp_print_get_description (known));

  return print;
}

/* Registry: remember the description of a successfully enrolled print. */
static void
oms_print_register (FpiDeviceOmsMoc *self, guint16 id, FpPrint *print)
{
  g_hash_table_replace (self->print_registry,
                        GINT_TO_POINTER (id + 1),
                        g_object_ref (print));
}

static void
oms_print_unregister (FpiDeviceOmsMoc *self, guint16 id)
{
  g_hash_table_remove (self->print_registry, GINT_TO_POINTER (id + 1));
}

/* ================================================================== *
* Transport layer                                                     *
* ------------------------------------------------------------------ *
* One "logical I/O" is implemented by a transient 3-state state machine
* (see the OmsIoState documentation).  It is started from a task state
* machine; on completion the task machine is continued.  The raw data
* received by a logical read is stored in self->io_resp so that the next
* task state can interpret it.
* ================================================================== */

typedef struct
{
  gboolean is_write;        /* logical write vs logical read */
  gboolean not_ready;       /* last payload was a "not ready" reply */
  guint    not_ready_left;  /* remaining "not ready" read retries      */
} OmsIoData;

/* Re-run the whole logical read.  Used when the module answered a read
 * request with a "not ready" reply because it had not yet produced the
 * next status. */
static void
oms_io_read_retry (FpiDeviceOmsMoc *self,
                   FpiSsm          *ssm,
                   OmsIoData       *data)
{
  if (self->cancel_requested && self->task_type != OMS_TASK_CANCEL)
    {
      /* stop IO SSM（failed）, stop task_ssm（failed）*/
      fp_dbg ("start cancel before read retry");
      fpi_ssm_mark_failed (ssm,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "Operation was cancelled"));
      return;
    }

  if (data->not_ready_left == 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Module did not return a valid response"));
      return;
    }
  data->not_ready_left--;

  fp_dbg ("Logical read not ready, retrying");
  fpi_ssm_jump_to_state_delayed (ssm, OMS_IO_ENVELOPE,
                                 OMS_READ_NOT_READY_DELAY_MS);
}

/* Submit one physical bulk transfer belonging to the logical I/O machine
 * @io_ssm.  The transfer callback resumes @io_ssm. */
static void
oms_io_submit (FpiDeviceOmsMoc *self,
               FpiSsm          *io_ssm,
               guint8           endpoint,
               gsize            length,
               guint            timeout)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->ssm = io_ssm;
  fpi_usb_transfer_fill_bulk (transfer, endpoint, length);
  fpi_usb_transfer_submit (transfer,
                           timeout,
                           NULL,
                           oms_io_transfer_cb,
                           fpi_ssm_get_data (io_ssm));
}

static void
oms_io_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  OmsIoData *data = fpi_ssm_get_data (ssm);
  guint8 envelope[OMS_USB_ENVELOPE_LEN] = { 0x55, 0x53, 0x42, 0x43, 0, 0, 0, 0 };
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_IO_ENVELOPE:
      /* Step 1: announce the logical operation to the module. */
      envelope[4] = data->is_write ? OMS_USB_DIR_WRITE : OMS_USB_DIR_READ;
      /* Commands are always written as fixed 64-byte frames.
       * Response length varies by command, but this driver always reads
       * OMS_CMD_FRAME_LEN (64) bytes. */
      envelope[5] = OMS_CMD_FRAME_LEN;

      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, OMSMOC_EP_OUT,
                                  OMS_USB_ENVELOPE_LEN);
      memcpy (transfer->buffer, envelope, OMS_USB_ENVELOPE_LEN);
      fpi_usb_transfer_submit (transfer,
                               OMS_IO_HEADER_TIMEOUT,
                               NULL,
                               oms_io_transfer_cb,
                               fpi_ssm_get_data (ssm));
      break;

    case OMS_IO_PAYLOAD:
      /* Step 2: write the command frame, or read the response. */
      if (data->is_write)
        {
          transfer = fpi_usb_transfer_new (device);
          transfer->ssm = ssm;
          fpi_usb_transfer_fill_bulk (transfer, OMSMOC_EP_OUT,
                                      OMS_CMD_FRAME_LEN);
          memcpy (transfer->buffer, self->io_cmd, OMS_CMD_FRAME_LEN);
          fpi_usb_transfer_submit (transfer,
                                   OMS_IO_WRITE_TIMEOUT,
                                   NULL,
                                   oms_io_transfer_cb,
                                   fpi_ssm_get_data (ssm));
        }
      else
        {
          oms_io_submit (self, ssm, OMSMOC_EP_IN, OMS_CMD_FRAME_LEN,
                         OMS_IO_READ_TIMEOUT);
        }
      break;

    case OMS_IO_TRAILER:
      /* Step 3: read the fixed acknowledgement packet. */
      oms_io_submit (self, ssm, OMSMOC_EP_IN, OMS_USB_TRAILER_LEN,
                     OMS_IO_TRAILER_TIMEOUT);
      break;
    }
}

static void
oms_io_transfer_cb (FpiUsbTransfer *transfer,
                    FpDevice       *device,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  FpiSsm *ssm = transfer->ssm;
  OmsIoData *data = fpi_ssm_get_data (ssm);
  int state;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  state = fpi_ssm_get_cur_state (ssm);

  switch (state)
    {
    case OMS_IO_ENVELOPE:
      fpi_ssm_next_state (ssm);
      break;

    case OMS_IO_PAYLOAD:
      if (data->is_write)
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          static const guint8 frame_prefix[OMS_FRAME_PREFIX_LEN] = OMS_FRAME_PREFIX;

          /* The module answered but did not (yet) produce a real response
           * frame (zero length, a zero filled packet, ...).  Mark the read
           * as "not ready" and still consume the trailing "USBS" packet so
           * that the stream stays in sync before we retry the whole read. */
          data->not_ready =
            (memcmp (transfer->buffer, frame_prefix, OMS_FRAME_PREFIX_LEN) != 0);

          if (!data->not_ready)
            {
              self->io_resp_len = MIN ((gsize) transfer->actual_length,
                                       sizeof (self->io_resp));
              memcpy (self->io_resp, transfer->buffer, self->io_resp_len);
            }
          fpi_ssm_next_state (ssm);
        }
      break;

    case OMS_IO_TRAILER:
      if (!data->is_write && data->not_ready)
        {
          /* The payload was "not ready": repeat the whole read. */
          data->not_ready = FALSE;
          oms_io_read_retry (self, ssm, data);
          return;
        }

      fpi_ssm_mark_completed (ssm);

      break;
    }
}

static void
oms_io_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  /* Continue (or fail) the task state machine that started this I/O. */
  if (error)
    {
      if (self->task_ssm)
        fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&error));
      else
        g_clear_error (&error);
    }
  else if (self->task_ssm)
    {
      fpi_ssm_next_state_delayed (self->task_ssm, OMS_TASK_SEND_READ_DELAY);
    }
}

/* Start a logical write of the pre-built self->io_cmd frame. */
static void
oms_task_send_cmd (FpiDeviceOmsMoc *self)
{
  if (self->cancel_requested && self->task_type != OMS_TASK_CANCEL)
    {
      fp_dbg ("start cancel before send cmd");
      oms_start_cancel (self);
      return;
    }

  OmsIoData *data = g_new0 (OmsIoData, 1);
  FpiSsm *io_ssm;

  data->is_write = TRUE;

  io_ssm = fpi_ssm_new (FP_DEVICE (self), oms_io_run_state, OMS_IO_NUM_STATES);
  fpi_ssm_set_data (io_ssm, data, g_free);
  fpi_ssm_start (io_ssm, oms_io_ssm_done);
}

/* Start a logical read. */
static void
oms_task_read_resp (FpiDeviceOmsMoc *self)
{
  if (self->cancel_requested && self->task_type != OMS_TASK_CANCEL)
    {
      fp_dbg ("start cancel before read resp");
      oms_start_cancel (self);
      return;
    }

  OmsIoData *data = g_new0 (OmsIoData, 1);
  FpiSsm *io_ssm;

  data->is_write = FALSE;
  data->not_ready_left = OMS_READ_NOT_READY_RETRIES;

  io_ssm = fpi_ssm_new (FP_DEVICE (self), oms_io_run_state, OMS_IO_NUM_STATES);
  fpi_ssm_set_data (io_ssm, data, g_free);
  fpi_ssm_start (io_ssm, oms_io_ssm_done);
}

/* Build the 64 byte command frame in self->io_cmd from its data part. */
static gboolean
oms_prepare_command (FpiDeviceOmsMoc *self,
                     const guint8    *data,
                     guint8           data_len)
{
  return oms_proto_build_command (data, data_len, self->io_cmd);
}

/* ================================================================== *
* Task: cancel                                                       *
* ================================================================== */
static void
oms_cancel_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  static const guint8 cancel_cmd_data[] = { OMS_CMD_CANCEL }; /* 0x30 */

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_CANCEL_SEND:
      if (!oms_prepare_command (self, cancel_cmd_data,
                                sizeof (cancel_cmd_data)))
        {
          /* Failed to build CANCEL command, completed the IO ssm directly */
          fpi_ssm_mark_completed (ssm);
          break;
        }
      oms_task_send_cmd (self);
      break;

    case OMS_CANCEL_READ:
      oms_task_read_resp (self);
      break;
    }
}

static void
oms_cancel_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  GError *cancel_error = NULL;

  /* Report cancellation regardless of whether sending the cancel command succeeds */
  cancel_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                      "Operation was cancelled");

  /* Cancel handling is normally required only by identify and enroll
   * However, to unify the handling logic, cancel handling is also added
   * to the other tasks. */
  switch (self->last_task_type)
    {
    case OMS_TASK_IDENTIFY:
      fpi_device_identify_complete (device, g_steal_pointer (&cancel_error));
      break;

    case OMS_TASK_ENROLL:
      fpi_device_enroll_complete (device, NULL, g_steal_pointer (&cancel_error));
      break;

    case OMS_TASK_LIST:
      fpi_device_list_complete (device, NULL, g_steal_pointer (&cancel_error));
      break;

    case OMS_TASK_DELETE:
      fpi_device_delete_complete (device, g_steal_pointer (&cancel_error));
      break;

    case OMS_TASK_CLEAR:
      fpi_device_clear_storage_complete (device, g_steal_pointer (&cancel_error));
      break;

    default:
      g_clear_error (&cancel_error);
      break;
    }

  self->last_task_type = OMS_TASK_CANCEL;
  self->task_type = OMS_TASK_NONE;
  /* task ssm for cancel task */
  self->task_ssm = NULL;
}

/*
 * When libfprint cancels an operation, it calls oms_fp_cancel, which sets
 * self->cancel_request to TRUE.
 *
 * Then, when to start the cancel task state machine:
 *   1. Before performing logical reads/writes
 *   2. Before oms_xxx_ssm_done
 *
 * The first timing does not disrupt logical reads/writes and is sufficient to
 * terminate most task state machines.
 * Some tasks may not perform logical reads/writes in their last few states,
 * so the second timing is added as a supplement.
 *
 * With the first timing, task_ssm is neither completed nor failed, which may
 * introduce potential bugs.
 * Finish task_ssm first, and then handle it uniformly in oms_xxx_ssm_done.
 */
static void
oms_start_cancel (FpiDeviceOmsMoc *self)
{
  if (!self->task_ssm)
    return;
  fpi_ssm_mark_failed (self->task_ssm,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                            "Operation was cancelled"));
}

/* ------------------------------------------------------------------ *
* AutoIdentify / AutoEnroll reply classification                      *
* ------------------------------------------------------------------ */

/* Classify the AutoIdentify reply stored by the last logical read.
 *
 * The module does NOT send a fixed number of replies: when there are no
 * stored templates it answers "no match" immediately (no image capture),
 * otherwise it emits one or more intermediate statuses (0x00 command
 * accepted, 0x01 image captured, ...) before the terminal result. */
static OmsAutoIdentifyResult
oms_classify_auto_identify (FpiDeviceOmsMoc *self,
                            guint16         *id,
                            gboolean        *image_captured)
{
  OmsProtoFrame frame;

  *id = 0;
  if (image_captured)
    *image_captured = FALSE;

  if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame) ||
      frame.data_len < 6)
    /* Not a valid reply yet; treat it like "not ready" and read again. */
    return OMS_AI_PROGRESS;

  switch (frame.data[0])
    {
    case OMS_ACK_NO_MATCH:                     /* 0x09: nothing matched */
      return OMS_AI_NO_MATCH;

    case OMS_ACK_OK:                           /* 0x00 */
      /* data[1] == 0x05 is the terminal "match attempt finished" status;
       * the matched module ID is stored in data[2..3] (big endian). */
      if (frame.data[1] == 0x05)
        {
          *id = (frame.data[2] << 8) | frame.data[3];
          return OMS_AI_MATCH;
        }
      /* data[1] == 0x01 means an image was captured. */
      if (frame.data[1] == 0x01 && image_captured)
        *image_captured = TRUE;
      return OMS_AI_PROGRESS;

    default:
      /* Unknown confirmation code: the module rejected the scan. */
      fp_warn ("AutoIdentify: unhandled confirmation code 0x%02x",
               frame.data[0]);
      return OMS_AI_SCAN_ERROR;
    }
}

/* Classify the AutoEnrollCmd status reply stored by the last logical
 * read.  data[1] selects the step, data[2] carries the round number (or
 * 0xf0/0xf1/0xf2 for the finalisation steps). */
static OmsAutoEnrollResult
oms_classify_auto_enroll (FpiDeviceOmsMoc *self,
                          guint           *round,
                          gboolean        *image_captured)
{
  OmsProtoFrame frame;

  *round = 0;
  if (image_captured)
    *image_captured = FALSE;

  if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame) ||
      frame.data_len < 3)
    return OMS_AE_PROGRESS;

  if (frame.data[0] != OMS_ACK_OK)
    {
      fp_warn ("AutoEnrollCmd: module reported error code 0x%02x (step 0x%02x)",
               frame.data[0], frame.data[1]);
      return OMS_AE_ERROR;
    }

  switch (frame.data[1])
    {
    case 0x00:                                 /* command accepted        */
    case 0x02:                                 /* feature generated       */
    case 0x04:                                 /* template merge          */
    case 0x05:                                 /* registered check        */
      return OMS_AE_PROGRESS;

    case 0x01:                                 /* image captured */
      if (image_captured)
        *image_captured = TRUE;
      return OMS_AE_PROGRESS;

    case 0x03:                                 /* round stored */
      *round = frame.data[2];
      return OMS_AE_ROUND;

    case 0x06:                                 /* store result */
      return OMS_AE_DONE;

    default:
      fp_warn ("AutoEnrollCmd: unhandled step code 0x%02x",
               frame.data[1]);
      return OMS_AE_ERROR;
    }
}

/* ================================================================== *
* Task: identify                                                      *
* ------------------------------------------------------------------ *
* One invocation corresponds to one AutoIdentify attempt.  Because the
* number of intermediate replies is not fixed, the machine only has a
* write state followed by a poll state:
*
*   OMS_IDENTIFY_SEND   write the AutoIdentify command
*   OMS_IDENTIFY_READ   one logical read (repeated on intermediate
*                       statuses by jumping back from OMS_IDENTIFY_DECIDE)
*   OMS_IDENTIFY_DECIDE classify; on a terminal result the machine is
*                       marked completed and oms_identify_ssm_done()
*                       reports the outcome.
* ================================================================== */

static void
oms_identify_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  static const guint8 cmd_auto_identify[] = { OMS_CMD_AUTO_IDENTIFY,
                                              0x05, 0xFF, 0xFF, 0x00, 0x00 };
  OmsAutoIdentifyResult result;
  guint16 id = 0;
  gboolean image = FALSE;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_IDENTIFY_SEND:
      self->ai_result = OMS_AI_PROGRESS;
      if (!oms_prepare_command (self, cmd_auto_identify,
                                sizeof (cmd_auto_identify)))
        oms_task_fail (self, "Failed to build AutoIdentify command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_IDENTIFY_READ:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      oms_task_read_resp (self);
      break;

    case OMS_IDENTIFY_DECIDE:
      result = oms_classify_auto_identify (self, &id, &image);

      if (result == OMS_AI_PROGRESS)
        {
          if (image)
            fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_IDENTIFY_READ,
                                         OMS_READ_NOT_READY_DELAY_MS);
          break;
        }

      self->ai_result = result;
      self->ai_id = id;
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
oms_identify_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  FpPrint *match = NULL;
  FpPrint *scan = NULL;
  GPtrArray *gallery = NULL;
  guint16 id;

  self->task_ssm = NULL;
  self->last_task_type = self->task_type;
  self->task_type = OMS_TASK_NONE;

  /* Newly added cancel handling logic */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->task_type = OMS_TASK_CANCEL;
      /* Clear the flag to avoid infinite recursion */
      self->cancel_requested = FALSE;
      FpiSsm *cancel_ssm;

      cancel_ssm = fpi_ssm_new (FP_DEVICE (self), oms_cancel_sm_run_state,
                                OMS_CANCEL_NUM_STATES);
      /* Very important; otherwise the IO SSM will not advance
       * the state machine for the Cancel command at all */
      self->task_ssm = cancel_ssm;
      fpi_ssm_start (cancel_ssm, oms_cancel_ssm_done);
      return;
    }

  if (error)
    {
      fpi_device_identify_complete (device, g_steal_pointer (&error));
      return;
    }

  id = self->ai_id;

  switch (self->ai_result)
    {
    case OMS_AI_NO_MATCH:
      /* A valid scan was performed but nothing matched. */
      fpi_device_identify_report (device, NULL, NULL, NULL);
      fpi_device_identify_complete (device, NULL);
      return;

    case OMS_AI_SCAN_ERROR:
      /* The module rejected the scan; ask the user to try again. */
      fpi_device_identify_report (device, NULL, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      fpi_device_identify_complete (device, NULL);
      return;

    case OMS_AI_MATCH:
      break;

    case OMS_AI_PROGRESS:
    default:
      g_assert_not_reached ();
    }

  fp_info ("Identify matched module fingerprint ID 0x%04x", id);

  scan = oms_print_new (self, id);

  /* Find the matching print inside the caller supplied gallery. */
  fpi_device_get_identify_data (device, &gallery);
  if (gallery)
    {
      for (guint i = 0; i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          guint16 print_id = 0;

          if (oms_print_get_id (print, &print_id) && print_id == id)
            {
              match = print;
              break;
            }
        }
    }

  fpi_device_identify_report (device, match, scan, NULL);
  fpi_device_identify_complete (device, NULL);
}

/* ================================================================== *
* Task: enroll                                                        *
* ------------------------------------------------------------------ *
* Enrolling a new fingerprint performs, in order:
*   1. AutoIdentify (polled)  - duplicate check; must report no-match
*   2. ReadIndexTable x2       - obtain a stable enrolment bitmap
*   3. AutoEnrollCmd (polled)  - the module captures OMS_AUTO_ENROLL_ROUNDS
*                                images, builds and stores the template.
*                                Its status replies are polled until the
*                                "store result" status is seen.
*   4. GetValidTemplateNum + ReadIndexTable - verify the result.
* ================================================================== */

static void
oms_enroll_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  OmsProtoFrame frame;
  OmsAutoIdentifyResult ai_result;
  OmsAutoEnrollResult ae_result;
  guint8 cmd[6];
  guint16 id = 0;
  guint round = 0;
  gboolean image = FALSE;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_ENROLL_DUP_SEND:
      {
        static const guint8 dup_cmd[] = { OMS_CMD_AUTO_IDENTIFY,
                                          0x05, 0xFF, 0xFF, 0x00, 0x00 };
        if (!oms_prepare_command (self, dup_cmd, sizeof (dup_cmd)))
          oms_task_fail (self, "Failed to build AutoIdentify command");
        else
          oms_task_send_cmd (self);
      }
      break;

    case OMS_ENROLL_DUP_READ:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      oms_task_read_resp (self);
      break;

    case OMS_ENROLL_DUP_DECIDE:
      ai_result = oms_classify_auto_identify (self, &id, &image);

      switch (ai_result)
        {
        case OMS_AI_PROGRESS:
          if (image)
            fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_DUP_READ,
                                         OMS_READ_NOT_READY_DELAY_MS);
          break;

        case OMS_AI_NO_MATCH:
          /* Not a duplicate: move on to the enrolment bitmap. */
          fpi_ssm_next_state (ssm);
          break;

        case OMS_AI_MATCH:
          {
            FpPrint *known = g_hash_table_lookup (self->print_registry,
                                                  GINT_TO_POINTER (id + 1));
            const gchar *desc = known ? fp_print_get_description (known) : NULL;

            fp_info ("Duplicate detected, module ID 0x%04x", id);
            fpi_ssm_mark_failed (self->task_ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                                           "Finger was already enrolled as '%s'",
                                                           desc ? desc : "unknown fingerprint"));
          }
          break;

        case OMS_AI_SCAN_ERROR:
          /* The scan was rejected; simply restart the duplicate check. */
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_DUP_SEND,
                                         OMS_READ_NOT_READY_DELAY_MS);
          break;
        }
      break;

    case OMS_ENROLL_TABLE1_SEND:
    case OMS_ENROLL_TABLE2_SEND:
      if (!oms_prepare_command (self,
                                (guint8[]){ OMS_CMD_READ_INDEX_TABLE, 0x00 },
                                2))
        oms_task_fail (self, "Failed to build ReadIndexTable command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_ENROLL_TABLE1_RECV:
    case OMS_ENROLL_TABLE2_RECV:
      oms_task_read_resp (self);
      break;

    case OMS_ENROLL_ENROLL_SEND:
      /* Snapshot the enrolment bitmap (result of the second table read)
       * and pick a free module ID for the new fingerprint. */
      if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame))
        {
          oms_task_fail (self, "Invalid ReadIndexTable response");
          break;
        }

      if (frame.data_len < 1 + OMS_INDEX_TABLE_BYTES)
        {
          oms_task_fail (self, "Invalid ReadIndexTable response");
          break;
        }

      memset (&self->table, 0, sizeof (self->table));
      memcpy (self->table.bytes, frame.data + 1, OMS_INDEX_TABLE_BYTES);

      if (!oms_index_table_find_free (&self->table, &self->enroll_id))
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_FULL));
          break;
        }

      fp_info ("Enrolling as module ID 0x%04x (%u stored)",
               self->enroll_id, oms_index_table_count (&self->table));

      cmd[0] = OMS_CMD_AUTO_ENROLL;
      cmd[1] = (self->enroll_id >> 8) & 0xFF;
      cmd[2] = self->enroll_id & 0xFF;
      cmd[3] = OMS_AUTO_ENROLL_ROUNDS;
      cmd[4] = 0x00;
      cmd[5] = 0x00;
      if (!oms_prepare_command (self, cmd, sizeof (cmd)))
        oms_task_fail (self, "Failed to build AutoEnrollCmd command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_ENROLL_ENROLL_READ:
      oms_task_read_resp (self);
      break;

    case OMS_ENROLL_ENROLL_DECIDE:
      ae_result = oms_classify_auto_enroll (self, &round, &image);

      switch (ae_result)
        {
        case OMS_AE_PROGRESS:
          if (image)
            fpi_device_report_finger_status (device, FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_ENROLL_READ,
                                         OMS_READ_NOT_READY_DELAY_MS);
          break;

        case OMS_AE_ROUND:
          /* A capture round was stored; report progress.  For all but the
           * last round the module will wait for the next finger press. */
          if (round < OMS_AUTO_ENROLL_ROUNDS)
            fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
          fpi_device_enroll_progress (device, round, NULL, NULL);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_ENROLL_READ,
                                         OMS_READ_NOT_READY_DELAY_MS);
          break;

        case OMS_AE_DONE:
          /* Template stored; verify and finish. */
          fpi_ssm_next_state (ssm);
          break;

        case OMS_AE_ERROR:
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "Module reported an error during enrollment"));
          break;
        }
      break;

    case OMS_ENROLL_VERIFY_NUM_SEND:
      if (!oms_prepare_command (self,
                                (guint8[]){ OMS_CMD_GET_VALID_TEMPLATE_NUM },
                                1))
        oms_task_fail (self, "Failed to build GetValidTemplateNum command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_ENROLL_VERIFY_NUM_RECV:
      oms_task_read_resp (self);
      break;

    case OMS_ENROLL_VERIFY_TABLE_SEND:
      if (!oms_prepare_command (self,
                                (guint8[]){ OMS_CMD_READ_INDEX_TABLE, 0x00 },
                                2))
        oms_task_fail (self, "Failed to build ReadIndexTable command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_ENROLL_VERIFY_TABLE_RECV:
      oms_task_read_resp (self);
      break;
    }
}

static void
oms_enroll_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  FpPrint *print = NULL;
  OmsProtoFrame frame;
  guint16 count = 0;
  gboolean id_present = FALSE;

  self->task_ssm = NULL;
  self->last_task_type = self->task_type;
  self->task_type = OMS_TASK_NONE;

  /* Newly added cancel handling logic */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->task_type = OMS_TASK_CANCEL;
      /* Clear the flag to avoid infinite recursion */
      self->cancel_requested = FALSE;
      FpiSsm *cancel_ssm;

      cancel_ssm = fpi_ssm_new (FP_DEVICE (self), oms_cancel_sm_run_state,
                                OMS_CANCEL_NUM_STATES);
      /* Very important; otherwise the IO SSM will not advance
       * the state machine for the Cancel command at all */
      self->task_ssm = cancel_ssm;
      fpi_ssm_start (cancel_ssm, oms_cancel_ssm_done);
      return;
    }

  if (error)
    {
      fpi_device_enroll_complete (device, NULL, g_steal_pointer (&error));
      return;
    }

  /* Verify that the newly enrolled template is present on the module. */
  if (oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame))
    {
      if (frame.data_len >= 3)
        count = (frame.data[1] << 8) | frame.data[2];
      if (frame.data_len >= 1 + OMS_INDEX_TABLE_BYTES)
        {
          OmsIndexTable table;

          memset (&table, 0, sizeof (table));
          memcpy (table.bytes, frame.data + 1, OMS_INDEX_TABLE_BYTES);
          id_present = oms_index_table_get (&table, self->enroll_id);
        }
    }

  if (!id_present)
    {
      fp_warn ("Enrollment verification failed, template not found on module");
      fpi_device_enroll_complete (device, NULL,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                            "Enrollment could not be verified on the module"));
      return;
    }

  fpi_device_get_enroll_data (device, &print);
  if (!print)
    {
      fpi_device_enroll_complete (device, NULL,
                                  fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  {
    g_autofree gchar *user_id = NULL;

    oms_print_set_id (print, self->enroll_id);
    user_id = fpi_print_generate_user_id (print);
    fp_print_set_description (print, user_id);

    oms_print_register (self, self->enroll_id, print);

    fp_info ("Enrollment complete, module ID 0x%04x (total %u)",
             self->enroll_id, count);
    fpi_device_enroll_complete (device, g_object_ref (print), NULL);
  }
}

/* ================================================================== *
* Task: list                                                          *
* ================================================================== */

static void
oms_list_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_LIST_SEND:
      if (!oms_prepare_command (self,
                                (guint8[]){ OMS_CMD_READ_INDEX_TABLE, 0x00 },
                                2))
        oms_task_fail (self, "Failed to build ReadIndexTable command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_LIST_RECV:
      oms_task_read_resp (self);
      break;
    }
}

static void
oms_list_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  OmsProtoFrame frame;

  self->task_ssm = NULL;
  self->last_task_type = self->task_type;
  self->task_type = OMS_TASK_NONE;

  /* Newly added cancel handling logic */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->task_type = OMS_TASK_CANCEL;
      /* Clear the flag to avoid infinite recursion */
      self->cancel_requested = FALSE;
      FpiSsm *cancel_ssm;

      cancel_ssm = fpi_ssm_new (FP_DEVICE (self), oms_cancel_sm_run_state,
                                OMS_CANCEL_NUM_STATES);
      /* Very important; otherwise the IO SSM will not advance
       * the state machine for the Cancel command at all */
      self->task_ssm = cancel_ssm;
      fpi_ssm_start (cancel_ssm, oms_cancel_ssm_done);
      return;
    }

  if (error)
    {
      fpi_device_list_complete (device, NULL, g_steal_pointer (&error));
      return;
    }

  if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame))
    {
      fpi_device_list_complete (device, NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Invalid ReadIndexTable response"));
      return;
    }

  self->list_result = g_ptr_array_new_with_free_func (g_object_unref);

  if (frame.data_len >= 1 + OMS_INDEX_TABLE_BYTES)
    {
      OmsIndexTable table;

      memset (&table, 0, sizeof (table));
      memcpy (table.bytes, frame.data + 1, OMS_INDEX_TABLE_BYTES);

      for (guint id = 0; id < OMS_INDEX_TABLE_BITS; id++)
        {
          if (oms_index_table_get (&table, id))
            {
              FpPrint *print = oms_print_new (self, id);

              g_ptr_array_add (self->list_result, g_object_ref_sink (print));
            }
        }
    }

  fp_info ("List complete, %u fingerprint(s) stored", self->list_result->len);
  fpi_device_list_complete (device, g_steal_pointer (&self->list_result), NULL);
}

/* ================================================================== *
* Task: delete                                                        *
* ================================================================== */

static void
oms_delete_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  guint8 cmd[5];
  guint16 id = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_DELETE_SEND:
      /* The delete-data print carries the module ID in its fpi-data. */
      if (!oms_print_get_id (self->print_to_delete, &id))
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          break;
        }

      cmd[0] = OMS_CMD_DELETE_CHAR;
      cmd[1] = (id >> 8) & 0xFF;
      cmd[2] = id & 0xFF;
      cmd[3] = 0x00;
      cmd[4] = 0x01;

      if (!oms_prepare_command (self, cmd, sizeof (cmd)))
        oms_task_fail (self, "Failed to build DeleteChar command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_DELETE_RECV:
      oms_task_read_resp (self);
      break;
    }
}

static void
oms_delete_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  OmsProtoFrame frame;
  guint16 id = 0;

  self->task_ssm = NULL;
  self->last_task_type = self->task_type;
  self->task_type = OMS_TASK_NONE;

  /* Newly added cancel handling logic */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->task_type = OMS_TASK_CANCEL;
      /* Clear the flag to avoid infinite recursion */
      self->cancel_requested = FALSE;
      FpiSsm *cancel_ssm;

      cancel_ssm = fpi_ssm_new (FP_DEVICE (self), oms_cancel_sm_run_state,
                                OMS_CANCEL_NUM_STATES);
      /* Very important; otherwise the IO SSM will not advance
       * the state machine for the Cancel command at all */
      self->task_ssm = cancel_ssm;
      fpi_ssm_start (cancel_ssm, oms_cancel_ssm_done);
      return;
    }

  if (error)
    {
      fpi_device_delete_complete (device, g_steal_pointer (&error));
      return;
    }

  if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame) ||
      frame.data_len < 1 || frame.data[0] != OMS_ACK_OK)
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                            "Failed to delete fingerprint from module"));
      return;
    }

  if (oms_print_get_id (self->print_to_delete, &id))
    {
      oms_print_unregister (self, id);
      fp_info ("Successfully deleted module fingerprint ID 0x%04x", id);
    }

  fpi_device_delete_complete (device, NULL);
}

/* ================================================================== *
* Task: clear storage                                                 *
* ================================================================== */

/* Helper: find the next still-stored ID in the clear_storage snapshot. */
static gboolean
oms_clear_find_next (FpiDeviceOmsMoc *self,
                     guint16         *id)
{
  for (guint i = 0; i < OMS_INDEX_TABLE_BITS; i++)
    {
      if (oms_index_table_get (&self->clear_table, i))
        {
          *id = i;
          return TRUE;
        }
    }
  return FALSE;
}

static void
oms_clear_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  guint8 cmd[5];
  guint16 id = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_CLEAR_QUERY_SEND:
      if (!oms_prepare_command (self,
                                (guint8[]){ OMS_CMD_READ_INDEX_TABLE, 0x00 },
                                2))
        oms_task_fail (self, "Failed to build ReadIndexTable command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_CLEAR_QUERY_RECV:
      oms_task_read_resp (self);
      break;

    case OMS_CLEAR_DELETE_SEND:
      /* On first entry parse the query result into the snapshot bitmap;
       * on later entries (after fpi_ssm_jump_to_state() from
       * OMS_CLEAR_DELETE_NEXT) the snapshot is already valid. */
      if (!self->clear_initialized)
        {
          OmsProtoFrame frame;

          if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame))
            {
              oms_task_fail (self, "Invalid ReadIndexTable response");
              break;
            }

          if (frame.data_len < 1 + OMS_INDEX_TABLE_BYTES)
            {
              oms_task_fail (self, "Invalid ReadIndexTable response");
              break;
            }

          memset (&self->clear_table, 0, sizeof (self->clear_table));
          memcpy (self->clear_table.bytes, frame.data + 1,
                  OMS_INDEX_TABLE_BYTES);
          self->clear_initialized = TRUE;
        }

      if (!oms_clear_find_next (self, &id))
        {
          /* Nothing stored. */
          fpi_ssm_mark_completed (ssm);
          break;
        }

      self->clear_pending_id = id;
      cmd[0] = OMS_CMD_DELETE_CHAR;
      cmd[1] = (id >> 8) & 0xFF;
      cmd[2] = id & 0xFF;
      cmd[3] = 0x00;
      cmd[4] = 0x01;
      if (!oms_prepare_command (self, cmd, sizeof (cmd)))
        oms_task_fail (self, "Failed to build DeleteChar command");
      else
        oms_task_send_cmd (self);
      break;

    case OMS_CLEAR_DELETE_RECV:
      oms_task_read_resp (self);
      break;

    case OMS_CLEAR_DELETE_NEXT:
      {
        OmsProtoFrame frame;

        if (!oms_proto_parse_frame (self->io_resp, self->io_resp_len, &frame) ||
            frame.data_len < 1 || frame.data[0] != OMS_ACK_OK)
          {
            oms_task_fail (self, "Failed to delete fingerprint from module");
            break;
          }

        oms_index_table_set (&self->clear_table, self->clear_pending_id, FALSE);

        if (oms_clear_find_next (self, &id))
          {
            self->clear_pending_id = id;
            cmd[0] = OMS_CMD_DELETE_CHAR;
            cmd[1] = (id >> 8) & 0xFF;
            cmd[2] = id & 0xFF;
            cmd[3] = 0x00;
            cmd[4] = 0x01;
            if (!oms_prepare_command (self, cmd, sizeof (cmd)))
              oms_task_fail (self, "Failed to build DeleteChar command");
            else
              fpi_ssm_jump_to_state_delayed (ssm, OMS_CLEAR_DELETE_SEND,
                                             OMS_READ_NOT_READY_DELAY_MS);
          }
        else
          {
            /* All stored prints deleted. */
            fpi_ssm_mark_completed (ssm);
          }
      }
      break;
    }
}

static void
oms_clear_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_ssm = NULL;
  self->last_task_type = self->task_type;
  self->task_type = OMS_TASK_NONE;

  /* Newly added cancel handling logic */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->task_type = OMS_TASK_CANCEL;
      /* Clear the flag to avoid infinite recursion */
      self->cancel_requested = FALSE;
      FpiSsm *cancel_ssm;

      cancel_ssm = fpi_ssm_new (FP_DEVICE (self), oms_cancel_sm_run_state,
                                OMS_CANCEL_NUM_STATES);
      /* Very important; otherwise the IO SSM will not advance
       * the state machine for the Cancel command at all */
      self->task_ssm = cancel_ssm;
      fpi_ssm_start (cancel_ssm, oms_cancel_ssm_done);
      return;
    }

  if (error)
    {
      fpi_device_clear_storage_complete (device, g_steal_pointer (&error));
      return;
    }

  g_hash_table_remove_all (self->print_registry);
  fp_info ("Successfully cleared module storage");
  fpi_device_clear_storage_complete (device, NULL);
}

/* ================================================================== *
* libfprint device interface                                          *
* ================================================================== */

static const FpIdEntry id_table[] = {
  { .vid = 0x33a7,  .pid = 0x2388,  },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },   /* terminating entry */
};

static void
oms_fp_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  g_autofree gchar *serial = NULL;
  GError *error = NULL;

  usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_reset (usb_dev, &error))
    goto err_close;

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    goto err_close;

  serial = g_usb_device_get_string_descriptor (usb_dev,
                                               g_usb_device_get_serial_number_index (usb_dev),
                                               &error);
  if (!serial)
    {
      /* Many modules do not provide a serial number; use a stable ID so
       * that stored prints stay compatible with the device. */
      serial = g_strdup_printf ("oms-%04x-%04x",
                                g_usb_device_get_vid (usb_dev),
                                g_usb_device_get_pid (usb_dev));
    }

  fpi_device_set_nr_enroll_stages (device, OMS_AUTO_ENROLL_ROUNDS);

  g_usb_device_release_interface (usb_dev, 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);
  fpi_device_probe_complete (device, serial, NULL, NULL);
  return;

err_close:
  g_usb_device_close (usb_dev, NULL);
  fpi_device_probe_complete (device, NULL, NULL, error);
}

static void
oms_fp_open (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  GError *error = NULL;

  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                     0, 0, &error))
    {
      fpi_device_open_complete (device, error);
      return;
    }

  self->task_ssm = NULL;
  fpi_device_open_complete (device, NULL);
}

static void
oms_fp_close (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                  0, 0, &error);

  g_hash_table_remove_all (self->print_registry);

  fpi_device_close_complete (device, error);
}

static void
oms_fp_enroll (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_type = OMS_TASK_ENROLL;
  self->cancel_requested = FALSE;

  self->task_ssm = fpi_ssm_new (device, oms_enroll_sm_run_state,
                                OMS_ENROLL_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_enroll_ssm_done);
}

static void
oms_fp_identify (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_type = OMS_TASK_IDENTIFY;
  self->cancel_requested = FALSE;

  self->task_ssm = fpi_ssm_new (device, oms_identify_sm_run_state,
                                OMS_IDENTIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_identify_ssm_done);
}

static void
oms_fp_list (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_type = OMS_TASK_LIST;
  self->cancel_requested = FALSE;

  self->task_ssm = fpi_ssm_new (device, oms_list_sm_run_state,
                                OMS_LIST_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_list_ssm_done);
}

static void
oms_fp_delete (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_type = OMS_TASK_DELETE;
  self->cancel_requested = FALSE;

  fpi_device_get_delete_data (device, &self->print_to_delete);

  self->task_ssm = fpi_ssm_new (device, oms_delete_sm_run_state,
                                OMS_DELETE_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_delete_ssm_done);
}

static void
oms_fp_clear_storage (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  self->task_type = OMS_TASK_CLEAR;
  self->cancel_requested = FALSE;

  self->clear_initialized = FALSE;
  self->task_ssm = fpi_ssm_new (device, oms_clear_sm_run_state,
                                OMS_CLEAR_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_clear_ssm_done);
}

static void
oms_fp_cancel (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (device);

  if (self->task_type == OMS_TASK_NONE || self->task_type == OMS_TASK_CANCEL)
    return;

  self->cancel_requested = TRUE;

  if (self->task_ssm)
    g_cancellable_cancel (fpi_device_get_cancellable (device));
}


static void
fpi_device_omsmoc_init (FpiDeviceOmsMoc *self)
{
  self->print_registry = g_hash_table_new_full (g_direct_hash,
                                                g_direct_equal,
                                                NULL,
                                                (GDestroyNotify) g_object_unref);
}

static void
fpi_device_omsmoc_finalize (GObject *object)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMSMOC (object);

  g_clear_pointer (&self->print_registry, g_hash_table_unref);

  G_OBJECT_CLASS (fpi_device_omsmoc_parent_class)->finalize (object);
}

static void
fpi_device_omsmoc_class_init (FpiDeviceOmsMocClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_omsmoc_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = OMSMOC_DRIVER_FULLNAME;

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = OMS_AUTO_ENROLL_ROUNDS;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = oms_fp_probe;
  dev_class->open  = oms_fp_open;
  dev_class->close = oms_fp_close;
  dev_class->enroll = oms_fp_enroll;
  dev_class->identify = oms_fp_identify;
  dev_class->delete = oms_fp_delete;
  dev_class->clear_storage = oms_fp_clear_storage;
  dev_class->list = oms_fp_list;
  dev_class->cancel = oms_fp_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features |= FP_DEVICE_FEATURE_DUPLICATES_CHECK;
}
