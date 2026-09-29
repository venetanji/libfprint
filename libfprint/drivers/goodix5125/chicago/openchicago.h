// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com>
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * openchicago: a free implementation of the Goodix ChicagoHS fingerprint
 * algorithm (profile 12, 64x80 sensors such as 27c6:5125).  Built on the
 * ChicagoHS port from libfprint MR !648.
 *
 * All state lives in an OcSession: the adaptive preprocessor calibration and
 * the cross-frame feature extraction state.  No globals; one session per sensor,
 * not thread-safe (use one thread per session).
 *
 * Frames: 16-bit raw sensor values, 12 significant bits, in the algorithm
 * orientation — 64 rows x 80 columns, row-major (OC_FRAME_WIDTH = 80).  The
 * 27c6:5125 transport delivers 80 rows x 64 columns: transpose with
 * oc_frame_transpose() first.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define OC_FRAME_WIDTH 80u
#define OC_FRAME_HEIGHT 64u
#define OC_FRAME_PIXELS (OC_FRAME_WIDTH * OC_FRAME_HEIGHT)
#define OC_SENSOR_ID_LEN 16u
#define OC_CONTEXT_SIZE 6u

/* Enrolment protocol defaults (OC_ENROLL_ENGINE). */
#define OC_ENGINE_STAGES 12u         /* counted frames per template */
#define OC_ENGINE_MAX_TIPS 8u        /* max. overlay tips per enrolment */
#define OC_ENGINE_MIN_QUALITY 25u    /* min. frame quality */
#define OC_ENGINE_MIN_COVERAGE 65u   /* min. frame coverage */

typedef struct _OcSession OcSession;

/* Why a frame was not used (or, for OC_REJECT_TIP, not counted). */
typedef enum {
  OC_REJECT_NONE = 0,
  OC_REJECT_BAD_INPUT,       /* preprocessor rc 0x29aa: raw frame invalid */
  OC_REJECT_POOR_CAPTURE,    /* preprocessor rc 0x7531 */
  OC_REJECT_CLASS,           /* preprocessor rc 0xc351 (resolution class) */
  OC_REJECT_NO_FEATURES,     /* feature extraction found no records */
  OC_REJECT_MERGE_FAILURE,   /* adding the frame failed, template unchanged */
  OC_REJECT_TEMPLATE_FULL,   /* enrolment already holds its maximum of frames */
  OC_REJECT_TOO_SHORT,       /* engine protocol: coverage < 65 */
  OC_REJECT_LOW_QUALITY,     /* engine protocol: quality < 25 */
  OC_REJECT_TIP,             /* engine protocol: area already enrolled; the frame
                             * stays in the template, the stage is not counted */
} OcReject;

const gchar *oc_reject_to_string (OcReject reject);

/* Transport frame (80 rows x 64 columns) -> algorithm frame (64 rows x 80). */
void oc_frame_transpose (const guint16 sensor[OC_FRAME_PIXELS],
                         guint16       frame[OC_FRAME_PIXELS]);

/* ---- session and calibration ------------------------------------------- */

/* First run: the calibration is generated from @image_base, a no-finger
 * frame. */
OcSession *oc_session_new (const guint16 image_base[OC_FRAME_PIXELS],
                           GError      **error);

/* Calibration file `goodix_calib.dat` ([sensor id][0x224b0-byte
 * payload], the layout of libfprint MR !648) bound to @sensor_id (first 16
 * bytes of the sensor OTP), rebased onto the fresh @image_base. */
OcSession *oc_session_new_from_calidata (const gchar  *path,
                                         const guint8  sensor_id[OC_SENSOR_ID_LEN],
                                         const guint16 image_base[OC_FRAME_PIXELS],
                                         GError      **error);

void oc_session_free (OcSession *self);

/* Complete adaptive state of the session in the openchicago format
 * (versioned, CRC-protected): calibration
 * payload, ImageBase, every adaptive preprocessor variable including the
 * resolution-context history and the cross-frame feature extraction
 * state.  A session restored with oc_session_new_from_state() continues
 * exactly as the saved one would have. */
GBytes *oc_session_save_state (OcSession *self);
OcSession *oc_session_new_from_state (GBytes  *state,
                                      GError **error);

/* Rebase a running session: a fresh no-finger
 * frame replaces the ImageBase (and the offset plane of the calibration);
 * every adaptive variable is kept.  Use it after oc_session_new_from_state()
 * when the sensor was re-initialised (new power cycle, other temperature). */
/* The ImageBase the session currently subtracts (64 x 80). */
const guint16 *oc_session_get_image_base (OcSession *self);

gboolean oc_session_rebase (OcSession    *self,
                            const guint16 image_base[OC_FRAME_PIXELS],
                            GError      **error);

/* ---- per-frame primitives ---------------------------------------------- */

typedef struct
{
  guint32 status;                       /* preprocessor() rc, 0 = ok */
  guint8  quality;                      /* 0..100 */
  guint8  coverage;                     /* 0..100 */
  guint8  context[OC_CONTEXT_SIZE];     /* cbuf[0..5] */
  guint8  enhanced[OC_FRAME_PIXELS];    /* 8-bit enhanced image */
} OcPreprocessResult;

