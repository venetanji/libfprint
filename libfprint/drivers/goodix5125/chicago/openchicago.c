// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * openchicago public API (openchicago.h): session, enrolment,
 * verification and state serialization on top of the ChicagoHS port.
 */

#include "openchicago.h"

#include <string.h>

#include <gio/gio.h>

#include "goodix-chicago-calibration.h"
#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-feature.h"
#include "goodix-chicago-preprocess.h"
#include "goodix-chicago-runtime.h"
#include "goodix-chicago-template.h"
#include "openchicago-private.h"

G_STATIC_ASSERT (OC_FRAME_PIXELS == GOODIX_CHICAGO_PIXELS);
G_STATIC_ASSERT (OC_SENSOR_ID_LEN == GOODIX_CHICAGO_SENSOR_ID_LEN);
G_STATIC_ASSERT (OC_CONTEXT_SIZE == GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE);

#define ENGINE_OVERLAY_TIP 30u       /* max_overlay_ratio */
#define ENGINE_PREOVERLAY_TIP 20u    /* max_preoverlay_ratio */
#define ENGINE_TIP_FIRST 5u          /* enroll_overlay_tip_start */
#define ENGINE_TIP_LAST 12u          /* enroll_overlay_tip_end */
#define ENGINE_BACKUP_OVERLAY 80u    /* del_enroll_img_overlay_threshold */
#define ENGINE_BACKUP_PREOVERLAY 70u /* del_enroll_img_preoverlay_threshold */

/* Per-enrolment counters of the OC_ENROLL_ENGINE protocol. */
typedef struct
{
  guint                     touched;
  guint                     enrolled;
  guint                     tipped;
  guint                     continuous_tips;
  guint                     last_tipped;
  guint                     tip_index;
  gboolean                  has_backup;
  GoodixChicagoRuntimeFrame backup;   /* frame removed on the first tip */
  guint                     backup_overlay;
} EngineState;

struct _OcSession
{
  GoodixChicagoPreprocessor *preprocessor;
  GoodixChicagoFeatureState  feature_state;   /* cross-frame feature extraction state */

  /* enrolment in progress */
  GoodixChicagoEnrollment *enrollment;
  OcEnrollProtocol         protocol;
  guint                    n_stages;
  guint                    stage;
  guint                    samples_max;
  EngineState              engine;
};

const gchar *
oc_reject_to_string (OcReject reject)
{
  switch (reject)
    {
    case OC_REJECT_NONE: return "none";

    case OC_REJECT_BAD_INPUT: return "bad-input";

    case OC_REJECT_POOR_CAPTURE: return "poor-capture";

    case OC_REJECT_CLASS: return "class-reject";

    case OC_REJECT_NO_FEATURES: return "no-features";

    case OC_REJECT_MERGE_FAILURE: return "merge-failure";

    case OC_REJECT_TEMPLATE_FULL: return "template-full";

    case OC_REJECT_TOO_SHORT: return "too-short";

    case OC_REJECT_LOW_QUALITY: return "low-quality";

    case OC_REJECT_TIP: return "tip";
    }
  return "unknown";
}

void
oc_frame_transpose (const guint16 sensor[OC_FRAME_PIXELS],
                    guint16       frame[OC_FRAME_PIXELS])
{
  g_return_if_fail (sensor != NULL && frame != NULL && sensor != frame);
  for (guint r = 0; r < OC_FRAME_WIDTH; r++)
    for (guint c = 0; c < OC_FRAME_HEIGHT; c++)
      frame[c * OC_FRAME_WIDTH + r] = sensor[r * OC_FRAME_HEIGHT + c];
}

static OcReject
reject_from_status (guint32 status)
{
  switch (status)
    {
    case GOODIX_CHICAGO_PREPROCESS_STATUS_OK: return OC_REJECT_NONE;

    case GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT: return OC_REJECT_BAD_INPUT;

    case GOODIX_CHICAGO_PREPROCESS_STATUS_CLASS_REJECT: return OC_REJECT_CLASS;

    default: return OC_REJECT_POOR_CAPTURE;
    }
}

/* ---- session ------------------------------------------------------------ */

static OcSession *
session_new_calibrated (GBytes        *calibration,
                        const guint16 *image_base,
                        GError       **error)
{
  g_autoptr(OcSession) self = g_new0 (OcSession, 1);

  self->preprocessor = goodix_chicago_preprocessor_new (calibration, image_base,
                                                        error);
  if (!self->preprocessor)
    return NULL;
  goodix_chicago_feature_state_init (&self->feature_state);
  return g_steal_pointer (&self);
}

