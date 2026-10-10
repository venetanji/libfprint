/*
 * CanvasBio CB2000 Fingerprint Reader Driver for libfprint
 *
 * Copyright (C) 2025 CanvasMancer Project
 * Based on protocol analysis from libfprint issue #509
 * https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/509
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ============================================================================
 * DRIVER OVERVIEW
 * ============================================================================
 *
 * DRIVER VERSION TAG: V45
 * Keep this tag updated at every iteration to make runtime/source identification
 * unambiguous across logs, backups and manual syncs.
 *
 * Current Linux implementation uses the host-side matching path (MoH behavior):
 * the sensor captures a raw grayscale image, and the driver performs image-based
 * NCC matching. A runtime-gated skeleton hook exists for future
 * device-assisted verify experiments without changing the default behavior.
 *
 * Device specifications:
 *   - Vendor ID:  0x2DF0 (CanvasBio)
 *   - Product IDs: 0x0003
 *   - Image size: 80x64 pixels (5120 bytes)
 *   - Interface:  USB bulk transfers + vendor control requests
 *
 * ARCHITECTURE DECISION: FpDevice vs FpImageDevice
 * The CB2000 sensor has a very small physical capture area (80x64 pixels).
 * Upstream FpImageDevice utilizes the NBIS matcher, which struggles to
 * reliably extract and match minutiae from such small partial prints.
 * Because of this limitation, the driver subclasses FpDevice directly and
 * implements its own custom Normalized Cross-Correlation (NCC) image matching
 * algorithm (in canvasbio-ncc.c). This ensures acceptable FAR/FRR performance
 * that NBIS cannot achieve for this specific hardware dimension.
 *
 * Architecture: Master cycle SSM + sub-SSMs per phase.
 * First cycle does USB reset + full activation. Subsequent cycles do soft rearm:
 *
 *   HARD_RESET -> INIT_CONFIG -> SETTLE_DELAY -> WAIT_FINGER ->
 *   CAPTURE_START -> READ_IMAGE -> FINALIZE -> VERIFY_RESULT_STATUS ->
 *   VERIFY_READY_QUERY_A -> VERIFY_READY_QUERY_B -> VERIFY_READY_DECIDE ->
 *   SUBMIT_IMAGE ->
 *   WAIT_REMOVAL -> REARM -> [next cycle]
 *
 * ============================================================================
 */

#define FP_COMPONENT "canvasbio"
#define CB2000_DRIVER_VERSION_TAG "V45"

#include "drivers_api.h"
#include <string.h>  /* For memcpy/strlen */
#include <math.h>    /* For sqrt() in NCC */
#include <gio/gio.h>
#include "canvasbio-ncc.h"

/* ============================================================================
 * DEVICE CONSTANTS
 * ============================================================================ */

/* USB Vendor and Product IDs */
#define CB2000_VID 0x2df0
#define CB2000_PID_1 0x0003

/* USB Endpoints */
#define CB2000_EP_OUT 0x01
#define CB2000_EP_IN 0x82

/* Image dimensions */
#define CB2000_IMG_WIDTH 80
#define CB2000_IMG_HEIGHT 64
#define CB2000_IMG_SIZE (CB2000_IMG_WIDTH * CB2000_IMG_HEIGHT)
#define CB2000_IMG_CHUNKS 20
#define CB2000_CHUNK_SIZE 320
#define CB2000_CHUNK_PAYLOAD_SIZE 256
#define CB2000_CHUNK_DEFAULT_PAYLOAD_OFFSET \
  (CB2000_CHUNK_SIZE - CB2000_CHUNK_PAYLOAD_SIZE)   /* 64 bytes */
/* Approx. sensor size: 15mm x 9mm => ~6.22 px/mm average. */
#define CB2000_PPMM_DEFAULT 13.39     /* ~340 dpi - actual sensor resolution */
/* CLAHE tile size (80/10=8, 64/8=8 → 10x8 grid of 8x8 tiles) */
#define CB2000_CLAHE_TILE_W 8
#define CB2000_CLAHE_TILE_H 8
#define CB2000_CLAHE_CLIP_LIMIT 3.0

/* Timing constants (milliseconds) */
#define CB2000_TIMEOUT 5000                 /* USB transfer timeout */
#define CB2000_POLL_INTERVAL 16             /* Windows parity: ~16ms REQ_POLL cadence */
#define CB2000_WAKEUP_DELAY 500             /* Post-init stabilization delay */
#define CB2000_REMOVAL_INTERVAL 175         /* Finger removal poll interval */

/* Validation thresholds */
#define CB2000_POLL_DEBOUNCE_COUNT 1
#define CB2000_POLL_STALE_TIMEOUT_MS 12000  /* ~12s max wait for finger */
#define CB2000_EARLY_PLACEMENT_POLLS 0      /* disabled (Windows parity) */
#define CB2000_EARLY_PLACEMENT_DELAY_MS 400
#define CB2000_REMOVAL_STABLE_OFF_COUNT 2
#define CB2000_REMOVAL_STALE_TIMEOUT_MS 10000 /* ~10s max wait for removal */
#define CB2000_MIN_IMAGE_VARIANCE 3000.0
/* Background capture/quality heuristics */
#define CB2000_BG_CAPTURE_MAX_VARIANCE 1800.0
#define CB2000_BLOCK_SIZE 8
#define CB2000_LOW_VAR_THRESHOLD 2000.0
#define CB2000_LOW_VAR_RATIO_MAX 0.15
/* Area thresholds per action (can be overridden by env). */
#define CB2000_AREA_MIN_CAPTURE_PCT 85
#define CB2000_AREA_MIN_ENROLL_PCT 70
#define CB2000_AREA_MIN_VERIFY_PCT 70
/* Focus (Laplacian variance) threshold to avoid very blurry frames */
#define CB2000_MIN_FOCUS_VARIANCE 500.0
/* Foreground threshold used for RE-style area/overlap heuristics. */
#define CB2000_FOREGROUND_THRESHOLD 32
/* Some PCAPs show an alternate detect value for reg 0x51. */
#define CB2000_USE_ALT_DETECT_51 0

#define CB2000_PRINT_FORMAT "(uqq@ay)"

/* ============================================================================
 * USB CONTROL TRANSFER REQUESTS
 * ============================================================================ */
#define REQ_CONFIG 202                      /* 0xCA */
#define REQ_STATUS 204                      /* 0xCC */
#define REQ_POLL 218                        /* 0xDA */
#define REQ_INIT 219                        /* 0xDB */

#define CTRL_IN 0xC0
#define CTRL_OUT 0x40

/* ============================================================================
 * STATE MACHINE DEFINITIONS
 * ============================================================================
 *
 * Architecture: One master cycle SSM controls the full capture flow.
 * Sub-SSMs handle individual phases (activation, polling, image read).
 * All polling lives inside sub-SSMs - no external timers except settle delay.
 */

/* Master cycle SSM - one complete capture cycle */
typedef enum {
  CYCLE_RECOVERY,            /* Explicit recovery state */
  CYCLE_HARD_RESET,          /* USB reset + reclaim interface */
  CYCLE_INIT_CONFIG,         /* Full activation via sub-SSM */
  CYCLE_SETTLE_DELAY,        /* Post-init stabilization timer */
  CYCLE_WAIT_FINGER,         /* Finger polling via sub-SSM */
  CYCLE_EARLY_PLACEMENT_DELAY,   /* Extra delay if finger too early */
  CYCLE_CAPTURE_START,       /* Capture start commands via sub-SSM */
  CYCLE_READ_IMAGE,          /* Image read (20 chunks) via sub-SSM */
  CYCLE_FINALIZE,            /* Capture finalize commands via sub-SSM */    /* Verify READY: decide route from query */
  CYCLE_SUBMIT_IMAGE,        /* Submit image to libfprint */
  CYCLE_WAIT_REMOVAL,        /* Finger removal polling via sub-SSM */
  CYCLE_REARM,               /* Soft rearm between captures */
  CYCLE_NUM_STATES,
} CycleState;

/* Sub-SSM: finger polling loop */
typedef enum {
  POLL_FINGER_DELAY,         /* Wait 16ms */
  POLL_FINGER_SEND,          /* Submit REQ_POLL, callback decides */
  POLL_FINGER_NUM,
} PollFingerState;

/* Sub-SSM: removal polling loop */
typedef enum {
  POLL_REMOVAL_DELAY,        /* Wait 175ms */
  POLL_REMOVAL_SEND,         /* Submit REQ_POLL, callback decides */
  POLL_REMOVAL_NUM,
} PollRemovalState;

/* Sub-SSM: image read (chunk + padding loop) */
typedef enum {
  IMG_READ_CHUNK,            /* Bulk read 320 bytes */
  IMG_WRITE_PADDING,         /* Bulk write 256 zeros */
  IMG_READ_NUM,
} ImageReadState;

/* Activation sub-SSM (existing, used by CYCLE_INIT_CONFIG) */
typedef enum {
  STATE_ACTIVATE_WAKE_SEQ,
  STATE_ACTIVATE_REGS_SEQ,
  STATE_ACTIVATE_TRIGGER_SEQ,
  STATE_ACTIVATE_CAPTURE_SEQ,
  STATE_ACTIVATE_FINALIZE_SEQ,
  STATE_ACTIVATE_NUM_STATES,
} ActivateState;

/* ============================================================================
 * COMMAND TABLE STRUCTURES
 * ============================================================================ */

typedef enum {
  CMD_END = 0,
  CMD_BULK_OUT,
  CMD_BULK_IN,
  CMD_CTRL_OUT,
  CMD_CTRL_IN,
} Cb2000CmdType;

typedef struct
{
  Cb2000CmdType type;
  guint8        request;
  guint16       value;
  guint16       index;
  const guint8 *data;
  gsize         len;
} Cb2000Command;

typedef struct
{
  const Cb2000Command *commands;
  const char          *seq_name;
  gint                 cmd_index;
  gint                 cmd_total;
} Cb2000CmdContext;

typedef enum {
  CB2000_RETRY_BACKGROUND_CAPTURE = 0,
  CB2000_RETRY_AREA_GATE,
  CB2000_RETRY_QUALITY_GATE,
  CB2000_RETRY_MINUTIAE_PRECHECK,
  CB2000_RETRY_DEVICE_STATUS,
  CB2000_RETRY_ENROLL_CONSISTENCY,
  CB2000_RETRY_CAUSE_COUNT,
} Cb2000RetryCause;

typedef enum {
  CB2000_VERIFY_ACK_UNKNOWN = 0,
  CB2000_VERIFY_ACK_READY,
  CB2000_VERIFY_ACK_RETRY,
  CB2000_VERIFY_ACK_DEVICE_NOMATCH,
} Cb2000VerifyAckDecision;

typedef enum {
  CB2000_VERIFY_ROUTE_UNKNOWN = 0,
  CB2000_VERIFY_ROUTE_HOST_NBIS,
  CB2000_VERIFY_ROUTE_DEVICE_STATUS,
  CB2000_VERIFY_ROUTE_DEVICE_NOMATCH,
  CB2000_VERIFY_ROUTE_RETRY_GATE,
} Cb2000VerifyRoute;

typedef enum {
  CB2000_VERIFY_RESULT_CLASS_UNKNOWN = 0,
  CB2000_VERIFY_RESULT_CLASS_READY,
  CB2000_VERIFY_RESULT_CLASS_RETRY,
  CB2000_VERIFY_RESULT_CLASS_DEVICE_NOMATCH,
  CB2000_VERIFY_RESULT_CLASS_COUNT,
} Cb2000VerifyResultClass;

typedef enum {
  CB2000_VERIFY_READY_MATRIX_UNKNOWN = 0,
  CB2000_VERIFY_READY_MATRIX_READY,
  CB2000_VERIFY_READY_MATRIX_RETRY,
  CB2000_VERIFY_READY_MATRIX_DEVICE_NOMATCH,
} Cb2000VerifyReadyMatrixDecision;

/* ============================================================================
 * DEVICE STRUCTURE
 * ============================================================================ */
struct _FpiDeviceCanvasbio
{
  FpDevice parent;

  /* Lifecycle */
  gboolean      deactivating;
  gboolean      deactivation_in_progress;

  /* NCC enrollment state */
  guint8        *enroll_images[5];
  gint           enroll_stage;
  guint          enroll_gate_rejects;
  FpiMatchResult verify_result;

  /* Image buffer */
  guint8  *image_buffer;
  guint8  *background_buffer;
  guint8  *previous_norm_buffer;
  gboolean background_valid;
  gboolean previous_norm_valid;
  gsize    image_offset;
  gint     chunks_read;

  /* Polling counters (reset each sub-SSM) */
  gint     poll_stable_hits;
  gint     no_finger_streak;
  gint     removal_poll_count;
  gint     removal_stable_off_hits;
  gint     poll_total_count;
  gboolean early_placement_detected;
  gint64   poll_start_us;
  gint64   removal_start_us;

  /* SSM tracking */
  FpiSsm  *cycle_ssm;                 /* Master cycle SSM (single reference) */
  gboolean force_recovery;
  gboolean initial_activation_done;

  };

G_DECLARE_FINAL_TYPE (FpiDeviceCanvasbio, fpi_device_canvasbio, FPI, DEVICE_CANVASBIO, FpDevice)
G_DEFINE_TYPE (FpiDeviceCanvasbio, fpi_device_canvasbio, FP_TYPE_DEVICE)

/* ============================================================================
 * COMMAND DATA BUFFERS
 * ============================================================================ */

