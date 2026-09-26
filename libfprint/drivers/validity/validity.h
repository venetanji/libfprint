/*
 * Validity/Synaptics VCSFW fingerprint sensor driver
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

#pragma once

#include "fpi-device.h"
#include "fpi-ssm.h"
#include "validity_capture.h"
#include "validity_data.h"
#include "validity_db.h"
#include "validity_pair.h"
#include "validity_sensor.h"
#include "validity_tls.h"

/* USB Endpoint addresses */
#define VALIDITY_EP_CMD_OUT 0x01
#define VALIDITY_EP_CMD_IN 0x81
#define VALIDITY_EP_DATA_IN 0x82
#define VALIDITY_EP_INT_IN 0x83

/* USB transfer parameters */
#define VALIDITY_USB_TIMEOUT 15000
#define VALIDITY_USB_INT_TIMEOUT 100
#define VALIDITY_MAX_TRANSFER_LEN (100 * 1024)
#define VALIDITY_USB_INT_DATA_SIZE 1024
#define VALIDITY_USB_SEND_HEADER_LEN 1

/* Number of enroll stages */
#define VALIDITY_ENROLL_STAGES 8

/* Refuse a new enrollment when the DB partition has less free space than this
 * (~one finger template of ~23 KB + a user record + record headers/slack), so
 * a full partition fails early and cleanly with DATA_FULL instead of an opaque
 * 0x04b5 protocol error at create_finger. */
#define VALIDITY_ENROLL_MIN_FREE_BYTES 26624

/* Interrupt response bits */
#define VALIDITY_INT_FINGER_DOWN 0x02
#define VALIDITY_INT_SCAN_COMPLETE 0x04

typedef enum {
  VALIDITY_DEV_90 = 0,  /* 138a:0090 */
  VALIDITY_DEV_97,      /* 138a:0097 */
  VALIDITY_DEV_9A,      /* 06cb:009a */
  VALIDITY_DEV_9D,      /* 138a:009d */
  VALIDITY_DEV_AB,      /* 138a:00ab (HP ZBook Studio x360 G5) */
  VALIDITY_DEV_CB,      /* 06cb:00cb (HP Pavilion x360 14-dh) */
  VALIDITY_DEV_B7,      /* 06cb:00b7 (HP EliteBook 840 G6, 0xd51) -- hw-validated (@gfiguero) */
  VALIDITY_DEV_D2,      /* 06cb:00d2 (HP ENVY 13-aq / ProBook 450 G10, 0x969) -- hw-validated (@david.escob.rodr) */
} ValidityDeviceType;

/* Firmware version info from GET_VERSION (cmd 0x01) */
typedef struct
{
  guint32 build_time;
  guint32 build_num;
  guint8  version_major;
  guint8  version_minor;
  guint8  target;
  guint8  product;
  guint8  silicon_rev;
  guint8  formal_release;
  guint8  platform;
  guint8  patch;
  guint8  serial_number[6];
  guint16 security;
  guint8  iface;
  guint8  device_type;
} ValidityVersionInfo;

/* Open SSM states */
typedef enum {
  VALIDITY_OPEN_CLAIM_INTERFACE = 0,
  VALIDITY_OPEN_SEND_INIT,
  VALIDITY_OPEN_NUM_STATES,
} ValidityOpenState;

/* Close SSM states */
typedef enum {
  VALIDITY_CLOSE_RELEASE_INTERFACE = 0,
  VALIDITY_CLOSE_NUM_STATES,
} ValidityCloseState;

/* Calibration SSM states (runs during open, after capture_setup) */
typedef enum {
  CALIB_BUILD_CMD = 0,
  CALIB_SEND_CMD,
  CALIB_SEND_CMD_RECV,
  CALIB_READ_DATA,
  CALIB_AVERAGE_FRAMES,
  CALIB_PROCESS,
  CALIB_LOOP_CHECK,
  CALIB_BUILD_CLEAN_SLATE_CMD,
  CALIB_CLEAN_SLATE_SEND,
  CALIB_CLEAN_SLATE_RECV,
  CALIB_CLEAN_SLATE_READ,
  CALIB_SAVE_CLEAN_SLATE,
  CALIB_DONE,
  CALIB_NUM_STATES,
} ValidityCalibState;