OcSession *
oc_session_new (const guint16 image_base[OC_FRAME_PIXELS],
                GError      **error)
{
  g_autoptr(GBytes) calibration = NULL;

  g_return_val_if_fail (image_base != NULL, NULL);
  calibration = goodix_chicago_calibration_generate (image_base);
  if (!calibration)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "openchicago: cannot generate the calibration");
      return NULL;
    }
  return session_new_calibrated (calibration, image_base, error);
}

OcSession *
oc_session_new_from_calidata (const gchar  *path,
                              const guint8  sensor_id[OC_SENSOR_ID_LEN],
                              const guint16 image_base[OC_FRAME_PIXELS],
                              GError      **error)
{
  g_autoptr(GBytes) loaded = NULL;
  g_autoptr(GBytes) rebased = NULL;

  g_return_val_if_fail (path != NULL && sensor_id != NULL, NULL);
  g_return_val_if_fail (image_base != NULL, NULL);
  loaded = goodix_chicago_calibration_load (path, sensor_id, error);
  if (!loaded)
    return NULL;
  rebased = goodix_chicago_calibration_rebase (loaded, image_base, error);
  if (!rebased)
    return NULL;
  return session_new_calibrated (rebased, image_base, error);
}

void
oc_session_free (OcSession *self)
{
  if (!self)
    return;
  oc_enroll_cancel (self);
  goodix_chicago_preprocessor_free (self->preprocessor);
  g_free (self);
}

/* ---- state serialization -----------------------------------------------
 * "OCST" | u32 version | sections { char tag[4]; u32 len; data } | u32 crc
 * CALI calibration payload, IBAS ImageBase (u16 LE), PREP preprocessor
 * (goodix_chicago_preprocessor_save_state), FEAT feature extraction state.  crc =
 * CRC-32 (IEEE) of everything before it. */

#define STATE_MAGIC "OCST"
#define STATE_VERSION 1u