/* Wake/init commands */
static const guint8 data_wake[] = {0x4f, 0x80};
static const guint8 data_wake_ack[] = {0xa9, 0x4f, 0x80};
static const guint8 data_config_read[] = {0xa8, 0xb9, 0x00};
static const guint8 data_trigger[] = {0xa9, 0x09, 0x00, 0x00};
static const guint8 data_read_start[] = {0xa9, 0x0c, 0x00};
static const guint8 data_read_state[] = {0xa8, 0x20, 0x00, 0x00};
static const guint8 data_stop[] = {0xa9, 0x04, 0x00};

/* Register configuration commands (15 registers) */
static const guint8 data_reg_50[] = {0xa9, 0x50, 0x12, 0x00};
static const guint8 data_reg_5f[] = {0xa9, 0x5f, 0x00, 0x00};
static const guint8 data_reg_4e[] = {0xa9, 0x4e, 0x02, 0x00};
static const guint8 data_reg_60[] = {0xa9, 0x60, 0x21, 0x00};
static const guint8 data_reg_61[] = {0xa9, 0x61, 0x70, 0x00};
static const guint8 data_reg_62[] = {0xa9, 0x62, 0x00, 0x21};
static const guint8 data_reg_63[] = {0xa9, 0x63, 0x00, 0x21};
static const guint8 data_reg_64[] = {0xa9, 0x64, 0x04, 0x08};
static const guint8 data_reg_65[] = {0xa9, 0x65, 0x85, 0x08};
static const guint8 data_reg_66[] = {0xa9, 0x66, 0x0d, 0x00};
static const guint8 data_reg_67[] = {0xa9, 0x67, 0x10, 0x00};
static const guint8 data_reg_68[] = {0xa9, 0x68, 0x00, 0x0c};
static const guint8 data_reg_6b[] = {0xa9, 0x6b, 0x11, 0x70};
static const guint8 data_reg_6c[] = {0xa9, 0x6c, 0x00, 0x0e};

/* Capture configuration commands */
static const guint8 data_cap_03[] = {0xa9, 0x03, 0x00};
static const guint8 data_cap_38[] = {0xa9, 0x38, 0x01, 0x00};
static const guint8 data_cap_10[] = {0xa9, 0x10, 0x60, 0x00};
static const guint8 data_cap_3b[] = {0xa9, 0x3b, 0x14, 0x00};
static const guint8 data_cap_3d[] = {0xa9, 0x3d, 0xff, 0x0f};
static const guint8 data_cap_26[] = {0xa9, 0x26, 0x30, 0x00};
static const guint8 data_cap_2f[] = {0xa9, 0x2f, 0xf6, 0xff};

/* Capture start sequence commands */
static const guint8 data_cap_read_08[] = {0xa8, 0x08, 0x00};
static const guint8 data_cap_09[] = {0xa9, 0x09, 0x00, 0x00};
static const guint8 data_cap_3e[] = {0xa8, 0x3e, 0x00, 0x00};
static const guint8 data_cap_03_4b[] = {0xa9, 0x03, 0x00, 0x00};
static const guint8 data_cap_20[] = {0xa8, 0x20, 0x00, 0x00};
static const guint8 data_cap_0d[] = {0xa9, 0x0d, 0x00};
static const guint8 data_cap_10_alt[] = {0xa9, 0x10, 0x00, 0x01};
static const guint8 data_cap_26_4b[] = {0xa9, 0x26, 0x00, 0x00};
static const guint8 data_cap_0c[] = {0xa9, 0x0c, 0x00};
static const guint8 data_cap_04_4b[] = {0xa9, 0x04, 0x00, 0x00};
static const guint8 data_cap_259[259] = {0xa8, 0x06};

/* Detection mode calibration values */
static const guint8 data_cap_5d_detect[] = {0xa9, 0x5d, 0x3d, 0x00};
static const guint8 data_cap_51_detect[] = {0xa9, 0x51, 0xa8, 0x01};
static const guint8 data_cap_51_detect_alt[] = {0xa9, 0x51, 0x68, 0x01};
/* Capture mode calibration values (post-detection) */
static const guint8 data_cap_5d_capture[] = {0xa9, 0x5d, 0x4d, 0x00};
static const guint8 data_cap_51_capture[] = {0xa9, 0x51, 0x88, 0x01};
/* Chunk padding (256 zeros) */
static const guint8 data_padding[256] = {0};

/*
 * Verify READY complementary status query.
 * This mirrors the RE "status execute/query then commit class" split:
 * we ask one extra status sample after finalize before host matcher fallback.
 */


/* ============================================================================
 * COMMAND TABLES
 * ============================================================================ */

/* Activation Phase 1: Wake sequence (PCAP-aligned) */
static const Cb2000Command activation_wake_cmds[] = {
  {CMD_CTRL_OUT, REQ_INIT,   0x0001, 1, NULL, 0},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 2, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_wake, 2},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_wake_ack, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_config_read, 3},
  /* Windows emits a short 3-byte BULK_IN here before register setup starts. */
  {CMD_BULK_IN,  0, 0, 0, NULL, 3},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 2: Register configuration (PCAP-aligned) */
static const Cb2000Command activation_regs_cmds[] = {
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_50, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_5f, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_4e, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_60, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_61, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_62, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_63, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_64, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_65, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_66, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_67, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_68, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_6b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_reg_6c, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  /* Keep trigger preamble in the next phase to avoid duplicating 0x0002:4. */
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 3: Init trigger (PCAP-aligned, 7 commands) */
static const Cb2000Command activation_trigger_cmds[] = {
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_INIT, 0x0001, 1, NULL, 0},
  {CMD_CTRL_IN,  REQ_POLL, 0x00fe, 0, NULL, 2},
  {CMD_CTRL_IN,  REQ_POLL, 0x00ff, 0, NULL, 2},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 4: Capture configuration (PCAP-aligned, 27 commands) - detection mode */
static const Cb2000Command activation_capture_cmds[] = {
  /* activation_trigger already ends with REQ_CONFIG 0x0002 idx=4. */
  {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_detect, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0,
   CB2000_USE_ALT_DETECT_51 ? data_cap_51_detect_alt : data_cap_51_detect, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_38, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_10, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3b, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3d, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_26, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_2f, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Activation Phase 5: Finalize (PCAP-aligned, 11 commands) */
static const Cb2000Command activation_finalize_cmds[] = {
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_read_start, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_read_state, 4},
  /* Windows reads 4 bytes from BULK_IN before issuing the stop command. */
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_INIT, 0x0007, 0, NULL, 0},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_stop, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Soft rearm between captures (PCAP-aligned). */
static const Cb2000Command rearm_cmds[] = {
  /* Windows verify traces end with REQ_INIT 0x0001 using wIndex=0. */
  {CMD_CTRL_OUT, REQ_INIT, 0x0001, 0, NULL, 0},

  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_detect, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0,
   CB2000_USE_ALT_DETECT_51 ? data_cap_51_detect_alt : data_cap_51_detect, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_03, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_38, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_10, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3d, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_26, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_2f, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},

  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_trigger, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_read_start, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_read_state, 4},
  {CMD_CTRL_OUT, REQ_INIT, 0x0007, 0, NULL, 0},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_stop, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Capture start sequence (PCAP-aligned, double capture) - sent after finger detected.
 * Phase 2: first capture trigger (a8:08) + quality check (a8:3e).
 * Phase 3: post-quality config (a8:20 status reads).
 * Phase 4: calibration + second capture trigger (idx=5123). */
static const Cb2000Command capture_start_cmds[] = {
  /* Phase 2: First capture trigger + quality response */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_read_08, 3},
  {CMD_BULK_IN,  0, 0, 0, NULL, 3},         /* First capture quality: ff:00:XX */
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Quality ACK: ff:00:YY:ZZ */
  /* Phase 3: Post-quality config */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_03_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Status #2: ff:00:01:07 */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_10_alt, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_26_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0c, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Status #3: ff:00:01:02 */
  /* Phase 4: Calibration + second capture trigger */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  /* Switch to CAPTURE mode values (post-detection) */
  {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_capture, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_51_capture, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_04_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 5123, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_259, 259},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Verify-specific start sequence.
 * Kept as a dedicated table (even if currently equal to capture_start_cmds)
 * so we can evolve verify 1:1 without impacting enroll/capture flow.
 */
static const Cb2000Command verify_start_cmds[] = {
  /* Phase 2: First capture trigger + quality response */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_read_08, 3},
  {CMD_BULK_IN,  0, 0, 0, NULL, 3},         /* First capture quality: ff:00:XX */
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Quality ACK: ff:00:YY:ZZ */
  /* Phase 3: Post-quality config */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_03_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Status #2: ff:00:01:07 */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_10_alt, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_26_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0c, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_20, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},         /* Status #3: ff:00:01:02 */
  /* Phase 4: Calibration + second capture trigger */
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_5d_capture, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_51_capture, 4},
  {CMD_CTRL_IN, REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_04_4b, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 5123, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_259, 259},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* Capture finalize sequence (PCAP-aligned) */
static const Cb2000Command capture_finalize_cmds[] = {
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/*
 * Verify-specific finalize sequence.
 * Dedicated table to isolate verify evolution from enroll/capture behavior.
 */
static const Cb2000Command verify_finalize_cmds[] = {
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_09, 4},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0003, 4, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_3e, 4},
  {CMD_BULK_IN,  0, 0, 0, NULL, 4},
  {CMD_CTRL_OUT, REQ_CONFIG, 0x0002, 3, NULL, 0},
  {CMD_BULK_OUT, 0, 0, 0, data_cap_0d, 3},
  {CMD_CTRL_IN,  REQ_STATUS, 0x0000, 0, NULL, 4},
  {CMD_END, 0, 0, 0, NULL, 0}
};

/* ============================================================================
 * SEQUENTIAL COMMAND EXECUTOR
 * ============================================================================ */

/* Forward declarations */
static void cmd_sequence_run (FpiSsm   *ssm,
                              FpDevice *dev);
static void cmd_transfer_cb (FpiUsbTransfer *transfer,
                             FpDevice       *dev,
                             gpointer        user_data,
                             GError         *error);
static const char *cb2000_cmd_type_to_str (Cb2000CmdType type);
static gboolean cb2000_is_verify_phase_action (FpiDeviceAction action);
static const char *cb2000_capture_start_seq_name_for_action (FpiDeviceAction action);
static const char *cb2000_capture_finalize_seq_name_for_action (FpiDeviceAction action);
static void cb2000_log_cmd_rx_data (FpDevice               *dev,
                                    const Cb2000CmdContext *ctx,
                                    const Cb2000Command    *cmd,
                                    const FpiUsbTransfer   *transfer);
static void start_new_cycle (FpDevice *dev);

/* Invert grayscale image in-place (sensor outputs inverted polarity). */
static void G_GNUC_UNUSED
invert_image_u8 (guint8 *data, gsize len)
{
  for (gsize i = 0; i < len; i++)
    data[i] = 0xff - data[i];
}


/*
 * Parse a boolean env var with explicit defaults.
 * Accepted true values: 1,true,yes,on
 * Accepted false values: 0,false,no,off
 */
static gboolean
cb2000_get_env_bool (const gchar *name, gboolean default_value)
{
  return default_value;
}

static int
cb2000_parse_upscale_value (const gchar *v, int default_value)
{
  return default_value;
}

static int
cb2000_get_env_int (const gchar *name,
                    int          default_value,
                    int          min_value,
                    int          max_value)
{
  const gchar *v = g_getenv (name);
  int out;

  out = default_value;
  if (v && *v)
    out = atoi (v);

  if (out < min_value)
    out = min_value;
  if (out > max_value)
    out = max_value;

  return out;
}




/*
 * Retry-class status pairs observed in Windows captures.
 * We keep this conservative: unknown values never become hard decisions.
 */











static int
cb2000_get_upscale_factor (FpiDeviceAction action)
{
  const gchar *v_phase = NULL;
  const gchar *v_global = g_getenv ("CB2000_UPSCALE");
  int action_default = 1;

  /*
   * Keep preprocess parity between template generation and matching:
   * enroll/verify/identify default to 2x, capture remains 1x.
   */
  switch (action)
    {
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      action_default = 1;
      v_phase = g_getenv ("CB2000_VERIFY_UPSCALE");
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
      action_default = 1;
      v_phase = g_getenv ("CB2000_IDENTIFY_UPSCALE");
      break;

    case FPI_DEVICE_ACTION_ENROLL:
      action_default = 1;
      v_phase = g_getenv ("CB2000_ENROLL_UPSCALE");
      break;

    case FPI_DEVICE_ACTION_CAPTURE:
      v_phase = g_getenv ("CB2000_CAPTURE_UPSCALE");
      break;

    default:
      break;
    }

  if (v_phase && *v_phase)
    return cb2000_parse_upscale_value (v_phase, action_default);

  return cb2000_parse_upscale_value (v_global, action_default);
}

/*
 * Background subtraction helps remove fixed-pattern noise in enroll/capture.
 * Default is configured per action via env knobs, with verify/identify mapped
 * by CB2000_VERIFY_BG_SUBTRACT and others by CB2000_BG_SUBTRACT.
 */