/* Enrollment SSM states — matches python-validity sensor.py Sensor.enroll() */
typedef enum {
  ENROLL_CLEANUP_STALE = 0,    /* Close any stale enrollment session */
  ENROLL_CLEANUP_STALE_RECV,
  ENROLL_DB_INFO,              /* db_info free-space guard (avoid opaque 0x04b5) */
  ENROLL_DB_INFO_RECV,
  ENROLL_START,                /* create_enrollment (cmd 0x69 flag=1) */
  ENROLL_START_RECV,
  /* --- Per-iteration loop --- */
  ENROLL_LED_ON,
  ENROLL_LED_ON_RECV,
  ENROLL_WAIT_FINGER_DELAY,
  ENROLL_BUILD_CAPTURE,
  ENROLL_CAPTURE_SEND,
  ENROLL_CAPTURE_RECV,
  ENROLL_WAIT_FINGER,
  ENROLL_WAIT_SCAN_COMPLETE,
  ENROLL_GET_PRG_STATUS,
  ENROLL_GET_PRG_STATUS_RECV,
  ENROLL_CAPTURE_STOP,
  ENROLL_CAPTURE_STOP_RECV,
  ENROLL_UPDATE_START,
  ENROLL_UPDATE_START_RECV,
  ENROLL_WAIT_UPDATE_START_INT,  /* PY: usb.wait_int() inside enrollment_update_start */
  ENROLL_DB_WRITE_ENABLE,        /* PY: write_enable() before 1st enrollment_update */
  ENROLL_DB_WRITE_ENABLE_RECV,
  ENROLL_APPEND_IMAGE,           /* 1st enrollment_update (trigger) */
  ENROLL_APPEND_IMAGE_RECV,
  ENROLL_CLEANUPS,               /* PY: call_cleanups() after 1st enrollment_update */
  ENROLL_CLEANUPS_RECV,
  ENROLL_WAIT_UPDATE_INT,        /* PY: usb.wait_int() between the two calls */
  ENROLL_DB_WRITE_ENABLE_READ,   /* PY: write_enable() before 2nd enrollment_update */
  ENROLL_DB_WRITE_ENABLE_READ_RECV,
  ENROLL_APPEND_IMAGE_READ,      /* 2nd enrollment_update (read result) */
  ENROLL_APPEND_IMAGE_READ_RECV,
  ENROLL_CLEANUPS_READ,          /* PY: call_cleanups() after 2nd enrollment_update */
  ENROLL_CLEANUPS_READ_RECV,
  ENROLL_UPDATE_END,             /* PY: enrollment_update_end = cmd 0x69 flag=0 (finally) */
  ENROLL_UPDATE_END_RECV,
  ENROLL_LOOP_CHECK,
  /* --- Post-loop: DB commit --- */
  ENROLL_UPDATE_END2,            /* PY: 2nd enrollment_update_end after loop */
  ENROLL_UPDATE_END2_RECV,
  ENROLL_GET_STORAGE,
  ENROLL_GET_STORAGE_RECV,
  /* If storage doesn't exist (0x04b3), create it: */
  ENROLL_INIT_STORAGE_WE,       /* db_write_enable for storage creation */
  ENROLL_INIT_STORAGE_WE_RECV,
  ENROLL_INIT_STORAGE_CREATE,   /* new_record(1, 4, 3, "StgWindsor\0") */
  ENROLL_INIT_STORAGE_CREATE_RECV,
  ENROLL_INIT_STORAGE_CLEAN,    /* call_cleanups */
  ENROLL_INIT_STORAGE_CLEAN_RECV,
  ENROLL_DB_WRITE_ENABLE2,
  ENROLL_DB_WRITE_ENABLE2_RECV,
  ENROLL_CREATE_USER,
  ENROLL_CREATE_USER_RECV,
  ENROLL_CREATE_USER_CLEANUPS,
  ENROLL_CREATE_USER_CLEANUPS_RECV,
  ENROLL_DB_WRITE_ENABLE3,
  ENROLL_DB_WRITE_ENABLE3_RECV,
  ENROLL_CREATE_FINGER,
  ENROLL_CREATE_FINGER_RECV,
  ENROLL_FINAL_CLEANUPS,
  ENROLL_FINAL_CLEANUPS_RECV,
  ENROLL_WAIT_FINGER_INT,
  ENROLL_LED_OFF,                /* PY: glow_end_scan() — LAST step per PY */
  ENROLL_LED_OFF_RECV,
  ENROLL_DONE,
  ENROLL_NUM_STATES,
} ValidityEnrollState;

