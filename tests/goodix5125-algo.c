/*
 * goodix5125: offline replay of recorded frames through the driver's
 * algorithm layer (goodix5125-algo.c), without USB.
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * Every frame goes the driver's way: 12-bit packing as sent by the sensor ->
 * goodix5125_image_decode_transposed() -> goodix5125_algo_*().  The same
 * frames run in parallel through the openchicago API directly; every
 * decision, score, learned template and the saved state must be identical.
 *
 *   A. plain scenario (plain enrolment of 12 natural touches, then every
 *      other frame verified with template study);
 *   B. the driver's enrolment (engine protocol, also with frame
 *      pairs) and identify against {impostor print, own print};
 *   C. state file: save, load at "open", rebase onto a new ImageBase,
 *      foreign sensor refused, corrupt file refused;
 *   D. learned-template store (fprintd never re-saves prints): enrol, verify
 *      the enrolled print with template study, then a new algorithm object and
 *      the print reloaded from its bytes must use the learned template; a
 *      corrupt, truncated or foreign entry falls back to the enrolled one;
 *      expiry of unused / excess entries.
 *
 * Data: GOODIX5125_TEST_FRAMES = directory with frames_raw.bin, bg_raw.bin
 * (u16, 80 x 64 transport order) and meta_raw.txt (label per frame).
 * Without it the test is skipped (exit code 77).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>
#include <utime.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "drivers_api.h"
#include "goodix5125-algo.h"
#include "goodix5125-image.h"

#define PX OC_FRAME_PIXELS
#define MAXREC 256

static guint16 * frames;   /* decoded by the driver path: 64 x 80 */
static guint16 *bases;
static char labels[MAXREC][16];
static int nrec;
static int failures;

static const guint8 sensor_id[OC_SENSOR_ID_LEN] = {
  0x44, 0x4e, 0x45, 0x31, 0x33, 0x36, 0x2e, 0x00,
  0x8a, 0x7d, 0x98, 0xab, 0x3e, 0xcc, 0x92, 0x0b,
};

#define CHECK(cond, ...) \
  G_STMT_START { \
    if (!(cond)) \
      { \
        failures++; \
        printf ("FAIL: " __VA_ARGS__); \
        printf ("\n"); \
      } \
  } G_STMT_END

#define FRAME(i) (frames + (gsize) (i) * PX)
#define BASE(i) (bases + (gsize) (i) * PX)

/* Sensor packing (inverse of goodix5125_image_decode): 6 bytes, 4 pixels. */
static void
pack_frame (const guint16 *stream, guint8 *packed)
{
  for (gsize s = 0, d = 0; d < GOODIX5125_IMAGE_PACKED_SIZE; s += 4, d += 6)
    {
      guint a = MIN (stream[s], 4095), b = MIN (stream[s + 1], 4095);
      guint c = MIN (stream[s + 2], 4095), e = MIN (stream[s + 3], 4095);

      packed[d] = ((a >> 8) & 0x0f) | ((b & 0x0f) << 4);
      packed[d + 1] = a & 0xff;
      packed[d + 2] = c & 0xff;
      packed[d + 3] = b >> 4;
      packed[d + 4] = e >> 4;
      packed[d + 5] = ((c >> 8) & 0x0f) | ((e & 0x0f) << 4);
    }
}

/* transport frame -> packed -> the driver's decoder; checked against
 * oc_frame_transpose() of the same frame */
static void
driver_decode (const guint16 *stream, guint16 *out)
{
  guint8 packed[GOODIX5125_IMAGE_PACKED_SIZE];
  guint16 clamped[PX], expected[PX];

  g_autoptr(GError) error = NULL;

  pack_frame (stream, packed);
  CHECK (goodix5125_image_decode_transposed (packed, sizeof (packed), out, PX,
                                             &error), "decode");
  for (guint i = 0; i < PX; i++)
    clamped[i] = MIN (stream[i], 4095);
  oc_frame_transpose (clamped, expected);
  CHECK (memcmp (out, expected, sizeof (expected)) == 0,
         "driver decoder != oc_frame_transpose");
}