static gboolean
cb2000_should_apply_bg_subtraction (FpiDeviceAction action)
{
  /* Keep verify/identify aligned with enroll defaults for NBIS matching. */
  if (action == FPI_DEVICE_ACTION_VERIFY)
    return cb2000_get_env_bool ("CB2000_VERIFY_BG_SUBTRACT", TRUE);

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      const gchar *v_identify = g_getenv ("CB2000_IDENTIFY_BG_SUBTRACT");
      if (v_identify && *v_identify)
        return cb2000_get_env_bool ("CB2000_IDENTIFY_BG_SUBTRACT", TRUE);
      return cb2000_get_env_bool ("CB2000_VERIFY_BG_SUBTRACT", TRUE);
    }

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      const gchar *v_enroll = g_getenv ("CB2000_ENROLL_BG_SUBTRACT");
      if (v_enroll && *v_enroll)
        return cb2000_get_env_bool ("CB2000_ENROLL_BG_SUBTRACT", TRUE);
      return cb2000_get_env_bool ("CB2000_BG_SUBTRACT", TRUE);
    }

  if (action == FPI_DEVICE_ACTION_CAPTURE)
    {
      const gchar *v_capture = g_getenv ("CB2000_CAPTURE_BG_SUBTRACT");
      if (v_capture && *v_capture)
        return cb2000_get_env_bool ("CB2000_CAPTURE_BG_SUBTRACT", TRUE);
    }

  return cb2000_get_env_bool ("CB2000_BG_SUBTRACT", TRUE);
}

static double
cb2000_get_ppmm (void)
{
  const char *v_ppmm = g_getenv ("CB2000_PPMM");

  if (v_ppmm && *v_ppmm)
    {
      double ppmm = g_ascii_strtod (v_ppmm, NULL);
      if (ppmm > 0.0)
        return ppmm;
    }

  const char *v_dpi = g_getenv ("CB2000_DPI");
  if (v_dpi && *v_dpi)
    {
      double dpi = g_ascii_strtod (v_dpi, NULL);
      if (dpi > 0.0)
        return dpi / 25.4;
    }

  return CB2000_PPMM_DEFAULT;
}

static gboolean
cb2000_should_precheck_minutiae (FpiDeviceAction action)
{
  const char *v = g_getenv ("CB2000_PRECHECK_MINUTIAE");

  if (v && *v && strcmp (v, "0") != 0)
    return TRUE;

  if (action == FPI_DEVICE_ACTION_VERIFY)
    return cb2000_get_env_bool ("CB2000_PRECHECK_VERIFY", FALSE);

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    return cb2000_get_env_bool ("CB2000_PRECHECK_IDENTIFY", FALSE);

  if (action == FPI_DEVICE_ACTION_ENROLL)
    return cb2000_get_env_bool ("CB2000_PRECHECK_ENROLL", FALSE);

  if (action == FPI_DEVICE_ACTION_CAPTURE)
    return cb2000_get_env_bool ("CB2000_PRECHECK_CAPTURE", FALSE);

  return FALSE;
}

static const gchar *
cb2000_action_label (FpiDeviceAction action)
{
  return "";
}


static int
cb2000_get_area_min_pct (FpiDeviceAction action)
{
  int action_default = CB2000_AREA_MIN_CAPTURE_PCT;
  int global_min = cb2000_get_env_int ("CB2000_AREA_MIN",
                                       CB2000_AREA_MIN_CAPTURE_PCT,
                                       1, 100);
  int capture_min = 0;

  /* Non-image actions still need explicit handling to keep -Wswitch-enum clean. */
  capture_min = cb2000_get_env_int ("CB2000_AREA_MIN_CAPTURE", global_min, 1, 100);

  switch (action)
    {
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      return capture_min;

    case FPI_DEVICE_ACTION_ENROLL:
      action_default = CB2000_AREA_MIN_ENROLL_PCT;
      global_min = cb2000_get_env_int ("CB2000_AREA_MIN",
                                       action_default, 1, 100);
      return cb2000_get_env_int ("CB2000_AREA_MIN_ENROLL", global_min, 1, 100);

    case FPI_DEVICE_ACTION_VERIFY:
    case FPI_DEVICE_ACTION_IDENTIFY:
      action_default = CB2000_AREA_MIN_VERIFY_PCT;
      global_min = cb2000_get_env_int ("CB2000_AREA_MIN",
                                       action_default, 1, 100);
      return cb2000_get_env_int ("CB2000_AREA_MIN_VERIFY", global_min, 1, 100);

    case FPI_DEVICE_ACTION_CAPTURE:
    default:
      return capture_min;
    }
}

/* Internal helper: read internal NBIS subprint cardinality for telemetry only. */


static void G_GNUC_UNUSED
cb2000_log_preprocess_profile (FpiDeviceAction action)
{
  fp_info ("[ PROFILE ] action=%s bg_subtract=%d upscale=%dx precheck=%d area_min=%d",
           cb2000_action_label (action),
           cb2000_should_apply_bg_subtraction (action),
           cb2000_get_upscale_factor (action),
           cb2000_should_precheck_minutiae (action),
           cb2000_get_area_min_pct (action));
}

static void
cb2000_update_submit_telemetry (FpiDeviceCanvasbio *self,
                                FpDevice           *dev,
                                FpiDeviceAction     action)
{
}


/*
 * Trilha B skeleton:
 * runtime gate for future verify path that uses device-assisted matching.
 * Current behavior intentionally falls back to host-side NBIS matching.
 */
static gboolean G_GNUC_UNUSED
cb2000_try_device_assisted_verify (FpiDeviceCanvasbio *self,
                                   FpDevice           *dev,
                                   FpImage            *img)
{
  return FALSE;
}


static void
cb2000_retry_scan_with_cause (FpDevice        *dev,
                              Cb2000RetryCause cause,
                              const gchar     *detail)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  FpDeviceRetry retry_error = FP_DEVICE_RETRY_GENERAL;
  const gchar *retry_msg = NULL;

  if (cause == CB2000_RETRY_AREA_GATE)
    {
      retry_error = FP_DEVICE_RETRY_TOO_SHORT;
      retry_msg = "Please press your finger more firmly and cover the entire sensor.";
    }
  else if (cause == CB2000_RETRY_QUALITY_GATE)
    {
      retry_msg = "Image quality too low, adjust finger and try again.";
    }
  else if (cause == CB2000_RETRY_MINUTIAE_PRECHECK)
    {
      retry_msg = "Adjust finger and try again.";
    }
  else if (cause == CB2000_RETRY_ENROLL_CONSISTENCY)
    {
      retry_error = FP_DEVICE_RETRY_CENTER_FINGER;
      retry_msg = "Place the same finger centered, as in the previous touch.";
    }

  if (cb2000_is_verify_phase_action (action))
    {
      self->verify_result = FPI_MATCH_ERROR;
      return;
    }

  if (action == FPI_DEVICE_ACTION_CAPTURE)
    return;

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      GError *err = NULL;
      if (retry_msg)
        err = fpi_device_retry_new_msg (retry_error, "%s", retry_msg);
      else
        err = fpi_device_retry_new (retry_error);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL, err);
      return;
    }
}

typedef struct
{
  FpDevice       *dev;
  FpiSsm         *ssm;
  FpImage        *img;
  FpiDeviceAction action;
} Cb2000MinutiaePrecheckCtx;

static void
cb2000_minutiae_precheck_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Cb2000MinutiaePrecheckCtx *ctx = user_data;

  g_autoptr(GError) error = NULL;

  if (!fp_image_detect_minutiae_finish (FP_IMAGE (source), res, &error))
    {
      fp_dbg ("Image screening failed with error %s",
              error ? error->message : "unknown");
      fp_info ("Reject! Image quality too bad. (minutiae precheck)");
      g_object_unref (ctx->img);
      cb2000_retry_scan_with_cause (ctx->dev,
                                    CB2000_RETRY_MINUTIAE_PRECHECK,
                                    "(precheck)");
      fpi_ssm_next_state (ctx->ssm);
      g_free (ctx);
      return;
    }  if (ctx->action == FPI_DEVICE_ACTION_CAPTURE)
    {
      /* Capture action must complete explicitly after async precheck. */
      fpi_device_capture_complete (ctx->dev, ctx->img, NULL);
      fpi_ssm_mark_completed (ctx->ssm);
      g_free (ctx);
      return;
    }

  /* For other actions, precheck is disabled by default in V33. */
  g_object_unref (ctx->img);
  fpi_ssm_next_state (ctx->ssm);
  g_free (ctx);
}


/*
 * Run a table-driven USB command sequence inside a sub-SSM.
 * The sub-SSM advances one command per callback and completes at CMD_END.
 */
static void
run_command_sequence (FpiSsm *parent_ssm, FpDevice *dev,
                      const char *seq_name,
                      const Cb2000Command *cmds)
{
  FpiSsm *subsm;
  Cb2000CmdContext *ctx;
  gint total = 0;

  ctx = g_new0 (Cb2000CmdContext, 1);
  ctx->commands = cmds;
  ctx->seq_name = seq_name ? seq_name : "unnamed_seq";
  ctx->cmd_index = 0;
  while (cmds[total].type != CMD_END)
    total++;
  ctx->cmd_total = total;

  subsm = fpi_ssm_new (dev, cmd_sequence_run, 1);
  fpi_ssm_set_data (subsm, ctx, g_free);
  fpi_ssm_start_subsm (parent_ssm, subsm);
}

static const char *
cb2000_cmd_type_to_str (Cb2000CmdType type)
{
  switch (type)
    {
    case CMD_END:
      return "END";

    case CMD_BULK_OUT:
      return "BULK_OUT";

    case CMD_BULK_IN:
      return "BULK_IN";

    case CMD_CTRL_OUT:
      return "CTRL_OUT";

    case CMD_CTRL_IN:
      return "CTRL_IN";

    default:
      return "UNKNOWN";
    }
}

static gboolean
cb2000_is_verify_phase_action (FpiDeviceAction action)
{
  return action == FPI_DEVICE_ACTION_VERIFY ||
         action == FPI_DEVICE_ACTION_IDENTIFY;
}

static const char *
cb2000_capture_start_seq_name_for_action (FpiDeviceAction action)
{
  if (cb2000_is_verify_phase_action (action))
    return "verify_start";
  return "capture_start_enroll";
}

static const char *
cb2000_capture_finalize_seq_name_for_action (FpiDeviceAction action)
{
  if (cb2000_is_verify_phase_action (action))
    return "verify_finalize";
  return "capture_finalize_enroll";
}

static const Cb2000Command *
cb2000_capture_start_cmds_for_action (FpiDeviceAction action)
{
  if (cb2000_is_verify_phase_action (action))
    return verify_start_cmds;
  return capture_start_cmds;
}

static const Cb2000Command *
cb2000_capture_finalize_cmds_for_action (FpiDeviceAction action)
{
  if (cb2000_is_verify_phase_action (action))
    return verify_finalize_cmds;
  return capture_finalize_cmds;
}



/*
 * Log short response payloads from command-sequence reads.
 * This is used to correlate verify status bytes with RE/Windows traces.
 */
static void
cb2000_log_cmd_rx_data (FpDevice               *dev,
                        const Cb2000CmdContext *ctx,
                        const Cb2000Command    *cmd,
                        const FpiUsbTransfer   *transfer)
{
}








/*
 * Normalize verify status/result byte pairs into a stable class.
 * This class is used for routing telemetry and for optional short-circuit
 * handling when device status is explicit enough to skip host NBIS.
 */






/*
 * V29 READY matrix decision.
 * Priority is conservative:
 *   1) any 0x02 -> RETRY
 *   2) any 0x0b -> DEVICE_NOMATCH
 *   3) A=0x00 and (B missing or B=0x00) -> READY fallback to host NBIS
 *   4) otherwise -> UNKNOWN fallback to host NBIS
 */


/*
 * Map finalize ACKs to a verify routing decision.
 * Runtime evidence so far:
 *   ff:00:ff:0f -> capture ready, proceed to host matcher
 *   ff:00:00:00 -> retry
 *   ff:00:ff:0c -> retry (observed in poor/partial captures)
 *   ff:00:f0:0f, ff:00:f3:0f, ff:00:88:08, ff:00:11:01,
 *   ff:00:08:00, ff:00:20:0f, ff:00:00:08, ff:00:11:00,
 *   ff:00:30:0f -> retry (Windows verify failures, 2026-02-16)
 *
 * Hypothesis from Windows RE:
 *   ff:00:ff:0b -> explicit device no-match status
 */



/*
 * Gate verify/identify routing using capture_finalize ACK bytes.
 * This performs only "early exits" (retry). Match/no-match decisions for
 * experimental device-assisted flow are handled later by
 * cb2000_try_device_assisted_verify().
 */



/*
 * Execute the next command in the sequence by mapping each entry to a
 * libfprint USB transfer. The callback advances the state machine.
 */