static guint32
crc32_ieee (const guint8 *data, gsize len)
{
  guint32 crc = G_MAXUINT32;

  for (gsize i = 0; i < len; i++)
    {
      crc ^= data[i];
      for (guint b = 0; b < 8; b++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
  return ~crc;
}

static void
put_u32 (GByteArray *out, guint32 v)
{
  guint32 le = GUINT32_TO_LE (v);

  g_byte_array_append (out, (const guint8 *) &le, 4);
}

static guint32
get_u32 (const guint8 *p)
{
  return (guint32) p[0] | (guint32) p[1] << 8 | (guint32) p[2] << 16 |
         (guint32) p[3] << 24;
}

static void
section_begin (GByteArray *out, const gchar tag[4], guint *len_at)
{
  g_byte_array_append (out, (const guint8 *) tag, 4);
  *len_at = out->len;
  put_u32 (out, 0);
}

static void
section_end (GByteArray *out, guint len_at)
{
  guint32 le = GUINT32_TO_LE (out->len - len_at - 4);

  memcpy (out->data + len_at, &le, 4);
}

#define FEATURE_STATE_SIZE (4 * 4 + 3 * GOODIX_CHICAGO_FEATURE_PIXELS + 5 * 4)

static void
feature_state_save (const GoodixChicagoFeatureState *fs, GByteArray *out)
{
  put_u32 (out, (guint32) fs->last_coverage);
  put_u32 (out, (guint32) fs->history_count);
  put_u32 (out, (guint32) fs->stable_count);
  put_u32 (out, (guint32) fs->last_level);
  g_byte_array_append (out, &fs->history[0][0], sizeof (fs->history));
  put_u32 (out, fs->consensus.negative_percent);
  put_u32 (out, fs->consensus.positive_percent);
  put_u32 (out, fs->consensus.neutral_percent);
  put_u32 (out, fs->consensus.density_class);
  put_u32 (out, fs->consensus.inactive_count);
}

/* The state is only checksummed, not authenticated: history_count indexes
 * history[], the other fields are bounded by what getFeature produces. */
static gboolean
feature_state_load (GoodixChicagoFeatureState *fs, const guint8 *p)
{
  fs->last_coverage = (gint) get_u32 (p);
  fs->history_count = (gint) get_u32 (p + 4);
  fs->stable_count = (gint) get_u32 (p + 8);
  fs->last_level = (gint) get_u32 (p + 12);
  p += 16;
  memcpy (&fs->history[0][0], p, sizeof (fs->history));
  p += sizeof (fs->history);
  fs->consensus.negative_percent = get_u32 (p);
  fs->consensus.positive_percent = get_u32 (p + 4);
  fs->consensus.neutral_percent = get_u32 (p + 8);
  fs->consensus.density_class = get_u32 (p + 12);
  fs->consensus.inactive_count = get_u32 (p + 16);
  return fs->history_count >= 0 && fs->history_count <= 3 &&
         fs->stable_count >= 0 && fs->stable_count <= 5 &&
         fs->last_level >= 0 && fs->last_level <= 0xff &&
         fs->consensus.negative_percent <= 100 &&
         fs->consensus.positive_percent <= 100 &&
         fs->consensus.neutral_percent <= 100 &&
         fs->consensus.density_class <= 2;
}

GBytes *
oc_session_save_state (OcSession *self)
{
  GByteArray *out;
  GBytes *calibration;
  const guint16 *image_base;
  const guint8 *cal;
  gsize cal_len;
  guint at;

  g_return_val_if_fail (self != NULL, NULL);
  out = g_byte_array_new ();
  g_byte_array_append (out, (const guint8 *) STATE_MAGIC, 4);
  put_u32 (out, STATE_VERSION);

  calibration = goodix_chicago_preprocessor_get_calibration (self->preprocessor);
  cal = g_bytes_get_data (calibration, &cal_len);
  section_begin (out, "CALI", &at);
  g_byte_array_append (out, cal, cal_len);
  section_end (out, at);

  image_base = goodix_chicago_preprocessor_get_image_base (self->preprocessor);
  section_begin (out, "IBAS", &at);
  for (guint i = 0; i < OC_FRAME_PIXELS; i++)
    {
      guint16 le = GUINT16_TO_LE (image_base[i]);

      g_byte_array_append (out, (const guint8 *) &le, 2);
    }
  section_end (out, at);

  section_begin (out, "PREP", &at);
  goodix_chicago_preprocessor_save_state (self->preprocessor, out);
  section_end (out, at);

  section_begin (out, "FEAT", &at);
  feature_state_save (&self->feature_state, out);
  section_end (out, at);

  put_u32 (out, crc32_ieee (out->data, out->len));
  return g_byte_array_free_to_bytes (out);
}

static gboolean
state_section (const guint8 *data, gsize len, gsize *pos, const gchar tag[4],
               const guint8 **body, gsize *body_len, GError **error)
{
  if (*pos + 8 > len || memcmp (data + *pos, tag, 4) != 0)
    goto bad;
  *body_len = get_u32 (data + *pos + 4);
  if (*body_len > len - *pos - 8)
    goto bad;
  *body = data + *pos + 8;
  *pos += 8 + *body_len;
  return TRUE;

bad:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "openchicago: state section %.4s missing or truncated", tag);
  return FALSE;
}

OcSession *
oc_session_new_from_state (GBytes  *state,
                           GError **error)
{
  const guint8 *data, *body;
  gsize len, body_len, pos = 8;

  g_autoptr(GBytes) calibration = NULL;
  guint16 image_base[OC_FRAME_PIXELS];
  g_autoptr(OcSession) self = NULL;

  g_return_val_if_fail (state != NULL, NULL);
  data = g_bytes_get_data (state, &len);
  if (len < 12 || memcmp (data, STATE_MAGIC, 4) != 0 ||
      get_u32 (data + 4) != STATE_VERSION)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "openchicago: not an openchicago state (v1)");
      return NULL;
    }
  if (crc32_ieee (data, len - 4) != get_u32 (data + len - 4))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: state CRC mismatch");
      return NULL;
    }
  len -= 4;

  if (!state_section (data, len, &pos, "CALI", &body, &body_len, error))
    return NULL;
  calibration = g_bytes_new (body, body_len);
  if (!state_section (data, len, &pos, "IBAS", &body, &body_len, error))
    return NULL;
  if (body_len != sizeof (image_base))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: bad ImageBase section");
      return NULL;
    }
  for (guint i = 0; i < OC_FRAME_PIXELS; i++)
    image_base[i] = (guint16) (body[2 * i] | body[2 * i + 1] << 8);

  self = session_new_calibrated (calibration, image_base, error);
  if (!self)
    return NULL;

  if (!state_section (data, len, &pos, "PREP", &body, &body_len, error) ||
      !goodix_chicago_preprocessor_load_state (self->preprocessor, body,
                                               body_len, error))
    return NULL;
  if (!state_section (data, len, &pos, "FEAT", &body, &body_len, error))
    return NULL;
  if (body_len != FEATURE_STATE_SIZE || pos != len)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: bad feature state section");
      return NULL;
    }
  if (!feature_state_load (&self->feature_state, body))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "openchicago: feature state out of range");
      return NULL;
    }
  return g_steal_pointer (&self);
}