static gboolean
load (const char *dir)
{
  g_autofree gchar *fp = g_build_filename (dir, "frames_raw.bin", NULL);
  g_autofree gchar *bp = g_build_filename (dir, "bg_raw.bin", NULL);
  g_autofree gchar *mp = g_build_filename (dir, "meta_raw.txt", NULL);
  g_autofree gchar *fdata = NULL;
  g_autofree gchar *bdata = NULL;
  gsize flen, blen;
  FILE *m;
  int dummy;

  if (!g_file_get_contents (fp, &fdata, &flen, NULL) ||
      !g_file_get_contents (bp, &bdata, &blen, NULL) || blen != flen)
    return FALSE;
  nrec = MIN (MAXREC, (int) (flen / (PX * 2)));
  frames = g_new (guint16, (gsize) nrec * PX);
  bases = g_new (guint16, (gsize) nrec * PX);
  for (int i = 0; i < nrec; i++)
    {
      driver_decode ((const guint16 *) fdata + (gsize) i * PX, FRAME (i));
      driver_decode ((const guint16 *) bdata + (gsize) i * PX, BASE (i));
    }
  if (!(m = fopen (mp, "r")))
    return FALSE;
  for (int i = 0; i < nrec; i++)
    if (fscanf (m, "%15s %d", labels[i], &dummy) != 2)
      break;
  fclose (m);
  return TRUE;
}

static GBytes *
print_template (GVariant *print)
{
  g_autoptr(GError) error = NULL;
  GBytes *blob = oc_print_data_get_template (print, sensor_id, &error);

  CHECK (blob != NULL, "print data: %s", error ? error->message : "?");
  return blob;
}

/* The state file = "G5OC" header + oc_session_save_state(). */
static gboolean
state_file_matches (const gchar *path, OcSession *direct)
{
  g_autofree gchar *contents = NULL;

  g_autoptr(GBytes) state = oc_session_save_state (direct);
  gsize len = 0;

  if (!g_file_get_contents (path, &contents, &len, NULL) || len < 24)
    return FALSE;
  return len - 24 == g_bytes_get_size (state) &&
         memcmp (contents + 8, sensor_id, OC_SENSOR_ID_LEN) == 0 &&
         memcmp (contents + 24, g_bytes_get_data (state, NULL), len - 24) == 0;
}

static gboolean
same_result (const OcVerifyResult *a, const OcVerifyResult *b)
{
  return a->match == b->match && a->score == b->score &&
         a->subtemplate == b->subtemplate && a->reject == b->reject &&
         a->status == b->status && a->quality == b->quality &&
         a->coverage == b->coverage;
}

static Goodix5125Algo *
new_algo (const gchar *dir, const gchar *name, const guint16 *base)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);

  g_autoptr(GError) error = NULL;
  Goodix5125Algo *algo = goodix5125_algo_new (path);

  CHECK (goodix5125_algo_load_state (algo, &error), "load (no file)");
  CHECK (goodix5125_algo_activate (algo, sensor_id, base, NULL, &error),
         "activate: %s", error ? error->message : "");
  return algo;
}