static void
cmd_sequence_run (FpiSsm *ssm, FpDevice *dev)
{
  Cb2000CmdContext *ctx = fpi_ssm_get_data (ssm);
  const Cb2000Command *cmd = &ctx->commands[ctx->cmd_index];
  FpiUsbTransfer *transfer;

  if (cmd->type == CMD_END)
    {
      fp_dbg ("Command sequence complete (%d commands)", ctx->cmd_total);
      fpi_ssm_mark_completed (ssm);
      return;
    }

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;

  switch (cmd->type)
    {
    case CMD_BULK_OUT:
      if (cmd->data != NULL && cmd->len >= 4)
        {
          fp_dbg ("[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x:%02x",
                  ctx->seq_name,
                  ctx->cmd_index + 1,
                  ctx->cmd_total,
                  cb2000_cmd_type_to_str (cmd->type),
                  cmd->len,
                  cmd->data[0], cmd->data[1], cmd->data[2], cmd->data[3]);
        }
      else if (cmd->data != NULL && cmd->len == 3)
        {
          fp_dbg ("[%s] cmd %d/%d %s len=%zu data=%02x:%02x:%02x",
                  ctx->seq_name,
                  ctx->cmd_index + 1,
                  ctx->cmd_total,
                  cb2000_cmd_type_to_str (cmd->type),
                  cmd->len,
                  cmd->data[0], cmd->data[1], cmd->data[2]);
        }
      else if (cmd->data != NULL && cmd->len == 2)
        {
          fp_dbg ("[%s] cmd %d/%d %s len=%zu data=%02x:%02x",
                  ctx->seq_name,
                  ctx->cmd_index + 1,
                  ctx->cmd_total,
                  cb2000_cmd_type_to_str (cmd->type),
                  cmd->len,
                  cmd->data[0], cmd->data[1]);
        }
      else if (cmd->data != NULL && cmd->len == 1)
        {
          fp_dbg ("[%s] cmd %d/%d %s len=%zu data=%02x",
                  ctx->seq_name,
                  ctx->cmd_index + 1,
                  ctx->cmd_total,
                  cb2000_cmd_type_to_str (cmd->type),
                  cmd->len,
                  cmd->data[0]);
        }
      else
        {
          fp_dbg ("[%s] cmd %d/%d %s len=%zu",
                  ctx->seq_name,
                  ctx->cmd_index + 1,
                  ctx->cmd_total,
                  cb2000_cmd_type_to_str (cmd->type),
                  cmd->len);
        }
      fpi_usb_transfer_fill_bulk_full (transfer, CB2000_EP_OUT,
                                       (guint8 *) cmd->data, cmd->len, NULL);
      transfer->short_is_error = TRUE;
      break;

    case CMD_BULK_IN:
      fp_dbg ("[%s] cmd %d/%d %s len=%zu",
              ctx->seq_name,
              ctx->cmd_index + 1,
              ctx->cmd_total,
              cb2000_cmd_type_to_str (cmd->type),
              cmd->len);
      fpi_usb_transfer_fill_bulk (transfer, CB2000_EP_IN, cmd->len);
      break;

    case CMD_CTRL_OUT:
      fp_dbg ("[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u",
              ctx->seq_name,
              ctx->cmd_index + 1,
              ctx->cmd_total,
              cb2000_cmd_type_to_str (cmd->type),
              cmd->request, cmd->value, cmd->index);
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     cmd->request, cmd->value, cmd->index, 0);
      break;

    case CMD_CTRL_IN:
      fp_dbg ("[%s] cmd %d/%d %s req=0x%02x value=0x%04x index=%u len=%zu",
              ctx->seq_name,
              ctx->cmd_index + 1,
              ctx->cmd_total,
              cb2000_cmd_type_to_str (cmd->type),
              cmd->request, cmd->value, cmd->index, cmd->len);
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     cmd->request, cmd->value, cmd->index,
                                     cmd->len);
      break;

    case CMD_END:
      g_assert_not_reached ();
      return;
    }

  fpi_usb_transfer_submit (transfer, CB2000_TIMEOUT, fpi_device_get_cancellable (dev),
                           cmd_transfer_cb, NULL);
}

/*
 * Command transfer completion: on success, advance to the next command;
 * on failure, abort the sequence and bubble up the error.
 */
static void
cmd_transfer_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  Cb2000CmdContext *ctx = fpi_ssm_get_data (transfer->ssm);
  const Cb2000Command *cmd = &ctx->commands[ctx->cmd_index];

  if (error)
    {
      if (error->message && strstr (error->message, "more data than requested"))
        {
          fp_warn ("Command transfer overflow tolerated: %s", error->message);
          g_clear_error (&error);
          if (transfer->actual_length == 0 || transfer->actual_length == (gsize) -1)
            transfer->actual_length = cmd->len;
        }
      else
        {
          fp_warn ("Command transfer failed: %s", error->message);
          fpi_ssm_mark_failed (transfer->ssm, error);
          return;
        }
    }

  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           g_error_new_literal (G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                "Operation cancelled"));
      return;
    }

  cb2000_log_cmd_rx_data (dev, ctx, cmd, transfer);
  ctx->cmd_index++;
  fpi_ssm_jump_to_state (transfer->ssm, 0);
}

/* ============================================================================
 * USB TRANSFER HELPERS
 * ============================================================================ */

/*
 * Submit a vendor control IN request and bind it to an SSM.
 * The SSM will keep ownership of the transfer lifecycle.
 */
static void
cb2000_ctrl_in (FpDevice              *dev,
                FpiSsm                *ssm,
                guint8                 request,
                guint16                value,
                guint16                index,
                gsize                  len,
                FpiUsbTransferCallback callback,
                gpointer               user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 request, value, index, len);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, CB2000_TIMEOUT, fpi_device_get_cancellable (dev),
                           callback, user_data);
}

/*
 * Submit a bulk read for a fixed-length chunk from the sensor.
 */
static void
cb2000_bulk_read (FpDevice              *dev,
                  FpiSsm                *ssm,
                  gsize                  len,
                  FpiUsbTransferCallback callback,
                  gpointer               user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, CB2000_EP_IN, len);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, CB2000_TIMEOUT, fpi_device_get_cancellable (dev),
                           callback, user_data);
}

/*
 * Submit a bulk write. Used for padding "keepalive" writes between reads.
 */
static void
cb2000_bulk_write (FpDevice              *dev,
                   FpiSsm                *ssm,
                   const guint8          *data,
                   gsize                  len,
                   FpiUsbTransferCallback callback,
                   gpointer               user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, CB2000_EP_OUT,
                                   (guint8 *) data, len, NULL);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer, CB2000_TIMEOUT, fpi_device_get_cancellable (dev),
                           callback, user_data);
}

/* ============================================================================
 * ACTIVATION SUB-SSM
 * ============================================================================ */

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case STATE_ACTIVATE_WAKE_SEQ:
      fp_dbg ("Activation: WAKE_SEQ (9 commands)");
      run_command_sequence (ssm, dev, "activation_wake", activation_wake_cmds);
      break;

    case STATE_ACTIVATE_REGS_SEQ:
      fp_dbg ("Activation: REGS_SEQ (42 commands)");
      run_command_sequence (ssm, dev, "activation_regs", activation_regs_cmds);
      break;

    case STATE_ACTIVATE_TRIGGER_SEQ:
      fp_dbg ("Activation: TRIGGER_SEQ (7 commands)");
      run_command_sequence (ssm, dev, "activation_trigger", activation_trigger_cmds);
      break;

    case STATE_ACTIVATE_CAPTURE_SEQ:
      fp_dbg ("Activation: CAPTURE_SEQ (27 commands)");
      run_command_sequence (ssm, dev, "activation_capture", activation_capture_cmds);
      break;

    case STATE_ACTIVATE_FINALIZE_SEQ:
      fp_dbg ("Activation: FINALIZE_SEQ (11 commands)");
      run_command_sequence (ssm, dev, "activation_finalize", activation_finalize_cmds);
      break;
    }
}

/* ============================================================================
 * FINGER POLLING SUB-SSM
 * ============================================================================ */

static void poll_finger_run_state (FpiSsm   *ssm,
                                   FpDevice *dev);

/*
 * Poll response handler. Implements debounce and finger detection.
 * The device reports finger presence via two status bytes; we treat either
 * bit as a valid presence signal for safety.
 */
static void
poll_finger_result_cb (FpiUsbTransfer *transfer,
                       FpDevice       *dev,
                       gpointer        user_data,
                       GError         *error)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  gint64 now_us = g_get_monotonic_time ();

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           g_error_new_literal (G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                "Operation cancelled"));
      return;
    }

  if (transfer->actual_length < 2)
    {
      fp_dbg ("Poll: short read (%zu bytes), retrying",
              transfer->actual_length);
      fpi_ssm_jump_to_state (transfer->ssm, POLL_FINGER_DELAY);
      return;
    }

  /* The device can signal presence via either byte. */
  gboolean finger_present = (transfer->buffer[1] != 0) ||
                            (transfer->buffer[0] == 0x01);
  self->poll_total_count++;

  fp_dbg ("Poll response: len=%zu [0]=%02x [1]=%02x (hits=%d streak=%d)",
          transfer->actual_length,
          transfer->buffer[0], transfer->buffer[1],
          self->poll_stable_hits, self->no_finger_streak);
  if (self->poll_total_count == 0)
    fp_dbg ("WAIT_FINGER started: t0=%" G_GINT64_FORMAT "us",
            self->poll_start_us);

  if (self->poll_total_count > 0 && (self->poll_total_count % 10) == 0)
    fp_dbg ("WAIT_FINGER timing: poll=%u t=+%" G_GINT64_FORMAT "us",
            self->poll_total_count, (now_us - self->poll_start_us));

  if (finger_present)
    {
      self->no_finger_streak = 0;
      self->poll_stable_hits++;

      /* Disabled for Windows parity (no early-placement delay). */
      if (self->poll_total_count <= CB2000_EARLY_PLACEMENT_POLLS)
        fp_dbg ("Early placement gate disabled (poll=%d)",
                self->poll_total_count);

      if (self->poll_stable_hits >= CB2000_POLL_DEBOUNCE_COUNT)
        {
          fp_dbg ("Finger detected (stable, hits=%d)", self->poll_stable_hits);
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }

      fp_dbg ("Finger detected but debouncing (%d/%d)",
              self->poll_stable_hits, CB2000_POLL_DEBOUNCE_COUNT);
    }
  else
    {
      self->poll_stable_hits = 0;
      self->no_finger_streak++;
    }

  /* Loop: back to delay */
  fpi_ssm_jump_to_state (transfer->ssm, POLL_FINGER_DELAY);
}

/*
 * Polling sub-SSM entry point. Alternates between a fixed delay and a
 * control IN request.
 */
static void
poll_finger_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);

  if (self->deactivating || self->deactivation_in_progress)
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case POLL_FINGER_DELAY:
      fpi_ssm_next_state_delayed (ssm, CB2000_POLL_INTERVAL);
      break;

    case POLL_FINGER_SEND:
      fp_dbg ("Polling REQ_POLL (wValue=0x0007 wIndex=0 len=2)");
      cb2000_ctrl_in (dev, ssm,
                      REQ_POLL, 0x0007, 0, 2,
                      poll_finger_result_cb, NULL);
      break;
    }
}

/* ============================================================================
 * REMOVAL POLLING SUB-SSM
 * ============================================================================ */

static void poll_removal_run_state (FpiSsm   *ssm,
                                    FpDevice *dev);

/*
 * Removal response handler. Requires N consecutive "no finger" readings
 * to declare removal, to avoid flicker.
 */
static void
poll_removal_result_cb (FpiUsbTransfer *transfer,
                        FpDevice       *dev,
                        gpointer        user_data,
                        GError         *error)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  gint64 now_us = g_get_monotonic_time ();

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           g_error_new_literal (G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                "Operation cancelled"));
      return;
    }

  if (transfer->actual_length < 2)
    {
      fpi_ssm_jump_to_state (transfer->ssm, POLL_REMOVAL_DELAY);
      return;
    }

  gboolean finger_present = (transfer->buffer[1] != 0);
  fp_dbg ("Removal poll: [0]=%02x [1]=%02x finger_present=%d",
          transfer->buffer[0], transfer->buffer[1], finger_present);

  self->removal_poll_count++;

  if ((now_us - self->removal_start_us) >
      (gint64) CB2000_REMOVAL_STALE_TIMEOUT_MS * 1000)
    {
      fp_warn ("WAIT_REMOVAL stale (>%dms) - triggering recovery",
               CB2000_REMOVAL_STALE_TIMEOUT_MS);
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Removal polling stale"));
      return;
    }

  if (finger_present)
    {
      self->removal_stable_off_hits = 0;
      fpi_ssm_jump_to_state (transfer->ssm, POLL_REMOVAL_DELAY);
    }
  else
    {
      self->removal_stable_off_hits++;
      if (self->removal_stable_off_hits < CB2000_REMOVAL_STABLE_OFF_COUNT)
        {
          fp_dbg ("Removal debounce: off hits %d/%d",
                  self->removal_stable_off_hits,
                  CB2000_REMOVAL_STABLE_OFF_COUNT);
          fpi_ssm_jump_to_state (transfer->ssm, POLL_REMOVAL_DELAY);
          return;
        }

      fp_dbg ("Finger removed, reporting status");
      if (!self->deactivating && !self->deactivation_in_progress)
        fpi_ssm_mark_completed (transfer->ssm);
    }
}

/*
 * Removal polling sub-SSM entry point.
 */
static void
poll_removal_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);

  if (self->deactivating || self->deactivation_in_progress)
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case POLL_REMOVAL_DELAY:
      fpi_ssm_next_state_delayed (ssm, CB2000_REMOVAL_INTERVAL);
      break;

    case POLL_REMOVAL_SEND:
      cb2000_ctrl_in (dev, ssm,
                      REQ_POLL, 0x0007, 0, 2,
                      poll_removal_result_cb, NULL);
      break;
    }
}

/* ============================================================================
 * IMAGE READ SUB-SSM
 * ============================================================================ */

static void image_read_run_state (FpiSsm   *ssm,
                                  FpDevice *dev);

/*
 * CLAHE (Contrast Limited Adaptive Histogram Equalization).
 * Enhances local contrast per tile, so ridge/valley boundaries are sharpened
 * without the global flattening that a full-image equalization would cause.
 *
 * Applied on the RAW 80x64 image BEFORE upscale. This is the single most
 * important preprocessing step for NBIS minutiae detection on small sensors:
 * it makes ridges locally contrasty so MINDTCT can compute direction maps.
 *
 * Grid: tiles_x * tiles_y tiles of tile_w * tile_h pixels each.
 * clip_limit controls how much contrast amplification is allowed per tile
 * (higher = more contrast, but also more noise amplification).
 * Bilinear interpolation between neighboring tile histograms avoids
 * visible tile boundary artifacts.
 */