const guint16 *
oc_session_get_image_base (OcSession *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return goodix_chicago_preprocessor_get_image_base (self->preprocessor);
}

gboolean
oc_session_rebase (OcSession    *self,
                   const guint16 image_base[OC_FRAME_PIXELS],
                   GError      **error)
{
  g_autoptr(GBytes) calibration = NULL;

  g_return_val_if_fail (self != NULL && image_base != NULL, FALSE);
  calibration = goodix_chicago_calibration_rebase (
    goodix_chicago_preprocessor_get_calibration (self->preprocessor),
    image_base, error);
  if (!calibration)
    return FALSE;
  return goodix_chicago_preprocessor_rebase (self->preprocessor, calibration,
                                             image_base, error);
}

gboolean
_oc_session_state_equal (OcSession *a, OcSession *b)
{
  g_return_val_if_fail (a != NULL && b != NULL, FALSE);
  return goodix_chicago_preprocessor_state_equal (a->preprocessor, b->preprocessor) &&
         memcmp (&a->feature_state, &b->feature_state, sizeof (a->feature_state)) == 0;
}

gboolean
_oc_enroll_delete_last (OcSession *self)
{
  g_return_val_if_fail (self != NULL, FALSE);
  return self->enrollment &&
         goodix_chicago_enrollment_drop_last (self->enrollment, NULL);
}

/* ---- per-frame primitives ---------------------------------------------- */

gboolean
oc_session_preprocess (OcSession          *self,
                       const guint16       frame[OC_FRAME_PIXELS],
                       gboolean            enroll,
                       OcPreprocessResult *result)
{
  g_autofree GoodixChicagoRuntimeFrame *f = g_new (GoodixChicagoRuntimeFrame, 1);
  gboolean ok;

  g_return_val_if_fail (self != NULL && frame != NULL && result != NULL, FALSE);
  ok = goodix_chicago_runtime_preprocess (self->preprocessor, frame, enroll, f);
  result->status = f->status;
  result->quality = f->quality;
  result->coverage = f->coverage;
  memcpy (result->context, f->context, sizeof (result->context));
  memcpy (result->enhanced, f->enhanced, sizeof (result->enhanced));
  return ok;
}

/* ---- enrolment ---------------------------------------------------------- */

gboolean
oc_enroll_begin (OcSession       *self,
                 OcEnrollProtocol protocol,
                 guint            n_stages,
                 GError         **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  if (n_stages == 0 || n_stages > GOODIX_CHICAGO_ENROLLMENT_CAPACITY)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "openchicago: %u enrolment stages (1..%u)", n_stages,
                   GOODIX_CHICAGO_ENROLLMENT_CAPACITY);
      return FALSE;
    }
  oc_enroll_cancel (self);
  self->enrollment = goodix_chicago_enrollment_new ();
  self->protocol = protocol;
  self->n_stages = n_stages;
  self->stage = 0;
  /* The template keeps at most samples_max frames: the engine protocol
   * allows samples + tips; the plain protocol stores every stage. */
  self->samples_max = protocol == OC_ENROLL_ENGINE ?
                      MIN (n_stages + OC_ENGINE_MAX_TIPS, GOODIX_CHICAGO_ENROLLMENT_CAPACITY) :
                      n_stages;
  memset (&self->engine, 0, sizeof (self->engine));
  return TRUE;
}

void
oc_enroll_cancel (OcSession *self)
{
  g_return_if_fail (self != NULL);
  g_clear_pointer (&self->enrollment, goodix_chicago_enrollment_free);
}

/* Add an enrolment frame: feature extraction (enrolment mode) + merge.
 * Returns FALSE with *reject set when the template did not change. */