/* Verify/Identify SSM states */
typedef enum {
  VERIFY_LED_ON = 0,
  VERIFY_LED_ON_RECV,
  VERIFY_BUILD_CAPTURE,
  VERIFY_CAPTURE_SEND,
  VERIFY_CAPTURE_RECV,
  VERIFY_WAIT_FINGER,
  VERIFY_WAIT_SCAN_COMPLETE,
  VERIFY_GET_PRG_STATUS,
  VERIFY_GET_PRG_STATUS_RECV,
  VERIFY_CAPTURE_STOP,
  VERIFY_CAPTURE_STOP_RECV,
  VERIFY_MATCH_START,
  VERIFY_MATCH_START_RECV,
  VERIFY_WAIT_MATCH_INT,
  VERIFY_GET_RESULT,
  VERIFY_GET_RESULT_RECV,
  VERIFY_CLEANUP,
  VERIFY_CLEANUP_RECV,
  VERIFY_LED_OFF,
  VERIFY_LED_OFF_RECV,
  VERIFY_GET_MATCH_USER,       /* read the matched user record (cmd 0x4a) */
  VERIFY_GET_MATCH_USER_RECV,
  VERIFY_DONE,
  VERIFY_NUM_STATES,
} ValidityVerifyState;

/* List prints SSM states */
typedef enum {
  LIST_GET_STORAGE = 0,
  LIST_GET_STORAGE_RECV,
  LIST_GET_USER,
  LIST_GET_USER_RECV,
  LIST_DONE,
  LIST_NUM_STATES,
} ValidityListState;

/* Delete print SSM states */
typedef enum {
  DELETE_GET_STORAGE = 0,
  DELETE_GET_STORAGE_RECV,
  DELETE_LOOKUP_USER,
  DELETE_LOOKUP_USER_RECV,
  DELETE_DEL_RECORD,
  DELETE_DEL_RECORD_RECV,
  DELETE_GET_OWNER,            /* re-read the deleted finger's user record */
  DELETE_GET_OWNER_RECV,
  DELETE_DEL_OWNER,            /* delete that record if it is now empty */
  DELETE_DEL_OWNER_RECV,
  DELETE_DONE,
  DELETE_NUM_STATES,
} ValidityDeleteState;

/* Clear storage SSM states. Clear erases the whole DB partition (cmd 0x3f),
 * which reclaims flash, rather than del_record-ing each user (which does not
 * compact and leaves the partition full). */
typedef enum {
  CLEAR_DB_WRITE_ENABLE = 0,
  CLEAR_DB_WRITE_ENABLE_RECV,
  CLEAR_ERASE,
  CLEAR_ERASE_RECV,
  CLEAR_CLEANUPS,
  CLEAR_CLEANUPS_RECV,
  CLEAR_DONE,
  CLEAR_NUM_STATES,
} ValidityClearState;

#define FPI_TYPE_DEVICE_VALIDITY (fpi_device_validity_get_type ())
G_DECLARE_FINAL_TYPE (FpiDeviceValidity, fpi_device_validity,
                      FPI, DEVICE_VALIDITY, FpDevice)

/* Invoked when validity_reconnect_start() finishes re-establishing the
 * session; @success is FALSE if the device could not be recovered. */
typedef void (*ValidityReconnectCb) (FpiDeviceValidity *self,
                                     gboolean           success,
                                     gpointer           user_data);

struct _FpiDeviceValidity
{
  FpDevice            parent;

  ValidityDeviceType  dev_type;
  ValidityVersionInfo version_info;

  /* Runtime data files loaded from libfprint-validity-data package */
  ValidityDataStore device_data;
  ValidityDataStore common_data;

  /* TLS session state */
  ValidityTlsState tls;