static void
apply_clahe (guint8 *data, gint width, gint height,
             gint tile_w, gint tile_h, double clip_limit)
{
  gint tiles_x = width / tile_w;
  gint tiles_y = height / tile_h;

  if (tiles_x < 2 || tiles_y < 2)
    return;

  gint num_tiles = tiles_x * tiles_y;
  gint tile_size = tile_w * tile_h;

  /* Build per-tile CDF lookup tables */
  guint8 *lut = g_malloc (num_tiles * 256);

  for (gint ty = 0; ty < tiles_y; ty++)
    {
      for (gint tx = 0; tx < tiles_x; tx++)
        {
          gint hist[256] = {0};
          gint tile_idx = ty * tiles_x + tx;

          /* Compute histogram for this tile */
          for (gint dy = 0; dy < tile_h; dy++)
            {
              gint y = ty * tile_h + dy;
              for (gint dx = 0; dx < tile_w; dx++)
                {
                  gint x = tx * tile_w + dx;
                  hist[data[y * width + x]]++;
                }
            }

          /* Clip histogram (redistribute excess above clip threshold) */
          gint clip_threshold = (gint) (clip_limit * tile_size / 256);
          if (clip_threshold < 1)
            clip_threshold = 1;

          gint excess = 0;
          for (gint i = 0; i < 256; i++)
            {
              if (hist[i] > clip_threshold)
                {
                  excess += hist[i] - clip_threshold;
                  hist[i] = clip_threshold;
                }
            }
          /* Redistribute excess evenly */
          gint per_bin = excess / 256;
          gint remainder = excess - per_bin * 256;
          for (gint i = 0; i < 256; i++)
            hist[i] += per_bin;
          /* Spread remainder across bins */
          for (gint i = 0; i < remainder; i++)
            hist[i]++;

          /* Build CDF -> LUT mapping */
          gint cumulative = 0;
          for (gint i = 0; i < 256; i++)
            {
              cumulative += hist[i];
              lut[tile_idx * 256 + i] =
                (guint8) ((cumulative * 255) / tile_size);
            }
        }
    }

  /* Apply with bilinear interpolation between tiles */
  guint8 *out = g_malloc (width * height);

  for (gint y = 0; y < height; y++)
    {
      for (gint x = 0; x < width; x++)
        {
          guint8 pixel = data[y * width + x];

          /* Find which tile center this pixel is nearest to */
          double fx = ((double) x - tile_w * 0.5) / tile_w;
          double fy = ((double) y - tile_h * 0.5) / tile_h;

          gint tx0 = (gint) fx;
          gint ty0 = (gint) fy;

          if (tx0 < 0)
            tx0 = 0;
          if (ty0 < 0)
            ty0 = 0;
          if (tx0 >= tiles_x - 1)
            tx0 = tiles_x - 2;
          if (ty0 >= tiles_y - 1)
            ty0 = tiles_y - 2;

          gint tx1 = tx0 + 1;
          gint ty1 = ty0 + 1;

          double ax = fx - tx0;
          double ay = fy - ty0;
          if (ax < 0.0)
            ax = 0.0;
          if (ax > 1.0)
            ax = 1.0;
          if (ay < 0.0)
            ay = 0.0;
          if (ay > 1.0)
            ay = 1.0;

          /* Bilinear interpolation of 4 neighboring tile LUTs */
          double v00 = lut[(ty0 * tiles_x + tx0) * 256 + pixel];
          double v10 = lut[(ty0 * tiles_x + tx1) * 256 + pixel];
          double v01 = lut[(ty1 * tiles_x + tx0) * 256 + pixel];
          double v11 = lut[(ty1 * tiles_x + tx1) * 256 + pixel];

          double val = v00 * (1 - ax) * (1 - ay) +
                       v10 * ax * (1 - ay) +
                       v01 * (1 - ax) * ay +
                       v11 * ax * ay;

          out[y * width + x] = (guint8) (val + 0.5);
        }
    }

  memcpy (data, out, width * height);
  g_free (out);
  g_free (lut);

  fp_dbg ("CLAHE applied: %dx%d tiles, clip=%.1f",
          tiles_x, tiles_y, clip_limit);
}

/*
 * Smart levels: percentile-based contrast stretch.
 * Clips bottom/top 5% of histogram before stretching, so outlier pixels
 * (stuck pixels, noise spikes) don't compress the useful ridge contrast.
 */
static void
normalize_contrast (guint8 *data, gint width, gint height)
{
  gint size = width * height;
  gint hist[256] = {0};

  for (gint i = 0; i < size; i++)
    hist[data[i]]++;

  /* Find 5th and 95th percentile values */
  gint clip_lo_count = (gint) (size * 0.05);
  gint clip_hi_count = (gint) (size * 0.05);
  guint8 lo = 0, hi = 255;
  gint cumulative = 0;

  for (gint v = 0; v < 256; v++)
    {
      cumulative += hist[v];
      if (cumulative > clip_lo_count)
        {
          lo = (guint8) v;
          break;
        }
    }

  cumulative = 0;
  for (gint v = 255; v >= 0; v--)
    {
      cumulative += hist[v];
      if (cumulative > clip_hi_count)
        {
          hi = (guint8) v;
          break;
        }
    }

  if (hi <= lo)
    return;

  fp_dbg ("Smart levels: lo=%d hi=%d (clipped 5%%/95%%)", lo, hi);

  double range = (double) (hi - lo);
  for (gint i = 0; i < size; i++)
    {
      if (data[i] <= lo)
        data[i] = 0;
      else if (data[i] >= hi)
        data[i] = 255;
      else
        data[i] = (guint8) (((data[i] - lo) * 255.0) / range);
    }
}

/*
 * 3x3 Gaussian blur (sigma ~0.85) to smooth bilinear upscale artifacts.
 * Kernel: [1 2 1; 2 4 2; 1 2 1] / 16
 * Only applied to upscaled images - smooths blocky pixel grid into
 * more natural ridge contours for NBIS.
 */
static void
gaussian_blur_3x3 (guint8 *data, gint width, gint height)
{
  guint8 *tmp = g_malloc (width * height);

  memcpy (tmp, data, width * height);

  for (gint y = 1; y < height - 1; y++)
    {
      for (gint x = 1; x < width - 1; x++)
        {
          gint sum =
            1 * tmp[(y - 1) * width + (x - 1)] +
            2 * tmp[(y - 1) * width + x] +
            1 * tmp[(y - 1) * width + (x + 1)] +
            2 * tmp[y * width + (x - 1)] +
            4 * tmp[y * width + x] +
            2 * tmp[y * width + (x + 1)] +
            1 * tmp[(y + 1) * width + (x - 1)] +
            2 * tmp[(y + 1) * width + x] +
            1 * tmp[(y + 1) * width + (x + 1)];
          data[y * width + x] = (guint8) (sum / 16);
        }
    }

  g_free (tmp);
}

static double
calculate_variance (const guint8 *data, gint width, gint height)
{
  gint size = width * height;
  double mean = 0.0;

  for (gint i = 0; i < size; i++)
    mean += data[i];
  mean /= size;

  double var = 0.0;
  for (gint i = 0; i < size; i++)
    {
      double diff = data[i] - mean;
      var += diff * diff;
    }

  return var / size;
}

static double
calculate_block_low_var_ratio (const guint8 *data,
                               gint          width,
                               gint          height,
                               gint          block_size,
                               double        var_threshold)
{
  gint blocks_x = width / block_size;
  gint blocks_y = height / block_size;
  gint total_blocks = blocks_x * blocks_y;
  gint low_blocks = 0;

  if (total_blocks == 0)
    return 0.0;

  for (gint by = 0; by < blocks_y; by++)
    {
      for (gint bx = 0; bx < blocks_x; bx++)
        {
          double sum = 0.0;
          double sum2 = 0.0;
          for (gint y = 0; y < block_size; y++)
            {
              gint row = (by * block_size + y) * width;
              for (gint x = 0; x < block_size; x++)
                {
                  guint8 v = data[row + (bx * block_size + x)];
                  sum += v;
                  sum2 += (double) v * (double) v;
                }
            }
          double n = (double) block_size * (double) block_size;
          double mean = sum / n;
          double var = (sum2 / n) - (mean * mean);
          if (var < var_threshold)
            low_blocks++;
        }
    }

  return (double) low_blocks / (double) total_blocks;
}

static double
calculate_laplacian_variance (const guint8 *data, gint width, gint height)
{
  double sum = 0.0;
  double sum2 = 0.0;
  gint count = 0;

  if (width < 3 || height < 3)
    return 0.0;

  for (gint y = 1; y < height - 1; y++)
    {
      for (gint x = 1; x < width - 1; x++)
        {
          gint idx = y * width + x;
          gint l = data[idx - width] + data[idx + width] +
                   data[idx - 1] + data[idx + 1] -
                   4 * data[idx];
          sum += l;
          sum2 += (double) l * (double) l;
          count++;
        }
    }

  if (count == 0)
    return 0.0;

  double mean = sum / count;
  return (sum2 / count) - (mean * mean);
}

/*
 * Approximate fingerprint coverage area (0.0..1.0).
 * This is a host-side proxy for the DLL notion of "fingerprint_area":
 * blocks with enough local variance are considered fingerprint foreground.
 */
static double
calculate_fingerprint_area_ratio (const guint8 *data, gint width, gint height)
{
  double low_var_ratio = calculate_block_low_var_ratio (data, width, height,
                                                        CB2000_BLOCK_SIZE,
                                                        CB2000_LOW_VAR_THRESHOLD);

  return CLAMP (1.0 - low_var_ratio, 0.0, 1.0);
}

/*
 * Approximate overlap percentage between consecutive normalized frames.
 * The metric is intentionally simple and only used as a retry heuristic log.
 * Returns -1 when overlap is undefined (no previous foreground data).
 */
static int
calculate_overlap_percent (const guint8 *curr, const guint8 *prev, gint size)
{
  gint curr_fg = 0;
  gint prev_fg = 0;
  gint inter = 0;

  for (gint i = 0; i < size; i++)
    {
      gboolean c = curr[i] > CB2000_FOREGROUND_THRESHOLD;
      gboolean p = prev[i] > CB2000_FOREGROUND_THRESHOLD;
      if (c)
        curr_fg++;
      if (p)
        prev_fg++;
      if (c && p)
        inter++;
    }

  gint denom = MIN (curr_fg, prev_fg);
  if (denom <= 0)
    return -1;

  return (inter * 100) / denom;
}

/*
 * Coarse quality score (0..100) to mirror DLL-style host quality diagnostics.
 * The score blends global variance, focus (Laplacian variance), and area ratio.
 */
static int
calculate_quality_score (double variance_norm, double focus_var, double area_ratio)
{
  double var_n = CLAMP (variance_norm / 5000.0, 0.0, 1.0);
  double focus_n = CLAMP (focus_var / 1500.0, 0.0, 1.0);
  double area_n = CLAMP (area_ratio, 0.0, 1.0);
  double score = (0.40 * var_n + 0.35 * focus_n + 0.25 * area_n) * 100.0;

  return (int) CLAMP (score, 0.0, 100.0);
}

static void
apply_background_subtraction (guint8       *data,
                              const guint8 *bg,
                              gint          width,
                              gint          height)
{
  gint size = width * height;

  for (gint i = 0; i < size; i++)
    {
      if (data[i] > bg[i])
        data[i] = (guint8) (data[i] - bg[i]);
      else
        data[i] = 0;
    }
}

/*
 * Read one image chunk.
 *
 * Windows captures show mixed packet lengths depending on transport mode:
 * - legacy framing: 320-byte chunk carrying 256-byte payload at offset 64
 * - compact framing: mostly 256-byte chunks, with a trailing 259-byte chunk
 *
 * In compact framing, the first chunk can start with a 3-byte marker
 * (typically ff 00 06). When present, those bytes are framing metadata and
 * must be skipped to keep image assembly aligned with Windows.
 *
 * CB2000_CHUNK_PAYLOAD_OFFSET can still force a static offset for experiments.
 */
static void
image_chunk_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);

  if (error)
    {
      fp_warn ("Image chunk read error: %s", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           g_error_new_literal (G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                "Operation cancelled"));
      return;
    }

  guint8 *src = transfer->buffer;
  gsize len = transfer->actual_length;
  int chunk_index = self->chunks_read;
  gboolean first_chunk_header =
    (chunk_index == 0 && len >= 3 &&
     src[0] == 0xff && src[1] == 0x00 && src[2] == 0x06);
  int default_payload_offset;
  gsize payload_offset;
  gsize payload_len;
  gsize remaining;
  gsize to_copy;

  if (len < CB2000_CHUNK_PAYLOAD_SIZE)
    {
      fp_warn ("Chunk %d too short: got %zu, need >= %d",
               self->chunks_read, len, CB2000_CHUNK_PAYLOAD_SIZE);
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  /* Prefer Windows-aligned defaults:
   * - 320-byte framing -> payload at offset 64
   * - first compact chunk with ff 00 06 -> skip 3-byte marker
   * - other compact chunks (including 259-byte tail) -> payload at 0 */
  if (len == CB2000_CHUNK_SIZE)
    default_payload_offset = CB2000_CHUNK_DEFAULT_PAYLOAD_OFFSET;
  else if (first_chunk_header)
    default_payload_offset = 3;
  else
    default_payload_offset = 0;
  payload_offset = (gsize) cb2000_get_env_int ("CB2000_CHUNK_PAYLOAD_OFFSET",
                                               default_payload_offset,
                                               0,
                                               (int) len - 1);
  if (payload_offset >= len)
    {
      fp_warn ("Invalid payload window: off=%zu len=%zu chunk=%zu",
               payload_offset, len, len);
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }
  payload_len = len - payload_offset;

  if (self->chunks_read == 0 && len >= 3)
    fp_dbg ("Chunk 0 prefix=%02x %02x %02x payload_off=%zu payload_len=%zu",
            src[0], src[1], src[2], payload_offset, payload_len);

  remaining = CB2000_IMG_SIZE - self->image_offset;
  to_copy = MIN (payload_len, remaining);

  if (to_copy > 0)
    {
      memcpy (self->image_buffer + self->image_offset,
              src + payload_offset,
              to_copy);
      self->image_offset += to_copy;
    }

  self->chunks_read++;

  fp_dbg ("Chunk %d/%d: read %zu payload_off=%zu payload_len=%zu copied %zu -> total %zu/%d",
          self->chunks_read, CB2000_IMG_CHUNKS,
          transfer->actual_length, payload_offset, payload_len, to_copy,
          self->image_offset, CB2000_IMG_SIZE);

  if (self->chunks_read >= CB2000_IMG_CHUNKS)
    {
      fp_dbg ("Image complete: %d chunks read", CB2000_IMG_CHUNKS);
      fpi_ssm_mark_completed (transfer->ssm);
    }
  else
    {
      fpi_ssm_next_state (transfer->ssm);
    }
}