/* A. plain enrolment of 12 touches, verify with study */
static void
scenario_plain (const gchar *dir, const int *nat, int nnat)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Goodix5125Algo) algo = new_algo (dir, "a.state", BASE (nat[0]));
  g_autoptr(OcSession) direct = oc_session_new (BASE (nat[0]), &error);
  g_autoptr(GVariant) print = NULL;
  g_autoptr(GBytes) blob = NULL;
  int is_gal[MAXREC] = { 0 };
  int offered = 0, usable = 0, hits[3] = { 0 }, studies = 0;

  CHECK (goodix5125_algo_enroll_begin (algo, OC_ENROLL_PLAIN, 12, &error) &&
         oc_enroll_begin (direct, OC_ENROLL_PLAIN, 12, &error), "begin");
  for (int k = 0; k < nnat && offered < 12; k++)
    {
      Goodix5125EnrollStep step;
      OcEnrollResult er;

      CHECK (goodix5125_algo_enroll_add (algo, FRAME (nat[k]), NULL, &step, &error) &&
             oc_enroll_add (direct, FRAME (nat[k]), &er, &error), "enroll add");
      CHECK (memcmp (&step.result, &er, sizeof (er)) == 0, "A: enroll rec %d differs", nat[k]);
      goodix5125_enroll_step_clear (&step);
      is_gal[nat[k]] = 1;
      offered += er.reject == OC_REJECT_NONE || er.reject == OC_REJECT_MERGE_FAILURE;
    }
  print = goodix5125_algo_enroll_finish (algo, &error);
  blob = oc_enroll_finish (direct, &error);
  CHECK (print && blob, "finish");
  {
    g_autoptr(GBytes) t = print_template (print);

    CHECK (t && g_bytes_equal (t, blob), "A: gallery template differs");
  }

  for (int i = 0; i < nrec; i++)
    {
      Goodix5125MatchStep step;
      OcVerifyResult vr;
      GBytes *updated = NULL;
      int g = !strcmp (labels[i], "natural") ? 0 : !strcmp (labels[i], "genuine") ? 1 : 2;

      if (is_gal[i])
        continue;
      CHECK (goodix5125_algo_match (algo, FRAME (i), &print, 1, &step, &error),
             "match: %s", error ? error->message : "");
      CHECK (oc_verify (direct, blob, FRAME (i), &vr, &updated, &error), "verify");
      CHECK (same_result (&step.result, &vr), "A: rec %d score %d/%d", i,
             step.result.score, vr.score);
      CHECK ((step.retry != NULL) == (vr.reject != OC_REJECT_NONE) &&
             (step.matched == 0) == vr.match, "A: rec %d decision", i);
      CHECK ((step.updated != NULL) == (updated != NULL), "A: rec %d study", i);
      if (vr.reject == OC_REJECT_NONE)
        usable++;
      hits[g] += vr.match;
      if (updated)
        {
          g_autoptr(GBytes) t = print_template (step.updated);

          CHECK (t && g_bytes_equal (t, updated), "A: rec %d learned template", i);
          /* as the driver does: the matched print takes the learned data */
          g_clear_pointer (&print, g_variant_unref);
          print = g_steal_pointer (&step.updated);
          g_bytes_unref (blob);
          blob = updated;
          studies++;
        }
      goodix5125_match_step_clear (&step);
    }
  CHECK (goodix5125_algo_save_state (algo, &error), "save");
  CHECK (state_file_matches (goodix5125_algo_get_state_path (algo), direct),
         "A: saved state differs");
  CHECK (hits[2] == 0, "A: impostor matched");
  printf ("A (plain n12): %d probes usable, natural %d, genuine %d, impostor %d "
          "matches, %d study updates\n", usable, hits[0], hits[1], hits[2], studies);
}

/* B. engine enrolment (+ pairs) and identify */
static GVariant *
engine_enrol (Goodix5125Algo *algo, OcSession *direct, const int *nat, int nnat,
              gboolean pairs, int *used, int *retries)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) blob = NULL;
  GVariant *print;
  gboolean complete = FALSE;
  int k;

  CHECK (goodix5125_algo_enroll_begin (algo, OC_ENROLL_ENGINE, OC_ENGINE_STAGES, &error) &&
         oc_enroll_begin (direct, OC_ENROLL_ENGINE, OC_ENGINE_STAGES, &error), "B begin");
  for (k = 0; k + (pairs ? 1 : 0) < nnat && !complete; k += pairs ? 2 : 1)
    {
      const guint16 *extra = pairs ? FRAME (nat[k + 1]) : NULL;
      Goodix5125EnrollStep step;
      OcEnrollResult er;

      CHECK (goodix5125_algo_enroll_add (algo, FRAME (nat[k]), extra, &step, &error),
             "B enroll add");
      CHECK (pairs ? oc_enroll_add_pair (direct, FRAME (nat[k]), extra, &er, &error) :
             oc_enroll_add (direct, FRAME (nat[k]), &er, &error), "B oc add");
      CHECK (memcmp (&step.result, &er, sizeof (er)) == 0, "B: enroll rec %d differs", nat[k]);
      /* what the driver reports: progress, or a retry of the libfprint domain */
      CHECK (er.accepted || er.complete ?
             step.retry == NULL :
             step.retry && step.retry->domain == FP_DEVICE_RETRY,
             "B: rec %d retry mapping", nat[k]);
      CHECK (er.reject != OC_REJECT_TIP ||
             g_error_matches (step.retry, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER),
             "B: tip is not CENTER_FINGER");
      *retries += step.retry != NULL;
      complete = step.complete;
      goodix5125_enroll_step_clear (&step);
    }
  *used = k;
  /* 15 frame pairs of the dataset are not always enough to complete */
  CHECK (complete || pairs, "B: enrolment incomplete");
  print = goodix5125_algo_enroll_finish (algo, &error);
  blob = oc_enroll_finish (direct, &error);
  {
    g_autoptr(GBytes) t = print ? print_template (print) : NULL;

    CHECK (t && blob && g_bytes_equal (t, blob), "B: template differs");
  }
  return print;
}