static gboolean
enroll_add_frame (OcSession                       *self,
                  const GoodixChicagoRuntimeFrame *frame,
                  OcReject                        *reject,
                  guint                           *overlay,
                  guint                           *preoverlay)
{
  g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
  g_autoptr(GError) local = NULL;
  const GoodixChicagoSubtemplateView *v;
  GoodixChicagoEnrollmentResult res = { 0 };
  GoodixChicagoRuntimeReject rr;
  gboolean ok;

  if (goodix_chicago_enrollment_get_count (self->enrollment) >= self->samples_max)
    {
      *reject = OC_REJECT_TEMPLATE_FULL;
      return FALSE;
    }
  probe = goodix_chicago_runtime_probe_new (&self->feature_state, frame, FALSE,
                                            &rr, &local);
  if (!probe)
    {
      *reject = rr == GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES ?
                OC_REJECT_NO_FEATURES : OC_REJECT_MERGE_FAILURE;
      return FALSE;
    }
  v = goodix_chicago_runtime_probe_get_view (probe);
  if (goodix_chicago_enrollment_get_count (self->enrollment) == 0)
    {
      ok = goodix_chicago_enrollment_insert_first (self->enrollment, v->records,
                                                   v->record_count, v->active_count,
                                                   v->quality, v->coverage,
                                                   v->metric_data, &res, &local);
    }
  else
    {
      ok = goodix_chicago_enrollment_insert_next (self->enrollment, v->records,
                                                  v->record_count, v->active_count,
                                                  v->quality, v->coverage,
                                                  v->metric_data, &res, &local);
    }
  if (!ok)
    {
      *reject = OC_REJECT_MERGE_FAILURE;
      return FALSE;
    }
  /* Merge result r: preoverlay = 100 - (r >> 24),
   * overlay = 100 - (r & 0xff). */
  *preoverlay = 100u - (res.packed_position_detail >> 24);
  *overlay = 100u - (res.packed_position_detail & 0xff);
  *reject = OC_REJECT_NONE;
  return TRUE;
}

static void
enroll_fill_progress (OcSession *self, OcEnrollResult *result)
{
  guint count = goodix_chicago_enrollment_get_count (self->enrollment);

  result->stage = self->stage;
  result->n_stages = self->n_stages;
  result->samples = count;
  /* min(100, count * 100 / max) */
  result->progress = MIN (100u, count * 100u / self->samples_max);
  result->complete = self->stage >= self->n_stages || count >= self->samples_max;
}

/* Overlay tips with the backup frame. */
static void
engine_after_add (OcSession                       *self,
                  const GoodixChicagoRuntimeFrame *frame,
                  guint                            overlay,
                  guint                            preoverlay,
                  OcEnrollResult                  *result)
{
  static const guint tip_info[4] = { 4, 1, 3, 2 };   /* RIGHT, HIGH, LEFT, LOW */
  EngineState *e = &self->engine;
  OcReject r;
  guint ov, pov;

  e->touched++;
  if (e->enrolled + 1 >= ENGINE_TIP_FIRST && e->enrolled + 1 <= ENGINE_TIP_LAST &&
      (overlay > ENGINE_OVERLAY_TIP || preoverlay > ENGINE_PREOVERLAY_TIP) &&
      e->tipped < OC_ENGINE_MAX_TIPS && e->continuous_tips < 2)
    {
      gboolean similar = overlay > ENGINE_BACKUP_OVERLAY &&
                         preoverlay > ENGINE_BACKUP_PREOVERLAY;

      e->continuous_tips++;
      e->tipped++;
      e->last_tipped = e->touched;
      if (e->continuous_tips == 1 && similar)
        {
          /* Keep the frame aside, drop it from the template */
          e->backup = *frame;
          e->backup_overlay = overlay;
          e->has_backup = TRUE;
          goodix_chicago_enrollment_drop_last (self->enrollment, NULL);
        }
      else if (e->continuous_tips == 2 && e->has_backup)
        {
          /* Keep the one with less overlay (equal: better
           * quality); otherwise the backup goes back in. */
          if (!similar || e->backup_overlay < overlay ||
              (e->backup_overlay == overlay && e->backup.quality > frame->quality))
            {
              if (similar)
                goodix_chicago_enrollment_drop_last (self->enrollment, NULL);
              enroll_add_frame (self, &e->backup, &r, &ov, &pov);
            }
          e->has_backup = FALSE;
        }
      result->accepted = FALSE;
      result->reject = OC_REJECT_TIP;
      result->tip_direction = tip_info[e->tip_index];
      return;
    }
  if (e->last_tipped != 0 && e->last_tipped + 1 == e->touched)
    {
      if (e->has_backup)
        {
          enroll_add_frame (self, &e->backup, &r, &ov, &pov);
          e->has_backup = FALSE;
        }
      e->tip_index = (e->tip_index + 1) % G_N_ELEMENTS (tip_info);
    }
  e->continuous_tips = 0;
  e->enrolled++;
  self->stage = e->enrolled;
  result->accepted = TRUE;
}