/*
 * Write padding between chunk reads. This mirrors Windows behavior and keeps
 * the device in sync.
 */
static void
image_padding_cb (FpiUsbTransfer *transfer,
                  FpDevice       *dev,
                  gpointer        user_data,
                  GError         *error)
{
  if (error)
    {
      fp_warn ("Padding write error: %s", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           g_error_new_literal (G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                "Operation cancelled"));
      return;
    }

  fpi_ssm_jump_to_state (transfer->ssm, IMG_READ_CHUNK);
}

/*
 * Image read sub-SSM entry point. Alternates between read and padding writes.
 */
static void
image_read_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case IMG_READ_CHUNK:
      cb2000_bulk_read (dev, ssm, CB2000_CHUNK_SIZE, image_chunk_cb, NULL);
      break;

    case IMG_WRITE_PADDING:
      cb2000_bulk_write (dev, ssm, data_padding, CB2000_CHUNK_PAYLOAD_SIZE,
                         image_padding_cb, NULL);
      break;
    }
}

/* ============================================================================
 * NCC CORRELATION MATCHER
 * ============================================================================
 * Implemented in drivers/canvasbio-ncc.c and declared in drivers/canvasbio-ncc.h
 */

/* ============================================================================
 * GVARIANT PRINT STORAGE (V33)
 * ============================================================================
 * Enrollment stores preprocessed 80x64 images in GVariant format.
 * Format: (uqqay) = (version, width, height, concatenated_images)
 */

/**
 * pack_enrollment_data - Store enrollment images into a print
 * @self: Device instance with enrollment images
 * @print: FpPrint to store data into
 *
 * Packs all enrollment images into a GVariant and sets it on the print.
 */
static void
pack_enrollment_data (FpiDeviceCanvasbio *self, FpPrint *print)
{
  gsize total_size = CB2000_NR_ENROLL_STAGES * CB2000_IMG_SIZE;
  guint8 *all_images = g_malloc (total_size);
  gint i;
  GVariant *images_var;
  GVariant *data;

  for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
    memcpy (all_images + i * CB2000_IMG_SIZE,
            self->enroll_images[i], CB2000_IMG_SIZE);

  images_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                          all_images, total_size, 1);

  data = g_variant_new (CB2000_PRINT_FORMAT,
                        (guint32) 1,
                        (guint16) CB2000_IMG_WIDTH,
                        (guint16) CB2000_IMG_HEIGHT,
                        images_var);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", data, NULL);

  g_free (all_images);

  fp_info ("Packed %d enrollment images (%zu bytes total)",
           CB2000_NR_ENROLL_STAGES, total_size);
}

/**
 * unpack_and_match - Unpack enrolled images and run robust NCC telemetry
 * @print: Enrolled FpPrint containing stored images
 * @captured: Captured image to match against (CB2000_IMG_SIZE bytes)
 * @tel: Output telemetry for decision matrix
 *
 * Returns TRUE if unpack succeeded, FALSE on format error.
 */
static gboolean
unpack_and_match (FpPrint *print, const guint8 *captured, Cb2000NccTelemetry *tel)
{
  g_autoptr(GVariant) print_data = NULL;
  g_autoptr(GVariant) images_var = NULL;
  gdouble captured_grad[CB2000_IMG_SIZE];
  guint8 captured_mask[CB2000_IMG_SIZE];
  gdouble top_scores[CB2000_NR_ENROLL_STAGES] = {0};
  guint32 version;
  guint16 width, height;
  const guint8 *images_data;
  gsize images_len;
  guint mask_threshold;
  guint min_samples;
  guint support_min;
  gint max_shift;
  gdouble support_int;
  gdouble support_grad;
  gdouble support_int_effective;
  gdouble support_grad_effective;
  gint n_images;
  gint i;
  guint top_n;

  g_object_get (print, "fpi-data", &print_data, NULL);
  if (!print_data)
    {
      fp_warn ("No print data found");
      return FALSE;
    }

  if (!g_variant_check_format_string (print_data, CB2000_PRINT_FORMAT, FALSE))
    {
      fp_warn ("Invalid print data format");
      return FALSE;
    }

  g_variant_get (print_data, CB2000_PRINT_FORMAT,
                 &version, &width, &height, &images_var);

  if (version != 1 || width != CB2000_IMG_WIDTH || height != CB2000_IMG_HEIGHT)
    {
      fp_warn ("Print data version/size mismatch: v=%u w=%u h=%u",
               version, width, height);
      return FALSE;
    }

  images_data = g_variant_get_fixed_array (images_var, &images_len, 1);
  n_images = images_len / CB2000_IMG_SIZE;

  if (n_images < 1 || images_len % CB2000_IMG_SIZE != 0)
    {
      fp_warn ("Invalid image data length: %zu", images_len);
      return FALSE;
    }

  memset (tel, 0, sizeof (*tel));
  tel->best_intensity = -1.0;
  tel->second_intensity = -1.0;
  tel->mean_top3_intensity = -1.0;
  tel->grad_for_best_intensity = 0.0;

  mask_threshold = cb2000_ncc_env_uint ("CB2000_NCC_MASK_THRESH",
                                        CB2000_NCC_MASK_THRESHOLD_DEFAULT,
                                        1, 254);
  min_samples = cb2000_ncc_env_uint ("CB2000_NCC_MIN_SAMPLES",
                                     CB2000_NCC_MIN_SAMPLES_DEFAULT,
                                     64, CB2000_IMG_SIZE);
  support_min = cb2000_ncc_env_uint ("CB2000_NCC_SUPPORT_MIN",
                                     CB2000_NCC_SUPPORT_MIN_DEFAULT,
                                     1, CB2000_NR_ENROLL_STAGES);
  support_int = cb2000_ncc_env_double ("CB2000_NCC_SUPPORT_INT",
                                       CB2000_NCC_SUPPORT_INT_DEFAULT,
                                       0.0, 1.0);
  support_grad = cb2000_ncc_env_double ("CB2000_NCC_SUPPORT_GRAD",
                                        CB2000_NCC_SUPPORT_GRAD_DEFAULT,
                                        0.0, 1.0);
  max_shift = (gint) cb2000_ncc_env_uint ("CB2000_NCC_MAX_SHIFT",
                                          CB2000_NCC_MAX_SHIFT_DEFAULT,
                                          0, 16);

  cb2000_ncc_compute_enroll_cohesion (images_data, n_images,
                                      (guint8) mask_threshold,
                                      min_samples,
                                      max_shift,
                                      tel);
  tel->adaptive_relax = cb2000_ncc_compute_adaptive_relax (tel);
  support_int_effective = MAX (0.0, support_int - (tel->adaptive_relax * 0.50));
  support_grad_effective = MAX (0.0, support_grad - (tel->adaptive_relax * 0.35));

  fp_info ("[ NCC ] enroll cohesion: pairs=%u mean_int=%.4f min_int=%.4f mean_grad=%.4f relax=%.4f",
           tel->enroll_pair_count,
           tel->enroll_pair_mean_intensity,
           tel->enroll_pair_min_intensity,
           tel->enroll_pair_mean_gradient,
           tel->adaptive_relax);

  cb2000_build_foreground_mask (captured, captured_mask,
                                CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                (guint8) mask_threshold);
  cb2000_compute_gradient_magnitude (captured, captured_grad,
                                     CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);

  gdouble best_fused_score = -4.0;
  gdouble grad_for_best_fused = 0.0;
  gdouble best_grad_supported = 0.0;
  gdouble grad_for_best_intensity = 0.0;

  for (i = 0; i < n_images; i++)
    {
      const guint8 *enrolled = images_data + i * CB2000_IMG_SIZE;
      gdouble enrolled_grad[CB2000_IMG_SIZE];
      guint8 enrolled_mask[CB2000_IMG_SIZE];
      gdouble int_score = 0.0;
      gdouble grad_score = 0.0;
      gdouble fused_score;

      cb2000_build_foreground_mask (enrolled, enrolled_mask,
                                    CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                    (guint8) mask_threshold);
      cb2000_compute_gradient_magnitude (enrolled, enrolled_grad,
                                         CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);

      cb2000_ncc_match_joint (captured, enrolled,
                              captured_grad, enrolled_grad,
                              captured_mask, enrolled_mask,
                              CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                              max_shift, min_samples,
                              &int_score, &grad_score);

      top_scores[i] = int_score;
      tel->compared_images++;

      fused_score = int_score + grad_score;
      if (fused_score > best_fused_score)
        {
          best_fused_score = fused_score;
          grad_for_best_fused = grad_score;
        }

      if (int_score > tel->best_intensity)
        {
          tel->second_intensity = tel->best_intensity;
          tel->best_intensity = int_score;
          grad_for_best_intensity = grad_score;
        }
      else if (int_score > tel->second_intensity)
        {
          tel->second_intensity = int_score;
        }

      if (int_score >= support_int_effective && grad_score >= support_grad_effective)
        {
          tel->support_count++;
          if (grad_score > best_grad_supported)
            best_grad_supported = grad_score;
        }

      fp_dbg ("[ NCC ] gallery=%d int=%.4f grad=%.4f fused=%.4f",
              i, int_score, grad_score, fused_score);
    }

  {
    gdouble chosen_grad = grad_for_best_intensity;
    if (grad_for_best_fused > chosen_grad)
      chosen_grad = grad_for_best_fused;
    if (best_grad_supported > chosen_grad)
      chosen_grad = best_grad_supported;
    tel->grad_for_best_intensity = chosen_grad;
  }

  top_n = MIN ((guint) n_images, (guint) 3);
  for (i = 0; i < n_images; i++)
    {
      gint j;
      for (j = i + 1; j < n_images; j++)
        {
          if (top_scores[j] > top_scores[i])
            {
              gdouble tmp = top_scores[i];
              top_scores[i] = top_scores[j];
              top_scores[j] = tmp;
            }
        }
    }
  if (top_n > 0)
    {
      gdouble sum = 0.0;
      for (i = 0; i < (gint) top_n; i++)
        sum += top_scores[i];
      tel->mean_top3_intensity = sum / top_n;
    }
  else
    {
      tel->mean_top3_intensity = 0.0;
    }

  if (n_images >= 1)
    tel->best_intensity = MAX (0.0, top_scores[0]);
  if (n_images >= 2)
    tel->second_intensity = MAX (0.0, top_scores[1]);
  else
    tel->second_intensity = 0.0;

  g_message ("[ NCC ] top1=%.4f top2=%.4f mean3=%.4f grad1=%.4f support=%u/%u "
             "mask_thresh=%u min_samples=%u max_shift=%d support_min=%u "
             "support_int=%.3f(%.3f) support_grad=%.3f(%.3f) adapt=%.3f",
             tel->best_intensity,
             tel->second_intensity,
             tel->mean_top3_intensity,
             tel->grad_for_best_intensity,
             tel->support_count,
             tel->compared_images,
             mask_threshold,
             min_samples,
             max_shift,
             support_min,
             support_int,
             support_int_effective,
             support_grad,
             support_grad_effective,
             tel->adaptive_relax);

  return TRUE;
}

/* ============================================================================
 * MASTER CYCLE SSM
 * ============================================================================
 *
 * Complete capture cycle matching Windows 1:1:
 *   HARD_RESET -> INIT_CONFIG -> SETTLE_DELAY -> WAIT_FINGER ->
 *   CAPTURE_START -> READ_IMAGE -> FINALIZE -> VERIFY_RESULT_STATUS ->
 *   VERIFY_READY_QUERY -> VERIFY_READY_DECIDE -> SUBMIT_IMAGE ->
 *   WAIT_REMOVAL -> REARM -> [cycle_complete -> start_new_cycle]
 */

static void cycle_run_state (FpiSsm   *ssm,
                             FpDevice *dev);

/* Disabled: early placement delay (kept for reference, Windows parity). */
#if 0
static gboolean
early_placement_delay_cb (gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDevice *dev = fpi_ssm_get_device (ssm);
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  self->early_placement_detected = FALSE;

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return G_SOURCE_REMOVE;
    }

  fpi_ssm_jump_to_state (ssm, CYCLE_WAIT_FINGER);
  return G_SOURCE_REMOVE;
}
#endif

/*
 * Master cycle state machine. This is the authoritative control flow for
 * capture, and is designed to stay as close to Windows behavior as possible.
 */