static void
scenario_engine (const gchar *dir, const int *nat, int nnat)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(Goodix5125Algo) algo = new_algo (dir, "b.state", BASE (nat[0]));
  g_autoptr(OcSession) direct = oc_session_new (BASE (nat[0]), &error);
  g_autoptr(GVariant) own = NULL;
  g_autoptr(GVariant) other = NULL;
  g_autoptr(GBytes) other_blob = NULL;
  int used = 0, retries = 0, n = 0, hits[3] = { 0 }, own_hits = 0;
  int imp[MAXREC] = { 0 }, nimp = 0;

  /* the pair path in a throw-away pair of sessions */
  {
    g_autoptr(Goodix5125Algo) pa = new_algo (dir, "p.state", BASE (nat[0]));
    g_autoptr(OcSession) pd = oc_session_new (BASE (nat[0]), &error);
    g_autoptr(GVariant) pp = NULL;
    int pu = 0, pr = 0;

    pp = engine_enrol (pa, pd, nat, nnat, TRUE, &pu, &pr);
    printf ("B (pairs): %d natural frames, %d retries\n", pu, pr);
  }

  own = engine_enrol (algo, direct, nat, nnat, FALSE, &used, &retries);
  printf ("B (engine): %d natural touches, %d retries\n", used, retries);

  /* impostor print: plain enrolment in a separate session */
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "impostor"))
      imp[nimp++] = i;
  {
    g_autoptr(OcSession) s = oc_session_new (BASE (imp[0]), &error);
    OcEnrollResult er = { 0 };

    CHECK (oc_enroll_begin (s, OC_ENROLL_PLAIN, 8, &error), "imp begin");
    for (int k = 0; k < nimp && !er.complete; k++)
      CHECK (oc_enroll_add (s, FRAME (imp[k]), &er, &error), "imp add");
    other_blob = oc_enroll_finish (s, &error);
    other = oc_print_data_new (s, sensor_id, other_blob);
  }

  for (int i = 0; i < nrec; i++)
    {
      GVariant *gallery[2] = { other, own };
      g_autoptr(GBytes) own_blob = print_template (own);
      GBytes *blobs[2] = { other_blob, own_blob };
      Goodix5125MatchStep step;
      OcVerifyResult vr;
      GBytes *updated = NULL;
      gint idx;
      int g = !strcmp (labels[i], "natural") ? 0 : !strcmp (labels[i], "genuine") ? 1 : 2;
      int k;

      for (k = 0; k < used && nat[k] != i; k++)
        ;
      if (k < used)
        continue;
      CHECK (goodix5125_algo_match (algo, FRAME (i), gallery, 2, &step, &error), "B match");
      CHECK (oc_identify (direct, blobs, 2, FRAME (i), &vr, &idx, &updated, &error),
             "B identify");
      CHECK (same_result (&step.result, &vr) && step.matched == idx,
             "B: rec %d idx %d/%d score %d/%d", i, step.matched, idx,
             step.result.score, vr.score);
      CHECK ((step.updated != NULL) == (updated != NULL), "B: rec %d study", i);
      if (updated && step.updated)
        {
          g_autoptr(GBytes) t = print_template (step.updated);

          CHECK (g_bytes_equal (t, updated), "B: rec %d learned template", i);
          if (idx == 1)
            {
              g_clear_pointer (&own, g_variant_unref);
              own = g_steal_pointer (&step.updated);
            }
          else
            {
              g_clear_pointer (&other, g_variant_unref);
              g_clear_pointer (&other_blob, g_bytes_unref);
              other = g_steal_pointer (&step.updated);
              other_blob = g_bytes_ref (updated);
            }
        }
      g_clear_pointer (&updated, g_bytes_unref);
      goodix5125_match_step_clear (&step);
      if (vr.reject != OC_REJECT_NONE)
        continue;
      n++;
      own_hits += idx == 1;
      hits[g] += idx >= 0;
    }
  CHECK (goodix5125_algo_save_state (algo, &error), "B save");
  CHECK (state_file_matches (goodix5125_algo_get_state_path (algo), direct),
         "B: saved state differs");
  printf ("B (identify): %d probes, own print %d (natural+genuine hits %d+%d), "
          "impostor frames matched %d (own print never: %s)\n", n, own_hits,
          hits[0], hits[1], hits[2], own_hits <= hits[0] + hits[1] ? "yes" : "NO");
  CHECK (own_hits <= hits[0] + hits[1], "B: impostor frame matched the own print");
}