  /* Tier 0 in-driver recovery (see validity_reconnect_start() in validity.c) */
  ValidityReconnectCb reconnect_cb;
  gpointer            reconnect_cb_data;
  guint               reconnect_attempts;
  gboolean            in_recovery;
  gboolean            recovery_redispatch;   /* re-dispatch after reconnect; keep the attempt count */

  /* Sensor identification and HAL state (post-TLS) */
  ValiditySensorState sensor;

  /* Capture program infrastructure and calibration state */
  ValidityCaptureState capture;

  /* Firmware extension status */
  gboolean fwext_loaded;

  /* Pairing state (for uninitialized devices) */
  ValidityPairState pair_state;

  /* Calibration state */
  gboolean calibrated;
  guint    calib_iteration;

  /* Enrollment state */
  guint32  enroll_key;
  guint8  *enroll_template;
  gsize    enroll_template_len;
  guint    enroll_stage;
  guint16  enroll_user_dbid;
  guint16  enroll_storage_dbid;
  guint    scan_incomplete_count;
  gboolean enroll_frame_rejected;   /* sensor rejected (0x0526) or flagged the last frame -- retry stage */
  guint    enroll_reject_retries;   /* cumulative frame retries -- bounds broken-firmware loops */
  gint     enroll_reported;         /* highest completed_stages reported this action */

  /* Verify: retry state for empty/phantom captures (no finger down) */
  guint    verify_capture_retries;
  gboolean verify_recapture;

  /* Verify/identify mode flag: TRUE = identify, FALSE = verify */
  gboolean identify_mode;

  /* Verify/identify: the user record the chip's match named, read back to
   * tell which enrolled print matched */
  ValidityUser verify_match_user;

  /* List prints state */
  ValidityUserStorage list_storage;
  guint               list_user_idx;

  /* Delete state */
  guint16  delete_storage_dbid;
  guint16  delete_finger_subtype;
  guint16  delete_finger_dbid;
  guint16  delete_owner_dbid;      /* the user record that holds the print */
  gboolean delete_owner_empties;   /* deleting the finger leaves that record empty */
  gboolean delete_finger_deleted;  /* del_record of the finger succeeded */
  guint16  delete_total_fingers;   /* fingers tallied while scanning all users */
  gboolean delete_found;           /* the print to delete was located */
  gboolean delete_scan_ok;         /* every get_user during the scan succeeded */
  gboolean delete_reclaim;         /* this delete empties the chip -> erase to reclaim */

  /* Command SSM: manages the send-cmd/recv-response cycle */
  FpiSsm *cmd_ssm;

  /* Parent SSM: back-pointer for non-subsm child SSMs */
  FpiSsm *open_ssm;

  /* Pending response data stashed for higher-level SSM consumption */
  guint16 cmd_response_status;
  guint8 *cmd_response_data;
  gsize   cmd_response_len;

  /* Bulk data buffer (EP 0x82 reads during capture/calibration) */
  guint8 *bulk_data;
  gsize   bulk_data_len;

  /* Emulation mode: in-memory print storage (element-type FpPrint) */
  GPtrArray *emulation_prints;
};

/* Enrollment SSM (validity_enroll.c) */
void validity_enroll (FpDevice *device);

/* Enrollment response parsing — exposed for unit testing */
#define ENROLLMENT_MAGIC_LEN 0x38

typedef struct
{
  guint8 *header;
  gsize   header_len;
  guint8 *template_data;
  gsize   template_len;
  guint8 *tid;
  gsize   tid_len;
} EnrollmentUpdateResult;

void     enrollment_update_result_clear (EnrollmentUpdateResult *r);
gboolean parse_enrollment_update_response (const guint8           *data,
                                           gsize                   data_len,
                                           EnrollmentUpdateResult *result);

/* Enroll progress to have reported after @frames accepted frames (@complete
 * FALSE), or once the print is stored (@complete TRUE), given the stages
 * already reported this action. Pure -- exposed for unit testing; the policy
 * is described in validity_enroll.c. */
gint     validity_enroll_progress_target (gint     reported,
                                          guint    frames,
                                          gboolean complete);