static void
hard_reset_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  FpDevice *dev = FP_DEVICE (source_object);
  GUsbDevice *usb = fpi_device_get_usb_device (dev);
  GError *err = NULL;

  /* Release interface before reset */
  g_usb_device_release_interface (usb, 0, 0, &err);
  g_clear_error (&err);

  /* USB device reset */
  g_usb_device_reset (usb, &err);
  if (err)
    {
      fp_warn ("USB reset: %s", err->message);
      g_clear_error (&err);
    }

  /* Reclaim interface */
  if (!g_usb_device_claim_interface (usb, 0,
                                     G_USB_DEVICE_CLAIM_INTERFACE_NONE, &err))
    {
      g_task_return_error (task, err);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
hard_reset_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  GError *err = NULL;

  if (!g_task_propagate_boolean (G_TASK (res), &err))
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
cycle_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);

  /* Check deactivation and cancellation at every state entry */
  if (self->deactivating || self->deactivation_in_progress || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {

    case CYCLE_RECOVERY:
      if (self->force_recovery)
        fp_warn ("Cycle: RECOVERY (hard reset + re-init)");
      else
        fp_dbg ("Cycle: RECOVERY (no-op)");
      fpi_ssm_next_state (ssm);
      break;

    case CYCLE_HARD_RESET:
      {
        if (self->initial_activation_done && !self->force_recovery)
          {
            fp_dbg ("Cycle: HARD_RESET skipped (rearm mode)");
            fpi_ssm_jump_to_state (ssm, CYCLE_WAIT_FINGER);
            break;
          }

        fp_dbg ("Cycle: HARD_RESET (USB reset + reclaim in background)");
        GTask *task = g_task_new (dev, NULL, hard_reset_cb, ssm);
        g_task_run_in_thread (task, hard_reset_thread);
        g_object_unref (task);
        break;
      }

    case CYCLE_INIT_CONFIG:
      fp_dbg ("Cycle: INIT_CONFIG (activation sub-SSM)");
      {
        FpiSsm *subsm = fpi_ssm_new (dev, activate_run_state,
                                     STATE_ACTIVATE_NUM_STATES);
        fpi_ssm_start_subsm (ssm, subsm);
      }
      break;

    case CYCLE_SETTLE_DELAY:
      if (!self->initial_activation_done)
        self->initial_activation_done = TRUE;
      if (self->force_recovery)
        self->force_recovery = FALSE;
      fp_dbg ("Cycle: SETTLE_DELAY (%dms)", CB2000_WAKEUP_DELAY);
      fpi_ssm_next_state_delayed (ssm, CB2000_WAKEUP_DELAY);
      break;

    case CYCLE_WAIT_FINGER:
      fp_dbg ("Cycle: WAIT_FINGER (polling sub-SSM, interval=%dms)",
              CB2000_POLL_INTERVAL);
      /* Reset counters for a new detection window. */
      self->poll_stable_hits = 0;
      self->no_finger_streak = 0;
      self->poll_total_count = 0;
      self->early_placement_detected = FALSE;
      self->poll_start_us = g_get_monotonic_time ();
      {
        FpiSsm *subsm = fpi_ssm_new (dev, poll_finger_run_state,
                                     POLL_FINGER_NUM);
        fpi_ssm_start_subsm (ssm, subsm);
      }
      break;

    case CYCLE_EARLY_PLACEMENT_DELAY:
      /* Disabled for Windows parity (no early-placement delay). */
      fpi_ssm_next_state (ssm);
      break;

    case CYCLE_CAPTURE_START:
      {
        FpiDeviceAction action = fpi_device_get_current_action (dev);
        const char *seq_name = cb2000_capture_start_seq_name_for_action (action);
        const Cb2000Command *seq_cmds = cb2000_capture_start_cmds_for_action (action);
        const char *phase_name = cb2000_is_verify_phase_action (action) ?
                                 "verify_like" :
                                 "capture_enroll";


        fp_info ("[ SSM_ROUTE ] action=%s phase=%s state=CAPTURE_START seq=%s",
                 cb2000_action_label (action), phase_name, seq_name);
        run_command_sequence (ssm, dev, seq_name, seq_cmds);
        break;
      }

    case CYCLE_READ_IMAGE:
      fp_dbg ("Cycle: READ_IMAGE (20 chunks)");
      self->chunks_read = 0;
      self->image_offset = 0;
      memset (self->image_buffer, 0, CB2000_IMG_SIZE);
      {
        FpiSsm *subsm = fpi_ssm_new (dev, image_read_run_state,
                                     IMG_READ_NUM);
        fpi_ssm_start_subsm (ssm, subsm);
      }
      break;

    case CYCLE_FINALIZE:
      {
        FpiDeviceAction action = fpi_device_get_current_action (dev);
        const char *seq_name = cb2000_capture_finalize_seq_name_for_action (action);
        const Cb2000Command *seq_cmds = cb2000_capture_finalize_cmds_for_action (action);
        const char *phase_name = cb2000_is_verify_phase_action (action) ?
                                 "verify_like" :
                                 "capture_enroll";

        fp_info ("[ SSM_ROUTE ] action=%s phase=%s state=FINALIZE seq=%s",
                 cb2000_action_label (action), phase_name, seq_name);
        run_command_sequence (ssm, dev, seq_name, seq_cmds);
        break;
      }

    case CYCLE_SUBMIT_IMAGE:
      fp_dbg ("Cycle: SUBMIT_IMAGE (got %zu/%d bytes)",
              self->image_offset, CB2000_IMG_SIZE);


      /* Create and submit image to libfprint for MoH processing. */
      {
        FpImage *img = fp_image_new (CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
        img->ppmm = cb2000_get_ppmm ();

        gsize copy_len = MIN (self->image_offset, CB2000_IMG_SIZE);
        memcpy (img->data, self->image_buffer, copy_len);

        if (copy_len < CB2000_IMG_SIZE)
          memset (img->data + copy_len, 0x00,
                  CB2000_IMG_SIZE - copy_len);

        double variance = calculate_variance (img->data,
                                              CB2000_IMG_WIDTH,
                                              CB2000_IMG_HEIGHT);
        double low_var_ratio = calculate_block_low_var_ratio (
          img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
          CB2000_BLOCK_SIZE, CB2000_LOW_VAR_THRESHOLD);
        double focus_var = calculate_laplacian_variance (
          img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
        fp_info ("Image variance: %.1f low_var_ratio=%.2f focus=%.1f",
                 variance, low_var_ratio, focus_var);
        fp_info ("Detect reg51=%s, Capture reg51=%s",
                 CB2000_USE_ALT_DETECT_51 ? "0x68" : "0xA8",
                 "0x88");

        FpiDeviceAction action = fpi_device_get_current_action (dev);
        gboolean use_bg_subtract = cb2000_should_apply_bg_subtraction (action);

        if (cb2000_is_verify_phase_action (action))
          {
            /* stripped log */
          }
        else
          {
            fp_dbg ("[ SSM_ROUTE ] action=%s phase=capture_enroll state=SUBMIT_IMAGE route=HOST_PREPROCESS",
                    cb2000_action_label (action));
          }

        /*
         * Background bootstrap is useful for capture/enroll only.
         * In verify/identify, Windows parity relies on finalize ACK routing,
         * so a low-variance frame should not be re-labeled as background.
         */
        if (!cb2000_is_verify_phase_action (action) &&
            !self->background_valid &&
            variance < CB2000_BG_CAPTURE_MAX_VARIANCE)
          {
            memcpy (self->background_buffer, img->data, CB2000_IMG_SIZE);
            self->background_valid = TRUE;
            fp_info ("Background captured (variance %.1f) - retrying capture",
                     variance);
            g_object_unref (img);
            cb2000_retry_scan_with_cause (dev,
                                          CB2000_RETRY_BACKGROUND_CAPTURE,
                                          "(capture background)");
            fpi_ssm_next_state (ssm);
            return;
          }

        if (self->background_valid && use_bg_subtract)
          {
            apply_background_subtraction (img->data,
                                          self->background_buffer,
                                          CB2000_IMG_WIDTH,
                                          CB2000_IMG_HEIGHT);
          }

        /* CLAHE: local contrast enhancement on raw image before upscale.
         * This is the key preprocessing for NBIS on small sensors. */
        apply_clahe (img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                     CB2000_CLAHE_TILE_W, CB2000_CLAHE_TILE_H,
                     CB2000_CLAHE_CLIP_LIMIT);

        normalize_contrast (img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
        /* Polarity: keep raw sensor orientation (white ridges on black). */
        /* invert_image_u8(img->data, CB2000_IMG_SIZE); */

        double variance_norm = calculate_variance (img->data,
                                                   CB2000_IMG_WIDTH,
                                                   CB2000_IMG_HEIGHT);
        double low_var_ratio_norm = calculate_block_low_var_ratio (
          img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
          CB2000_BLOCK_SIZE, CB2000_LOW_VAR_THRESHOLD);
        fp_info ("Image variance (normalized): %.1f low_var_ratio=%.2f",
                 variance_norm, low_var_ratio_norm);

        const int area_min_pct = cb2000_get_area_min_pct (action);

        double area_ratio_norm = calculate_fingerprint_area_ratio (
          img->data, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
        int area_pct = (int) (area_ratio_norm * 100.0 + 0.5);
        int overlap_pct = -1;
        if (self->previous_norm_valid && self->previous_norm_buffer)
          {
            overlap_pct = calculate_overlap_percent (img->data,
                                                     self->previous_norm_buffer,
                                                     CB2000_IMG_SIZE);
          }
        int quality_score = calculate_quality_score (variance_norm,
                                                     focus_var,
                                                     area_ratio_norm);
        fp_info ("[ IMAGE ] quality = %d, area = %d, overlap = %d",
                 quality_score, area_pct, overlap_pct);

        gboolean fail_area = (area_pct < area_min_pct);
        gboolean fail_variance = (variance_norm < CB2000_MIN_IMAGE_VARIANCE);
        gboolean fail_focus = (focus_var < CB2000_MIN_FOCUS_VARIANCE);

        if (fail_area)
          fp_info ("Reject! quality.fingerprint_area %d < %d (threshold)",
                   area_pct, area_min_pct);

        if (fail_variance || fail_area || fail_focus)
          {
            fp_info ("Reject! Image quality too bad. var=%.1f area=%d focus=%.1f overlap=%d",
                     variance_norm, area_pct, focus_var, overlap_pct);
            fp_info ("[ GATE ] action=%s min_area=%d fail_var=%d fail_area=%d fail_focus=%d",
                     cb2000_action_label (action), area_min_pct,
                     fail_variance, fail_area, fail_focus);
            if (self->previous_norm_buffer)
              {
                memcpy (self->previous_norm_buffer, img->data, CB2000_IMG_SIZE);
                self->previous_norm_valid = TRUE;
              }
            g_object_unref (img);
            /* Avoid misleading "finger not centered" when failure is just quality/coverage. */
            cb2000_retry_scan_with_cause (dev,
                                          fail_area ?
                                          CB2000_RETRY_AREA_GATE :
                                          CB2000_RETRY_QUALITY_GATE,
                                          "(quality gate)");
            fpi_ssm_next_state (ssm);
            return;
          }

        /* Let libfprint invert for matching (see fp-image.c). */
        img->flags = FPI_IMAGE_COLORS_INVERTED;

        /* Optional upscale to improve NBIS minutiae extraction on small images. */
        int upscale = cb2000_get_upscale_factor (action);
        fp_dbg ("Preprocess profile: action=%s bg_subtract=%d upscale=%dx",
                cb2000_action_label (action), use_bg_subtract, upscale);
        if (upscale > 1)
          {
            FpImage *resized = fpi_image_resize (img, upscale, upscale);
            if (resized)
              {
                resized->flags = img->flags;
                resized->ppmm = img->ppmm * upscale;
                /* Smooth bilinear upscale artifacts for NBIS ridge detection. */
                gaussian_blur_3x3 (resized->data, resized->width,
                                   resized->height);
                g_object_unref (img);
                img = resized;
              }
          }


        fp_info ("Captured fingerprint: %zu bytes (%dx%d)",
                 self->image_offset, img->width, img->height);

        if (self->previous_norm_buffer)
          {
            memcpy (self->previous_norm_buffer, img->data, CB2000_IMG_SIZE);
            self->previous_norm_valid = TRUE;
          }

        gboolean precheck_enabled = cb2000_should_precheck_minutiae (action);
        fp_dbg ("Precheck profile: action=%s precheck=%d",
                cb2000_action_label (action), precheck_enabled);

        if (precheck_enabled)
          {
            Cb2000MinutiaePrecheckCtx *ctx = g_new0 (Cb2000MinutiaePrecheckCtx, 1);
            ctx->dev = dev;
            ctx->ssm = ssm;
            ctx->img = img;
            ctx->action = action;
            fp_image_detect_minutiae (img, NULL, cb2000_minutiae_precheck_cb, ctx);
            return;
          }

        cb2000_update_submit_telemetry (self, dev, action);
        /* Action-specific handling (V33 FpDevice + NCC) */
        {
          if (action == FPI_DEVICE_ACTION_CAPTURE)
            {
              fpi_device_capture_complete (dev, img, NULL);
              fpi_ssm_mark_completed (ssm);
              return;

            }
          else if (action == FPI_DEVICE_ACTION_ENROLL)
            {
              guint8 *norm = g_malloc (CB2000_IMG_SIZE);
              memcpy (norm, img->data, CB2000_IMG_SIZE);
              normalize_contrast (norm, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
              g_object_unref (img);

              if (self->enroll_stage > 0 &&
                  cb2000_ncc_env_uint ("CB2000_NCC_ENROLL_GATE",
                                       CB2000_NCC_ENROLL_GATE_ENABLED_DEFAULT, 0, 1))
                {
                  gdouble g_best_int = 0.0, g_best_grad = 0.0, g_min_int = 0.0;
                  gboolean g_ok = cb2000_ncc_enroll_stage_check (
                    norm,
                    (const guint8 * const *) self->enroll_images,
                    self->enroll_stage,
                    (guint8) cb2000_ncc_env_uint ("CB2000_NCC_MASK_THRESH",
                                                  CB2000_NCC_MASK_THRESHOLD_DEFAULT,
                                                  1, 254),
                    cb2000_ncc_env_uint ("CB2000_NCC_MIN_SAMPLES",
                                         CB2000_NCC_MIN_SAMPLES_DEFAULT,
                                         64, CB2000_IMG_SIZE),
                    (gint) cb2000_ncc_env_uint ("CB2000_NCC_MAX_SHIFT",
                                                CB2000_NCC_MAX_SHIFT_DEFAULT,
                                                0, 16),
                    &g_best_int, &g_best_grad, &g_min_int);

                  g_message ("[ NCC ] enroll gate stage=%d best_int=%.4f best_grad=%.4f min_int=%.4f rejects=%u -> %s",
                             self->enroll_stage, g_best_int, g_best_grad, g_min_int,
                             self->enroll_gate_rejects, g_ok ? "ACCEPT" : "REJECT");

                  if (!g_ok)
                    {
                      self->enroll_gate_rejects++;

                      if (self->enroll_stage == 1 && self->enroll_gate_rejects >= 3)
                        {
                          /* The anchor (stage 0) is likely a poor/partial touch:
                           * replace it with the latest sample and keep going. */
                          fp_info ("[ NCC ] enroll gate: replacing anchor stage 0");
                          g_clear_pointer (&self->enroll_images[0], g_free);
                          self->enroll_images[0] = norm;
                          self->enroll_gate_rejects = 0;
                          cb2000_retry_scan_with_cause (dev,
                                                        CB2000_RETRY_ENROLL_CONSISTENCY,
                                                        "(anchor replaced)");
                          fpi_ssm_next_state (ssm);
                          return;
                        }

                      if (self->enroll_gate_rejects < CB2000_NCC_ENROLL_GATE_MAX_REJECTS)
                        {
                          g_free (norm);
                          cb2000_retry_scan_with_cause (dev,
                                                        CB2000_RETRY_ENROLL_CONSISTENCY,
                                                        "(no overlap with gallery)");
                          fpi_ssm_next_state (ssm);
                          return;
                        }

                      fp_warn ("[ NCC ] enroll gate: accepting stage %d after %u rejects",
                               self->enroll_stage, self->enroll_gate_rejects);
                    }
                }

              self->enroll_gate_rejects = 0;
              g_clear_pointer (&self->enroll_images[self->enroll_stage], g_free);
              self->enroll_images[self->enroll_stage] = norm;
              self->enroll_stage++;

              fp_info ("Enrollment stage %d/%d complete",
                       self->enroll_stage, CB2000_NR_ENROLL_STAGES);

              if (self->enroll_stage >= CB2000_NR_ENROLL_STAGES)
                {
                  fpi_ssm_mark_completed (ssm);
                }
              else
                {
                  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
                  fpi_ssm_next_state (ssm);      /* -> WAIT_REMOVAL -> REARM -> new cycle */
                }
              return;

            }
          else if (action == FPI_DEVICE_ACTION_VERIFY ||
                   action == FPI_DEVICE_ACTION_IDENTIFY)
            {
              FpPrint *enrolled = NULL;
              guint8 norm[CB2000_IMG_SIZE];
              Cb2000NccTelemetry ncc_tel;
              Cb2000NccDecision ncc_decision = CB2000_NCC_DECISION_ERROR;

              memcpy (norm, img->data, CB2000_IMG_SIZE);
              normalize_contrast (norm, CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT);
              g_object_unref (img);

              fpi_device_get_verify_data (dev, &enrolled);
              if (enrolled && unpack_and_match (enrolled, norm, &ncc_tel))
                {
                  ncc_decision = cb2000_ncc_classify (&ncc_tel);
                  switch (ncc_decision)
                    {
                    case CB2000_NCC_DECISION_MATCH:
                      self->verify_result = FPI_MATCH_SUCCESS;
                      break;

                    case CB2000_NCC_DECISION_NO_MATCH:
                      self->verify_result = FPI_MATCH_FAIL;
                      break;

                    case CB2000_NCC_DECISION_RETRY:
                      self->verify_result = FPI_MATCH_ERROR;
                      break;

                    case CB2000_NCC_DECISION_ERROR:
                    default:
                      self->verify_result = FPI_MATCH_ERROR;
                      break;
                    }
                }
              else
                {
                  self->verify_result = FPI_MATCH_ERROR;
                }

              g_message ("[ NCC ] decision=%s -> result=%s",
                         cb2000_ncc_decision_label (ncc_decision),
                         self->verify_result == FPI_MATCH_SUCCESS ? "MATCH" :
                         self->verify_result == FPI_MATCH_FAIL ? "NO MATCH" : "ERROR");

              fpi_ssm_mark_completed (ssm);
              return;

            }
          else
            {
              g_object_unref (img);
              fpi_ssm_next_state (ssm);
              return;
            }
        }
      }
      break;

    case CYCLE_WAIT_REMOVAL:
      fp_dbg ("Cycle: WAIT_REMOVAL (polling sub-SSM)");
      self->removal_poll_count = 0;
      self->removal_stable_off_hits = 0;
      self->removal_start_us = g_get_monotonic_time ();
      {
        FpiSsm *subsm = fpi_ssm_new (dev, poll_removal_run_state,
                                     POLL_REMOVAL_NUM);
        fpi_ssm_start_subsm (ssm, subsm);
      }
      break;

    case CYCLE_REARM:
      fp_dbg ("Cycle: REARM (soft reset + detection rearm)");
      run_command_sequence (ssm, dev, "rearm", rearm_cmds);
      break;
    }
}

/*
 * Cycle completion handler. Cancels any pending timers, handles recovery,
 * and starts the next cycle unless deactivation is in progress.
 */
static void
cycle_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);

  self->cycle_ssm = NULL;

  /* Cancel settle timer if pending */

  FpiDeviceAction action = fpi_device_get_current_action (dev);

  /* Check for cancellation */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      (action != FPI_DEVICE_ACTION_NONE && fpi_device_action_is_cancelled (dev)) ||
      self->deactivating)
    {
      fp_info ("Cycle cancelled");
      if (action != FPI_DEVICE_ACTION_NONE)
        {
          if (!error)
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
          fpi_device_action_error (dev, error);
        }
      else
        {
          g_clear_error (&error);
        }
      return;
    }

  if (error)
    {
      fp_warn ("Cycle failed: %s", error->message);
      if (action != FPI_DEVICE_ACTION_NONE)
        fpi_device_action_error (dev, error);
      else
        g_clear_error (&error);
      return;
    }

  /* stripped log */
  fp_dbg ("Cycle complete, checking action-specific completion");

  {
    /* Action-specific completion for FpDevice */
    if (action == FPI_DEVICE_ACTION_ENROLL)
      {
        if (self->enroll_stage >= CB2000_NR_ENROLL_STAGES)
          {
            /* All stages done - pack and complete */
            FpPrint *print = NULL;
            fpi_device_get_enroll_data (dev, &print);
            pack_enrollment_data (self, print);
            fp_info ("Enrollment complete (%d stages)", CB2000_NR_ENROLL_STAGES);
            fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
            return;
          }
        /* More stages: fall through to start_new_cycle */
      }
    else if (action == FPI_DEVICE_ACTION_VERIFY ||
             action == FPI_DEVICE_ACTION_IDENTIFY)
      {
        

        if (self->verify_result == FPI_MATCH_ERROR)
          {
            GError *err = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
            fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, err);
          }
        else
          {
            fpi_device_verify_report (dev, self->verify_result, NULL, NULL);
          }
        fpi_device_verify_complete (dev, NULL);
        return;
      }
    else if (action == FPI_DEVICE_ACTION_CAPTURE)
      {
        
        /* capture_complete already called in SUBMIT_IMAGE success path. */
        return;
      }
  }

  fp_dbg ("Starting next cycle");
  start_new_cycle (dev);
}

/*
 * Start a fresh master cycle SSM if the device is active.
 */
static void
start_new_cycle (FpDevice *dev)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  if (action == FPI_DEVICE_ACTION_NONE)
    {
      fp_dbg ("start_new_cycle: no active action, ignoring");
      return;
    }

  if (self->deactivating || fpi_device_action_is_cancelled (dev))
    {
      fp_info ("start_new_cycle: action is cancelled/deactivating, completing");
      fpi_device_action_error (dev,
                               g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_CANCELLED,
                                                    "Operation cancelled"));
      return;
    }

  if (self->cycle_ssm != NULL)
    return;

  self->cycle_ssm = fpi_ssm_new (dev, cycle_run_state,
                                 CYCLE_NUM_STATES);
  fpi_ssm_start (self->cycle_ssm, cycle_complete);
}