/* C. state file across "open" */
static void
scenario_state (const gchar *dir, const int *nat, int nnat)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = g_build_filename (dir, "c.state", NULL);
  g_autoptr(Goodix5125Algo) first = new_algo (dir, "c.state", BASE (nat[0]));
  g_autoptr(OcSession) direct = oc_session_new (BASE (nat[0]), &error);
  const guint16 *new_base = BASE (nat[nnat - 1]);
  g_autoptr(GVariant) print = NULL;
  g_autoptr(GBytes) blob = NULL;
  int used = 0, retries = 0, same = 0;

  print = engine_enrol (first, direct, nat, nnat, FALSE, &used, &retries);
  CHECK (goodix5125_algo_save_state (first, &error), "C save");
  goodix5125_algo_deactivate (first);
  blob = print_template (print);

  /* next open(): load, activate with a fresh ImageBase = rebase */
  {
    g_autoptr(Goodix5125Algo) again = goodix5125_algo_new (path);

    CHECK (goodix5125_algo_load_state (again, &error), "C load: %s",
           error ? error->message : "");
    CHECK (goodix5125_algo_activate (again, sensor_id, new_base, NULL, &error),
           "C activate");
    CHECK (oc_session_rebase (direct, new_base, &error), "C rebase");
    CHECK (state_file_matches (path, direct), "C: rebased state differs");
    for (int i = 0; i < nrec; i += 5)
      {
        Goodix5125MatchStep step;
        OcVerifyResult vr;

        CHECK (goodix5125_algo_match (again, FRAME (i), &print, 1, &step, &error) &&
               oc_verify (direct, blob, FRAME (i), &vr, NULL, &error), "C match");
        same += same_result (&step.result, &vr);
        CHECK (same_result (&step.result, &vr), "C: rec %d after restore", i);
        goodix5125_match_step_clear (&step);
      }
  }

  /* another sensor: refused, file kept */
  {
    g_autoptr(Goodix5125Algo) other = goodix5125_algo_new (path);
    guint8 foreign[OC_SENSOR_ID_LEN];
    g_autoptr(GError) e2 = NULL;

    memcpy (foreign, sensor_id, sizeof foreign);
    foreign[0] ^= 0xff;
    CHECK (goodix5125_algo_load_state (other, &error), "C load 2");
    CHECK (!goodix5125_algo_activate (other, foreign, new_base, NULL, &e2) &&
           g_error_matches (e2, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED),
           "C: foreign sensor accepted");
  }

  /* corrupt file: refused at load */
  {
    g_autofree gchar *contents = NULL;
    gsize len = 0;
    g_autoptr(Goodix5125Algo) bad = goodix5125_algo_new (path);
    g_autoptr(GError) e3 = NULL;

    g_file_get_contents (path, &contents, &len, NULL);
    contents[len / 2] ^= 0x01;
    g_file_set_contents (path, contents, len, NULL);
    CHECK (!goodix5125_algo_load_state (bad, &e3) && e3, "C: corrupt state accepted");
  }
  printf ("C (state): restore + rebase, %d frames identical, foreign/corrupt refused\n",
          same);
}