/* Pick the better of two captures of the same touch. */
static gboolean
engine_extra_is_better (const GoodixChicagoRuntimeFrame *extra,
                        const GoodixChicagoRuntimeFrame *current)
{
  const gint nq = extra->quality, cq = current->quality;

  return nq > cq + 2 || (ABS (nq - cq) < 2 && extra->coverage > current->coverage) ||
         (nq > cq && extra->coverage >= current->coverage);
}

static gboolean enroll_add_impl (OcSession      *self,
                                 const guint16  *frame,
                                 const guint16  *extra_frame,
                                 OcEnrollResult *result,
                                 GError        **error);

gboolean
oc_enroll_add (OcSession      *self,
               const guint16   frame[OC_FRAME_PIXELS],
               OcEnrollResult *result,
               GError        **error)
{
  return enroll_add_impl (self, frame, NULL, result, error);
}

gboolean
oc_enroll_add_pair (OcSession      *self,
                    const guint16   frame[OC_FRAME_PIXELS],
                    const guint16   extra_frame[OC_FRAME_PIXELS],
                    OcEnrollResult *result,
                    GError        **error)
{
  return enroll_add_impl (self, frame, extra_frame, result, error);
}

static gboolean
enroll_add_impl (OcSession      *self,
                 const guint16  *frame,
                 const guint16  *extra_frame,
                 OcEnrollResult *result,
                 GError        **error)
{
  g_autofree GoodixChicagoRuntimeFrame *f = NULL;
  g_autofree GoodixChicagoRuntimeFrame *f2 = NULL;
  guint overlay = 0, preoverlay = 0;
  OcReject reject;

  g_return_val_if_fail (self != NULL && frame != NULL && result != NULL, FALSE);
  memset (result, 0, sizeof (*result));
  if (!self->enrollment)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                           "openchicago: oc_enroll_begin() was not called");
      return FALSE;
    }
  f = g_new (GoodixChicagoRuntimeFrame, 1);
  enroll_fill_progress (self, result);
  if (result->complete)
    {
      result->reject = OC_REJECT_TEMPLATE_FULL;
      return TRUE;
    }
  goodix_chicago_runtime_preprocess (self->preprocessor, frame, TRUE, f);
  if (extra_frame && f->status == GOODIX_CHICAGO_PREPROCESS_STATUS_OK)
    {
      /* second frame of the same touch, preprocessed (purpose 1) only after
       * the first one succeeded; replaces it when better */
      f2 = g_new (GoodixChicagoRuntimeFrame, 1);
      if (goodix_chicago_runtime_preprocess (self->preprocessor, extra_frame, TRUE, f2) &&
          engine_extra_is_better (f2, f))
        {
          g_free (f);
          f = g_steal_pointer (&f2);
        }
    }
  result->status = f->status;
  result->quality = f->quality;
  result->coverage = f->coverage;
  result->reject = reject_from_status (f->status);
  if (result->reject != OC_REJECT_NONE)
    return TRUE;
  if (self->protocol == OC_ENROLL_ENGINE)
    {
      /* Frame thresholds before the frame is added */
      if (f->coverage < OC_ENGINE_MIN_COVERAGE)
        result->reject = OC_REJECT_TOO_SHORT;
      else if (f->quality < OC_ENGINE_MIN_QUALITY)
        result->reject = OC_REJECT_LOW_QUALITY;
      if (result->reject != OC_REJECT_NONE)
        return TRUE;
    }
  if (!enroll_add_frame (self, f, &reject, &overlay, &preoverlay))
    {
      result->reject = reject;
      enroll_fill_progress (self, result);
      return TRUE;
    }
  result->overlay = overlay;
  result->preoverlay = preoverlay;
  if (self->protocol == OC_ENROLL_ENGINE)
    {
      engine_after_add (self, f, overlay, preoverlay, result);
    }
  else
    {
      self->stage++;
      result->accepted = TRUE;
    }
  enroll_fill_progress (self, result);
  return TRUE;
}