/* ============================================================================
 * DEVICE OPERATIONS
 * ============================================================================ */

/*
 * Open the USB device, claim interface, and initialize driver state.
 */
static void
dev_open (FpDevice *device)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (device);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);
  GError *error = NULL;

  fp_info ("Opening CanvasBio CB2000 device");

  if (!g_usb_device_set_configuration (usb_dev, 1, &error))
    {
      fp_dbg ("set_configuration: %s (may be OK if already configured)",
              error ? error->message : "unknown");
      g_clear_error (&error);
    }

  if (!g_usb_device_claim_interface (usb_dev, 0,
                                     G_USB_DEVICE_CLAIM_INTERFACE_NONE,
                                     &error))
    {
      fp_err ("Failed to claim USB interface: %s", error->message);
      fpi_device_open_complete (device, error);
      return;
    }

  self->image_buffer = g_malloc0 (CB2000_IMG_SIZE);
  self->background_buffer = g_malloc0 (CB2000_IMG_SIZE);
  self->previous_norm_buffer = g_malloc0 (CB2000_IMG_SIZE);
  self->background_valid = FALSE;
  self->previous_norm_valid = FALSE;
  self->deactivating = FALSE;
  self->deactivation_in_progress = FALSE;
  self->image_offset = 0;
  self->chunks_read = 0;
  self->poll_stable_hits = 0;
  self->no_finger_streak = 0;
  self->removal_poll_count = 0;
  self->removal_stable_off_hits = 0;
  self->poll_total_count = 0;
  self->early_placement_detected = FALSE;
  self->poll_start_us = 0;
  self->removal_start_us = 0;
  self->cycle_ssm = NULL;



  self->force_recovery = FALSE;
  fp_dbg ("Device opened successfully");
  fpi_device_open_complete (device, NULL);
}

/*
 * Release USB interface and free resources.
 */
static void
dev_close_transfer_cb (FpiUsbTransfer *transfer,
                       FpDevice       *device,
                       gpointer        user_data,
                       GError         *error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);
  GError *release_error = NULL;

  if (error)
    {
      fp_warn ("Deactivation during close failed: %s", error->message);
      g_error_free (error);
    }

  g_usb_device_release_interface (usb_dev, 0, 0, &release_error);
  fpi_device_close_complete (device, release_error);
}

static void
dev_close (FpDevice *device)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (device);
  int i;
  FpiUsbTransfer *transfer;

  fp_info ("Closing CanvasBio CB2000 device");

  g_clear_pointer (&self->image_buffer, g_free);
  g_clear_pointer (&self->background_buffer, g_free);
  g_clear_pointer (&self->previous_norm_buffer, g_free);

  for (i = 0; i < 5; i++)
    g_clear_pointer (&self->enroll_images[i], g_free);

  /* Send vendor deactivation asynchronously before releasing interface. */
  transfer = fpi_usb_transfer_new (device);
  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_INIT, 0x0001, 0, 0);

  transfer->short_is_error = FALSE;
  fpi_usb_transfer_submit (transfer, 500, NULL, dev_close_transfer_cb, NULL);
}

/*
 * Common initialization shared by all action entry points.
 */
static void
dev_action_common_init (FpiDeviceCanvasbio *self)
{
  self->deactivating = FALSE;
  self->deactivation_in_progress = FALSE;
  self->initial_activation_done = FALSE;
  self->force_recovery = FALSE;
  self->previous_norm_valid = FALSE;}

static void
dev_enroll (FpDevice *device)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (device);
  gint i;

  fp_info ("Starting enrollment (%d stages)", CB2000_NR_ENROLL_STAGES);
  dev_action_common_init (self);
  self->enroll_stage = 0;
  self->enroll_gate_rejects = 0;
  for (i = 0; i < CB2000_NR_ENROLL_STAGES; i++)
    g_clear_pointer (&self->enroll_images[i], g_free);

  start_new_cycle (device);
}

static void
dev_verify (FpDevice *device)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (device);

  fp_info ("Starting verification (NCC)");
  dev_action_common_init (self);
  self->verify_result = FPI_MATCH_FAIL;

  start_new_cycle (device);
}

static void
dev_capture (FpDevice *device)
{
  FpiDeviceCanvasbio *self = FPI_DEVICE_CANVASBIO (device);

  fp_info ("Starting image capture");
  dev_action_common_init (self);

  start_new_cycle (device);
}

/* ============================================================================
 * DRIVER REGISTRATION
 * ============================================================================ */

static const FpIdEntry id_table[] = {
  { .vid = CB2000_VID, .pid = CB2000_PID_1 },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_canvasbio_init (FpiDeviceCanvasbio *self)
{
}

static void
fpi_device_canvasbio_class_init (FpiDeviceCanvasbioClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "CanvasBio CB2000";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  dev_class->open    = dev_open;
  dev_class->close   = dev_close;
  dev_class->enroll  = dev_enroll;
  dev_class->verify  = dev_verify;
  dev_class->capture = dev_capture;
  dev_class->nr_enroll_stages = CB2000_NR_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  fpi_device_class_auto_initialize_features (dev_class);
}