/* D. learned-template store: template study across "sessions" (fprintd) */

#define LEARNED_HEADER 88   /* magic, version, sensor id, key, payload SHA-256 */

static GBytes *
learned_file_template (const gchar *path)
{
  g_autofree gchar *contents = NULL;

  g_autoptr(GBytes) payload = NULL;
  g_autoptr(GVariant) print = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &contents, &len, NULL) || len <= LEARNED_HEADER)
    return NULL;
  payload = g_bytes_new (contents + LEARNED_HEADER, len - LEARNED_HEADER);
  print = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE ("(uayayay)"),
                                                        payload, FALSE));
  return oc_print_data_get_template (print, sensor_id, NULL);
}

static void
make_file (const gchar *dir, const gchar *name, gint64 age_s)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);
  struct utimbuf t;

  t.actime = t.modtime = (time_t) (g_get_real_time () / G_USEC_PER_SEC - age_s);
  CHECK (g_file_set_contents (path, "x", 1, NULL) && g_utime (path, &t) == 0,
         "D: fake %s", name);
}

/* G_DEBUG=fatal-warnings: the expected fallback warnings are counted */
static int learned_warnings, other_warnings;

static void
count_warning (const gchar   *domain,
               GLogLevelFlags level,
               const gchar   *message,
               gpointer       user_data)
{
  if (strstr (message, "ignoring the learned template"))
    {
      learned_warnings++;
    }
  else
    {
      other_warnings++;
      printf ("unexpected warning: %s\n", message);
    }
}

static gboolean
exists_in (const gchar *dir, const gchar *name)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);

  return g_file_test (path, G_FILE_TEST_EXISTS);
}

static gchar *
fake_name (char c, const gchar *suffix)
{
  char hex[65];

  memset (hex, c, 64);
  hex[64] = 0;
  return g_strconcat (hex, suffix, NULL);
}

/* one probe through the driver layer and in parallel through openchicago
 * with the template the driver is expected to use (@expect_learned) */
static void
d_probe (Goodix5125Algo *algo, OcSession *direct, GVariant *print, int i,
         GBytes **current, GBytes *original, gboolean expect_learned,
         const gchar *what, int *studies, int *hits)
{
  g_autoptr(GError) error = NULL;
  Goodix5125MatchStep step;
  OcVerifyResult vr;
  GBytes *updated = NULL;
  GBytes *used = expect_learned ? *current : original;

  CHECK (goodix5125_algo_match (algo, FRAME (i), &print, 1, &step, &error),
         "D %s match: %s", what, error ? error->message : "");
  CHECK (oc_verify (direct, used, FRAME (i), &vr, &updated, &error), "D verify");
  CHECK (step.n_learned == (expect_learned ? 1u : 0u), "D %s: rec %d learned %u",
         what, i, step.n_learned);
  CHECK (same_result (&step.result, &vr), "D %s: rec %d score %d/%d", what, i,
         step.result.score, vr.score);
  CHECK ((step.updated != NULL) == (updated != NULL) &&
         (updated == NULL || step.stored), "D %s: rec %d study/store", what, i);
  *hits += vr.match;
  if (updated)
    {
      g_autofree gchar *path = goodix5125_algo_learned_path (algo, print);
      g_autoptr(GBytes) stored = learned_file_template (path);

      CHECK (stored && g_bytes_equal (stored, updated), "D %s: rec %d stored template", what, i);
      g_bytes_unref (*current);
      *current = updated;
      (*studies)++;
    }
  goodix5125_match_step_clear (&step);
}

