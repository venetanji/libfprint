/*
 * Goodix 27c6:5125 - openchicago glue (no USB)
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * Everything the driver does with a decoded frame lives here, so that the
 * same calls can be replayed offline on recorded frames.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "chicago/openchicago.h"

G_BEGIN_DECLS

/* Persistent state file: magic, version, sensor id (OTP[0:16]), then the
 * openchicago session state (oc_session_save_state). */
#define GOODIX5125_ALGO_STATE_MAGIC "G5OC"
#define GOODIX5125_ALGO_STATE_VERSION 1u

/* Learned-template store (template study across fprintd sessions).  fprintd
 * reloads prints from disk for every verify/identify and never re-saves them,
 * so the driver keeps the last learned template itself: one file per print,
 * <directory>/<SHA-256 of the print's fpi-data, hex>.tpl =
 *   "G5LT", u32 LE format version, sensor id (OTP[0:16]),
 *   SHA-256 of the fpi-data (key), SHA-256 of the payload,
 *   payload = learned fpi-data, serialized GVariant (uayayay).
 * An entry not used for max_age_days, or beyond max_entries (least recently
 * used first), is removed: fprintd never tells the driver that a print was
 * deleted. */
#define GOODIX5125_LEARNED_MAGIC "G5LT"
#define GOODIX5125_LEARNED_VERSION 1u
#define GOODIX5125_LEARNED_SUFFIX ".tpl"
#define GOODIX5125_LEARNED_MAX_ENTRIES 50
#define GOODIX5125_LEARNED_MAX_AGE_DAYS 180

typedef struct _Goodix5125Algo Goodix5125Algo;

typedef struct
{
  guint          stage; /* counted enrolment stages (0..OC_ENGINE_STAGES) */
  gboolean       complete; /* goodix5125_algo_enroll_finish() may be called */
  GError        *retry; /* FP_DEVICE_RETRY_* for a frame not counted, or NULL */
  OcEnrollResult result;
} Goodix5125EnrollStep;

typedef struct
{
  gint           matched; /* index into the print data array, -1: no match */
  GError        *retry; /* frame unusable (FP_DEVICE_RETRY_GENERAL), or NULL */
  GVariant      *updated; /* template study result for the matched print, or NULL */
  gboolean       stored; /* @updated was written to the learned-template store */
  guint          n_learned; /* prints compared through their learned template */
  OcVerifyResult result;
} Goodix5125MatchStep;

void goodix5125_enroll_step_clear (Goodix5125EnrollStep *step);
void goodix5125_match_step_clear (Goodix5125MatchStep *step);

Goodix5125Algo *goodix5125_algo_new (const gchar *state_path);
void goodix5125_algo_free (Goodix5125Algo *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Goodix5125Algo, goodix5125_algo_free)

const gchar *goodix5125_algo_get_state_path (Goodix5125Algo * self);

/* Reads and validates the state file (a missing file is not an error). */
gboolean goodix5125_algo_load_state (Goodix5125Algo *self,
                                     GError        **error);

/* Binds the algorithm to the sensor once its OTP and a fresh no-finger
 * ImageBase are known:
 *   - saved state of this sensor: restored and rebased onto @image_base;
 *   - saved state of another sensor: error (the file is never replaced);
 *   - no saved state: calibration imported from @calidata_path
 *     ([sensor id][payload] layout of MR !648) when
 *     it exists and belongs to this sensor, otherwise the first-run
 *     calibration from @image_base.
 * Called again while active (sensor re-initialised), it only rebases.  The
 * state is saved at once. */
gboolean goodix5125_algo_activate (Goodix5125Algo *self,
                                   const guint8    sensor_id[OC_SENSOR_ID_LEN],
                                   const guint16   image_base[OC_FRAME_PIXELS],
                                   const gchar    *calidata_path,
                                   GError        **error);

gboolean goodix5125_algo_is_active (Goodix5125Algo *self);

/* Enables the learned-template store in @directory (created 0700 on the
 * first write); NULL disables it.  0 for a limit means the default. */
void goodix5125_algo_set_learned_store (Goodix5125Algo *self,
                                        const gchar    *directory,
                                        guint           max_entries,
                                        guint           max_age_days);

/* Store key of a print: SHA-256 (hex) of its fpi-data in normal form. */
gchar *goodix5125_algo_print_key (GVariant *print);

/* Path of the store entry of @print, or NULL when the store is disabled. */
gchar *goodix5125_algo_learned_path (Goodix5125Algo *self,
                                     GVariant       *print);

/* Atomic write, mode 0600, parent directories 0700. */
gboolean goodix5125_algo_save_state (Goodix5125Algo *self,
                                     GError        **error);

/* Drops the session (next activation reloads from the state file). */
void goodix5125_algo_deactivate (Goodix5125Algo *self);

/* The driver enrols with OC_ENROLL_ENGINE and OC_ENGINE_STAGES; the offline
 * test also replays the plain protocol. */
gboolean goodix5125_algo_enroll_begin (Goodix5125Algo  *self,
                                       OcEnrollProtocol protocol,
                                       guint            n_stages,
                                       GError         **error);
/* @extra: second frame of the same touch or NULL. */
gboolean goodix5125_algo_enroll_add (Goodix5125Algo       *self,
                                     const guint16         frame[OC_FRAME_PIXELS],
                                     const guint16        *extra,
                                     Goodix5125EnrollStep *step,
                                     GError              **error);
/* FpPrint fpi-data of the finished enrolment. */
GVariant *goodix5125_algo_enroll_finish (Goodix5125Algo *self,
                                         GError        **error);
void goodix5125_algo_enroll_cancel (Goodix5125Algo *self);

/* Verify (@n_prints = 1) or identify.  Prints that belong to another sensor
 * or are not openchicago data are skipped; FALSE only on an internal error or
 * when no print is usable.  With the learned-template store, a print that has
 * a valid entry of this sensor is compared through the learned template, and
 * a new template study result is written back under the key of the print as
 * given (never of the learned data).  A corrupt, foreign or unreadable entry
 * only logs a warning; the print's own template is used. */
gboolean goodix5125_algo_match (Goodix5125Algo      *self,
                                const guint16        frame[OC_FRAME_PIXELS],
                                GVariant *const     *prints,
                                guint                n_prints,
                                Goodix5125MatchStep *step,
                                GError             **error);

G_END_DECLS