/* Error classification for in-driver recovery ("Tier 0": reconnect and retry
 * inside the driver, with no libfprint core changes). Tier 0 covers a sensor
 * reset that leaves the device on the bus; a true re-enumeration still
 * reports FP_DEVICE_ERROR_REMOVED, since recovering from it needs core
 * support (proposed in libfprint issue #852,
 * https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/852).
 *
 * Maps a transport/protocol error to a recovery class. Pure and side-effect
 * free: for the ambiguous NO_DEVICE case the caller supplies @device_alive
 * from a liveness probe -- TRUE when the sensor still answers on the bus (a
 * reset that dropped the TLS session) vs FALSE when it truly re-enumerated. */
typedef enum {
  VALIDITY_ERR_NONE = 0,       /* @error was NULL -- no error */
  VALIDITY_ERR_CANCELLED,      /* user/action cancellation -- do not recover */
  VALIDITY_ERR_PIPE_STALL,     /* transport blip, device still on the bus */
  VALIDITY_ERR_SESSION_LOST,   /* sensor reset: reconnect + restart the op */
  VALIDITY_ERR_REMOVED,        /* device gone (re-enumeration) -- REMOVED */
  VALIDITY_ERR_FATAL,          /* not recoverable -- abort with the error */
} ValidityErrClass;

ValidityErrClass validity_classify_error (const GError *error,
                                          gboolean      device_alive);

/* Liveness probe: TRUE if the sensor still answers on the bus (a reset that
 * only dropped the session) vs FALSE if it has left (re-enumeration). Does one
 * cheap control request; only call off the hot path (i.e. on an error). */
gboolean validity_probe_alive (FpiDeviceValidity *self);

/* Re-establish the sensor session in place after a mid-operation reset by
 * re-running the proven open sequence (the same one a fprintd re-open runs).
 * @cb is invoked with success/failure when it finishes. */
void validity_reconnect_start (FpiDeviceValidity  *self,
                               ValidityReconnectCb cb,
                               gpointer            user_data);

/* Max in-driver reconnect attempts per operation before giving up (REMOVED). */
#define VALIDITY_MAX_RECONNECTS 3

/* If @error during an operation is a recoverable device reset (device still on
* the bus), start an in-driver reconnect and return TRUE -- the caller must NOT
* complete its action. Otherwise return FALSE and the caller completes with the
* error. Bounded by reconnect_attempts; a true re-enumeration returns FALSE. */
gboolean validity_recover_or_fail (FpiDeviceValidity  *self,
                                   const GError       *error,
                                   ValidityReconnectCb on_done,
                                   gpointer            user_data);

/* Verify/Identify SSMs (validity_verify.c) */
/* Map a USB "no device" error (intermittent re-enumeration) to
 * FP_DEVICE_ERROR_REMOVED. Takes ownership of @error, returns the result. */
GError *validity_map_removed_error (GError *error);

void validity_verify (FpDevice *device);
void validity_identify (FpDevice *device);
void validity_list (FpDevice *device);
void validity_delete (FpDevice *device);
void validity_clear_storage (FpDevice *device);

/* Match correlation helpers (validity_verify.c) -- exposed for unit testing */
gboolean validity_print_matches_record (FpPrint                   *print,
                                        const ValidityMatchResult *match,
                                        const ValidityUser        *user);
FpPrint *validity_find_gallery_match (GPtrArray                 *gallery,
                                      const ValidityMatchResult *match,
                                      const ValidityUser        *user);
FpiMatchResult validity_match_decide (gboolean                   identify,
                                      FpPrint                   *print,
                                      GPtrArray                 *gallery,
                                      const ValidityMatchResult *match,
                                      const ValidityUser        *user,
                                      FpPrint                  **out_gallery_match);
gboolean validity_match_user_to_read (const guint8 *data,
                                      gsize         data_len,
                                      guint16      *out_dbid);
void validity_verify_clear_match_state (FpiDeviceValidity *self);

/* Delete selection helpers (validity_verify.c) -- exposed for unit testing */
gboolean validity_delete_find_finger (FpPrint            *print,
                                      const ValidityUser *user,
                                      guint16            *out_finger_dbid,
                                      gboolean           *out_leaves_empty);
gboolean validity_delete_owner_is_empty (FpPrint            *print,
                                         guint16             owner_dbid,
                                         const ValidityUser *user);