static void
scenario_learned (const gchar *dir, const int *nat, int nnat)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *state = g_build_filename (dir, "d.state", NULL);
  g_autofree gchar *store = g_build_filename (dir, "learned", NULL);
  g_autoptr(OcSession) direct = oc_session_new (BASE (nat[0]), &error);
  g_autoptr(GVariant) print = NULL;
  g_autoptr(GBytes) saved = NULL;       /* the print as fprintd stores it */
  g_autoptr(GBytes) original = NULL;
  g_autoptr(GBytes) current = NULL;     /* last learned template */
  g_autofree gchar *entry = NULL;
  g_autofree gchar *fa = fake_name ('a', ".tpl"), *fb = fake_name ('b', ".tpl");
  g_autofree gchar *fc = fake_name ('c', ".tpl"), *fd = fake_name ('d', ".tpl");
  g_autofree gchar *fe = fake_name ('e', ".tpl.AbC123");
  const guint16 *new_base = BASE (nat[nnat - 1]);
  int used = 0, retries = 0, studies1 = 0, studies2 = 0, hits1 = 0, hits2 = 0;
  int is_used[MAXREC] = { 0 }, s1[MAXREC], s2[MAXREC], n1 = 0, n2 = 0;
  int first_impostor = 0;
  GLogLevelFlags fatal;
  guint handler;
  GStatBuf st;

  {
    g_autoptr(Goodix5125Algo) first = new_algo (dir, "d.state", BASE (nat[0]));
    g_autoptr(GVariant) enrolled = engine_enrol (first, direct, nat, nnat, FALSE,
                                                 &used, &retries);

    for (int k = 0; k < used; k++)
      is_used[nat[k]] = 1;
    for (int i = 0; i < nrec; i++)
      if (strcmp (labels[i], "impostor") != 0)
        first_impostor = i + 1;
    for (int i = 0; i < nrec; i++)
      if (!is_used[i])
        {
          /* own frames alternate, impostor frames split in halves */
          gboolean one = !strcmp (labels[i], "impostor") ?
                         i < (first_impostor + nrec) / 2 : i % 2 == 0;

          if (one)
            s1[n1++] = i;
          else
            s2[n2++] = i;
        }
    saved = g_variant_get_data_as_bytes (enrolled);
    original = print_template (enrolled);
    current = g_bytes_ref (original);

    /* store limits 3 entries / 30 days; stale entries of deleted prints */
    g_mkdir_with_parents (store, 0700);
    make_file (store, fa, 40 * 86400);  /* expired */
    make_file (store, fb, 10 * 86400);  /* oldest: over the limit */
    make_file (store, fc, 5 * 86400);
    make_file (store, fd, 1 * 86400);
    make_file (store, fe, 2 * 3600);    /* interrupted write */
    make_file (store, "README", 400 * 86400);   /* not ours */
    goodix5125_algo_set_learned_store (first, store, 3, 30);

    /* session 1: fprintd passes the enrolled print every time */
    print = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE ("(uayayay)"),
                                                          saved, FALSE));
    for (int k = 0; k < n1; k++)
      d_probe (first, direct, print, s1[k], &current, original, studies1 > 0,
               "session 1", &studies1, &hits1);
    CHECK (studies1 > 0, "D: no template study in session 1");
    CHECK (goodix5125_algo_save_state (first, &error), "D save");
    entry = goodix5125_algo_learned_path (first, print);
  }

  /* expiry after the first store write */
  CHECK (!exists_in (store, fa) && !exists_in (store, fb) && !exists_in (store, fe) &&
         exists_in (store, fc) && exists_in (store, fd) && exists_in (store, "README"),
         "D: store expiry/limit");
  CHECK (g_stat (entry, &st) == 0 && (st.st_mode & 0777) == 0600, "D: entry mode %o",
         st.st_mode & 0777);

  /* session 2: new algorithm object, print reloaded from its bytes */
  {
    g_autoptr(Goodix5125Algo) again = goodix5125_algo_new (state);
    g_autoptr(GVariant) reloaded = g_variant_ref_sink (
      g_variant_new_from_bytes (G_VARIANT_TYPE ("(uayayay)"), saved, FALSE));
    g_autofree gchar *contents = NULL;
    gsize len = 0;

    CHECK (goodix5125_algo_load_state (again, &error) &&
           goodix5125_algo_activate (again, sensor_id, new_base, NULL, &error),
           "D reopen: %s", error ? error->message : "");
    CHECK (oc_session_rebase (direct, new_base, &error), "D rebase");
    goodix5125_algo_set_learned_store (again, store, 3, 30);
    CHECK (!g_bytes_equal (current, original), "D: learned = enrolled template");
    for (int k = 0; k < n2; k++)
      d_probe (again, direct, reloaded, s2[k], &current, original, TRUE,
               "session 2", &studies2, &hits2);

    fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);
    handler = g_log_set_handler ("libfprint-goodix5125", G_LOG_LEVEL_WARNING,
                                 count_warning, NULL);

    /* corrupt payload: warning, the enrolled template is used */
    CHECK (g_file_get_contents (entry, &contents, &len, NULL) && len > LEARNED_HEADER,
           "D read entry");
    contents[LEARNED_HEADER + (len - LEARNED_HEADER) / 2] ^= 0x10;
    CHECK (g_file_set_contents (entry, contents, len, NULL), "D corrupt");
    d_probe (again, direct, reloaded, s2[0], &current, original, FALSE, "corrupt",
             &studies2, &hits2);
    g_file_set_contents (entry, "G5L", 3, NULL);      /* truncated */
    d_probe (again, direct, reloaded, s2[1], &current, original, FALSE,
             "truncated", &studies2, &hits2);

    /* entry of another sensor: warning, the enrolled template is used */
    contents[LEARNED_HEADER + (len - LEARNED_HEADER) / 2] ^= 0x10;   /* repaired */
    contents[8] ^= 0xff;
    CHECK (g_file_set_contents (entry, contents, len, NULL), "D foreign");
    d_probe (again, direct, reloaded, s2[2], &current, original, FALSE,
             "foreign", &studies2, &hits2);

    g_log_remove_handler ("libfprint-goodix5125", handler);
    g_log_set_always_fatal (fatal);
    CHECK (learned_warnings == 3 && other_warnings == 0, "D: warnings %d/%d",
           learned_warnings, other_warnings);
  }

  {
    g_autoptr(GDir) d = g_dir_open (store, 0, NULL);
    const gchar *name;

    while (d && (name = g_dir_read_name (d)))
      {
        g_autofree gchar *p = g_build_filename (store, name, NULL);

        g_unlink (p);
      }
    g_rmdir (store);
  }
  printf ("D (learned store): session 1 %d studies, %d matches; session 2 %d probes "
          "through the learned template, %d matches, %d studies; expiry, "
          "corrupt/truncated/foreign entries -> enrolled template\n",
          studies1, hits1, n2, hits2, studies2);
}

int
main (int argc, char **argv)
{
  const gchar *data = g_getenv ("GOODIX5125_TEST_FRAMES");
  g_autofree gchar *dir = NULL;
  int nat[MAXREC], nnat = 0;

  if (!data && argc > 1)
    data = argv[1];
  if (!data || !load (data))
    {
      printf ("no recorded frames (GOODIX5125_TEST_FRAMES): skipped\n");
      return 77;
    }
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "natural"))
      nat[nnat++] = i;
  dir = g_dir_make_tmp ("goodix5125-algo-XXXXXX", NULL);

  scenario_plain (dir, nat, nnat);
  scenario_engine (dir, nat, nnat);
  scenario_state (dir, nat, nnat);
  scenario_learned (dir, nat, nnat);

  {
    const gchar *names[] = { "a.state", "b.state", "c.state", "d.state", "p.state" };

    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
      {
        g_autofree gchar *p = g_build_filename (dir, names[i], NULL);

        g_unlink (p);
      }
    g_rmdir (dir);
  }
  g_free (frames);
  g_free (bases);
  printf ("%s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