/* One preprocessor() call (@enroll: purpose 1, else 0).  It advances the
 * adaptive state exactly like the frames given to enroll/verify do — use it
 * for warm-up and diagnostics, not in addition to them for the same frame.
 * Returns TRUE when status == 0. */
gboolean oc_session_preprocess (OcSession          *self,
                                const guint16       frame[OC_FRAME_PIXELS],
                                gboolean            enroll,
                                OcPreprocessResult *result);

/* ---- enrolment --------------------------------------------------------- */

typedef enum {
  /* Add every usable frame (no quality/coverage
   * thresholds, no tips); complete after @n_stages frames. */
  OC_ENROLL_PLAIN,
  /* Enrolment protocol for ChicagoHS: coverage >= 65 and
   * quality >= 25, overlay/preoverlay tips (frames 5..12: overlay > 30 or
   * preoverlay > 20, at most 8 tips, 2 in a row, backup frame), complete
   * after @n_stages (12) counted frames or 20 frames in the template. */
  OC_ENROLL_ENGINE,
} OcEnrollProtocol;

typedef struct
{
  gboolean accepted;      /* frame counted as a stage */
  OcReject reject;        /* reason when !accepted */
  guint32  status;        /* preprocessor() rc */
  guint    quality;
  guint    coverage;
  guint    overlay;       /* % of the frame already covered by the template */
  guint    preoverlay;    /* overlap with the previous frame */
  guint    tip_direction; /* OC_REJECT_TIP: 1 up, 2 down, 3 left, 4 right */
  guint    stage;         /* counted frames so far */
  guint    n_stages;
  guint    samples;       /* frames stored in the template */
  guint    progress;      /* 0..100 */
  gboolean complete;      /* oc_enroll_finish() may be called */
} OcEnrollResult;

gboolean oc_enroll_begin (OcSession       *self,
                          OcEnrollProtocol protocol,
                          guint            n_stages,
                          GError         **error);

/* Adds one frame.  Returns FALSE only on an internal error (@error set); a
 * rejected frame is reported in @result. */
gboolean oc_enroll_add (OcSession      *self,
                        const guint16   frame[OC_FRAME_PIXELS],
                        OcEnrollResult *result,
                        GError        **error);

/* As oc_enroll_add(), choosing the better of two frames: @extra_frame is a second
 * capture of the same touch.  It is preprocessed only if @frame preprocesses
 * without error, and replaces @frame when its quality is higher by more than
 * 2, or within 2 with a larger coverage, or higher with no smaller coverage. */
gboolean oc_enroll_add_pair (OcSession      *self,
                             const guint16   frame[OC_FRAME_PIXELS],
                             const guint16   extra_frame[OC_FRAME_PIXELS],
                             OcEnrollResult *result,
                             GError        **error);

/* The packed template (tagged ChicagoHS template layout).  Ends the
 * enrolment; may be called before completion (then with fewer frames). */
GBytes *oc_enroll_finish (OcSession *self,
                          GError   **error);

void oc_enroll_cancel (OcSession *self);

/* ---- verification ------------------------------------------------------ */

typedef struct
{
  gboolean match;         /* score > 0 */
  gint32   score;
  gint     subtemplate;   /* selected subtemplate, -1 if none */
  OcReject reject;        /* frame unusable: no match attempted */
  guint32  status;        /* preprocessor() rc */
  guint    quality;
  guint    coverage;
} OcVerifyResult;

/* Match @frame against @template_blob.  If @updated_blob is not
 * NULL and the frame matched, template study runs: *updated_blob receives the
 * learned template to store instead of the old one, or NULL when nothing was
 * learned.  Returns FALSE only on an error (e.g. a corrupt template). */
gboolean oc_verify (OcSession      *self,
                    GBytes         *template_blob,
                    const guint16   frame[OC_FRAME_PIXELS],
                    OcVerifyResult *result,
                    GBytes        **updated_blob,
                    GError        **error);

/* Match one @frame against @n_templates templates (the frame is
 * preprocessed once, as by oc_verify()).  The match is the template with the
 * highest score > 0 (first one on a tie); *@matched_index receives its index
 * or -1.  @result describes the frame and the best match.  @updated_blob as
 * in oc_verify(), for the matched template only. */
gboolean oc_identify (OcSession      *self,
                      GBytes *const  *templates,
                      guint           n_templates,
                      const guint16   frame[OC_FRAME_PIXELS],
                      OcVerifyResult *result,
                      gint           *matched_index,
                      GBytes        **updated_blob,
                      GError        **error);

/* ---- storage helpers for drivers --------------------------------------- */

/* FpPrint data of libfprint MR !648: GVariant "(uayayay)" = (version,
 * sensor id, SHA-256 of the session calibration (informational, not
 * checked), packed template). */
GVariant *oc_print_data_new (OcSession   *self,
                             const guint8 sensor_id[OC_SENSOR_ID_LEN],
                             GBytes      *template_blob);
/* Returns the packed template; fails for another sensor id (if given). */
GBytes *oc_print_data_get_template (GVariant    *print_data,
                                    const guint8 sensor_id[OC_SENSOR_ID_LEN],
                                    GError     **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OcSession, oc_session_free)

G_END_DECLS