GBytes *
oc_enroll_finish (OcSession *self,
                  GError   **error)
{
  GBytes *packed;

  g_return_val_if_fail (self != NULL, NULL);
  if (!self->enrollment || goodix_chicago_enrollment_get_count (self->enrollment) == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                           "openchicago: no enrolled frames");
      return NULL;
    }
  packed = goodix_chicago_enrollment_pack (self->enrollment, error);
  oc_enroll_cancel (self);
  return packed;
}

/* ---- verification ------------------------------------------------------ */

gboolean
oc_verify (OcSession      *self,
           GBytes         *template_blob,
           const guint16   frame[OC_FRAME_PIXELS],
           OcVerifyResult *result,
           GBytes        **updated_blob,
           GError        **error)
{
  gint index;

  g_return_val_if_fail (template_blob != NULL, FALSE);
  return oc_identify (self, &template_blob, 1, frame, result, &index,
                      updated_blob, error);
}

gboolean
oc_identify (OcSession      *self,
             GBytes *const  *templates,
             guint           n_templates,
             const guint16   frame[OC_FRAME_PIXELS],
             OcVerifyResult *result,
             gint           *matched_index,
             GBytes        **updated_blob,
             GError        **error)
{
  g_autofree GoodixChicagoRuntimeFrame *f = NULL;

  g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
  GoodixChicagoRuntimeReject rr;
  GoodixChicagoMatchTemplateResult res, best;

  g_return_val_if_fail (self != NULL && (templates != NULL || n_templates == 0), FALSE);
  g_return_val_if_fail (frame != NULL && result != NULL && matched_index != NULL, FALSE);
  memset (result, 0, sizeof (*result));
  result->subtemplate = -1;
  *matched_index = -1;
  if (updated_blob)
    *updated_blob = NULL;

  f = g_new (GoodixChicagoRuntimeFrame, 1);
  probe = goodix_chicago_runtime_prepare_probe (self->preprocessor,
                                                &self->feature_state, frame,
                                                TRUE, f, &rr, error);
  result->status = f->status;
  result->quality = f->quality;
  result->coverage = f->coverage;
  if (!probe)
    {
      if (rr == GOODIX_CHICAGO_RUNTIME_REJECT_NONE)
        return FALSE;   /* internal error */
      result->reject = rr == GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES ?
                       OC_REJECT_NO_FEATURES : reject_from_status (f->status);
      return TRUE;
    }

  memset (&best, 0, sizeof (best));
  for (guint i = 0; i < n_templates; i++)
    {
      g_return_val_if_fail (templates[i] != NULL, FALSE);
      memset (&res, 0, sizeof (res));
      if (!goodix_chicago_runtime_match (probe, templates[i], &res, error))
        return FALSE;
      if (i == 0 || (res.score > 0 && res.score > best.score))
        {
          best = res;
          if (res.score > 0)
            *matched_index = (gint) i;
        }
    }
  result->score = best.score;
  result->subtemplate = n_templates ? best.selected_index : -1;
  result->match = *matched_index >= 0;
  /* score > 0 -> template study */
  if (result->match && updated_blob)
    return goodix_chicago_runtime_study (probe, templates[*matched_index], &best,
                                         updated_blob, error);
  return TRUE;
}

/* ---- storage helpers ---------------------------------------------------- */

GVariant *
oc_print_data_new (OcSession   *self,
                   const guint8 sensor_id[OC_SENSOR_ID_LEN],
                   GBytes      *template_blob)
{
  g_return_val_if_fail (self != NULL, NULL);
  return goodix_chicago_print_data_build (
    sensor_id, goodix_chicago_preprocessor_get_calibration (self->preprocessor),
    template_blob);
}

GBytes *
oc_print_data_get_template (GVariant    *print_data,
                            const guint8 sensor_id[OC_SENSOR_ID_LEN],
                            GError     **error)
{
  GBytes *packed = NULL;

  if (!goodix_chicago_print_data_parse (print_data, sensor_id, NULL, &packed,
                                        error))
    return NULL;
  return packed;
}
