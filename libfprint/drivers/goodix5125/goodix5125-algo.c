/*
 * Goodix 27c6:5125 - openchicago glue (no USB)
 *
 * Copyright (C) 2026 RuVl <ru.vlad.13@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include "drivers_api.h"
#include "goodix5125-algo.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#include <glib/gstdio.h>

#define STATE_HEADER_SIZE (4 + 4 + OC_SENSOR_ID_LEN)

#define DIGEST_LEN 32
#define KEY_HEX_LEN (DIGEST_LEN * 2)
#define LEARNED_PRINT_TYPE "(uayayay)"
#define LEARNED_HEADER_SIZE (4 + 4 + OC_SENSOR_ID_LEN + DIGEST_LEN + DIGEST_LEN)
#define LEARNED_OFF_SENSOR 8
#define LEARNED_OFF_KEY (LEARNED_OFF_SENSOR + OC_SENSOR_ID_LEN)
#define LEARNED_OFF_SUM (LEARNED_OFF_KEY + DIGEST_LEN)
/* leftovers of an interrupted atomic write (<entry>.XXXXXX) */
#define LEARNED_TMP_MAX_AGE_S 3600

struct _Goodix5125Algo
{
  gchar     *state_path;
  OcSession *loaded;                        /* from the file, not yet rebased */
  guint8     loaded_sensor_id[OC_SENSOR_ID_LEN];
  OcSession *session;                       /* active */
  guint8     sensor_id[OC_SENSOR_ID_LEN];
  gboolean   enrolling;
  gchar     *learned_dir;                   /* NULL: store disabled */
  guint      learned_max_entries;
  guint      learned_max_age_days;
};

void
goodix5125_enroll_step_clear (Goodix5125EnrollStep *step)
{
  g_clear_error (&step->retry);
}

void
goodix5125_match_step_clear (Goodix5125MatchStep *step)
{
  g_clear_error (&step->retry);
  g_clear_pointer (&step->updated, g_variant_unref);
}

Goodix5125Algo *
goodix5125_algo_new (const gchar *state_path)
{
  Goodix5125Algo *self = g_new0 (Goodix5125Algo, 1);

  g_return_val_if_fail (state_path != NULL, NULL);
  self->state_path = g_strdup (state_path);
  return self;
}

void
goodix5125_algo_free (Goodix5125Algo *self)
{
  if (!self)
    return;
  g_clear_pointer (&self->loaded, oc_session_free);
  g_clear_pointer (&self->session, oc_session_free);
  g_free (self->state_path);
  g_free (self->learned_dir);
  g_free (self);
}

const gchar *
goodix5125_algo_get_state_path (Goodix5125Algo *self)
{
  return self->state_path;
}

gboolean
goodix5125_algo_is_active (Goodix5125Algo *self)
{
  return self->session != NULL;
}

gboolean
goodix5125_algo_load_state (Goodix5125Algo *self,
                            GError        **error)
{
  g_autofree gchar *contents = NULL;

  g_autoptr(GError) local = NULL;
  g_autoptr(GBytes) state = NULL;
  gsize length = 0;
  guint32 version;

  g_clear_pointer (&self->loaded, oc_session_free);
  if (!g_file_get_contents (self->state_path, &contents, &length, &local))
    {
      if (g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return TRUE;
      g_propagate_prefixed_error (error, g_steal_pointer (&local),
                                  "goodix5125: cannot read the state file: ");
      return FALSE;
    }

  if (length < STATE_HEADER_SIZE ||
      memcmp (contents, GOODIX5125_ALGO_STATE_MAGIC, 4) != 0)
    goto invalid;
  memcpy (&version, contents + 4, sizeof (version));
  if (GUINT32_FROM_LE (version) != GOODIX5125_ALGO_STATE_VERSION)
    goto invalid;
  state = g_bytes_new (contents + STATE_HEADER_SIZE, length - STATE_HEADER_SIZE);
  self->loaded = oc_session_new_from_state (state, &local);
  if (!self->loaded)
    goto invalid;
  memcpy (self->loaded_sensor_id, contents + 8, OC_SENSOR_ID_LEN);
  fp_dbg ("loaded the openchicago state from %s", self->state_path);
  return TRUE;

invalid:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "goodix5125: %s is not a valid state file (%s); remove it to "
               "calibrate the sensor from scratch",
               self->state_path, local ? local->message : "bad header");
  return FALSE;
}

gboolean
goodix5125_algo_save_state (Goodix5125Algo *self,
                            GError        **error)
{
  g_autofree gchar *directory = NULL;

  g_autoptr(GBytes) state = NULL;
  g_autoptr(GByteArray) out = NULL;
  guint32 version = GUINT32_TO_LE (GOODIX5125_ALGO_STATE_VERSION);
  const guint8 *data;
  gsize length;

  g_return_val_if_fail (self->session != NULL, FALSE);

  state = oc_session_save_state (self->session);
  data = g_bytes_get_data (state, &length);
  out = g_byte_array_sized_new (STATE_HEADER_SIZE + length);
  g_byte_array_append (out, (const guint8 *) GOODIX5125_ALGO_STATE_MAGIC, 4);
  g_byte_array_append (out, (const guint8 *) &version, 4);
  g_byte_array_append (out, self->sensor_id, OC_SENSOR_ID_LEN);
  g_byte_array_append (out, data, length);

  directory = g_path_get_dirname (self->state_path);
  if (g_mkdir_with_parents (directory, 0700) != 0)
    {
      int saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "goodix5125: cannot create %s: %s", directory,
                   g_strerror (saved_errno));
      return FALSE;
    }
  return g_file_set_contents_full (self->state_path, (const gchar *) out->data,
                                   out->len, G_FILE_SET_CONTENTS_CONSISTENT,
                                   0600, error);
}

gboolean
goodix5125_algo_activate (Goodix5125Algo *self,
                          const guint8    sensor_id[OC_SENSOR_ID_LEN],
                          const guint16   image_base[OC_FRAME_PIXELS],
                          const gchar    *calidata_path,
                          GError        **error)
{
  g_autoptr(OcSession) session = NULL;

  g_return_val_if_fail (sensor_id != NULL && image_base != NULL, FALSE);

  if (self->session)
    {
      /* re-initialised sensor within one open(): new ImageBase only */
      if (memcmp (self->sensor_id, sensor_id, OC_SENSOR_ID_LEN) != 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "goodix5125: the sensor identity changed");
          return FALSE;
        }
      if (!oc_session_rebase (self->session, image_base, error))
        return FALSE;
      return goodix5125_algo_save_state (self, error);
    }

  if (self->loaded)
    {
      if (memcmp (self->loaded_sensor_id, sensor_id, OC_SENSOR_ID_LEN) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "goodix5125: %s belongs to another sensor; move it "
                       "away to calibrate this one", self->state_path);
          return FALSE;
        }
      if (!oc_session_rebase (self->loaded, image_base, error))
        return FALSE;
      session = g_steal_pointer (&self->loaded);
      fp_dbg ("openchicago state restored and rebased onto the live ImageBase");
    }
  else
    {
      g_autoptr(GError) local = NULL;

      if (calidata_path && g_file_test (calidata_path, G_FILE_TEST_EXISTS))
        {
          session = oc_session_new_from_calidata (calidata_path, sensor_id,
                                                  image_base, &local);
          if (session)
            fp_info ("imported the calibration from %s", calidata_path);
          else
            fp_warn ("ignoring the calibration %s: %s", calidata_path,
                     local->message);
        }
      if (!session)
        session = oc_session_new (image_base, error);
      if (!session)
        return FALSE;
    }

  g_clear_pointer (&self->session, oc_session_free);
  self->session = g_steal_pointer (&session);
  memcpy (self->sensor_id, sensor_id, OC_SENSOR_ID_LEN);
  self->enrolling = FALSE;
  return goodix5125_algo_save_state (self, error);
}

void
goodix5125_algo_deactivate (Goodix5125Algo *self)
{
  g_clear_pointer (&self->session, oc_session_free);
  g_clear_pointer (&self->loaded, oc_session_free);
  self->enrolling = FALSE;
}

/* ---- enrolment ---------------------------------------------------------- */

gboolean
goodix5125_algo_enroll_begin (Goodix5125Algo  *self,
                              OcEnrollProtocol protocol,
                              guint            n_stages,
                              GError         **error)
{
  g_return_val_if_fail (self->session != NULL, FALSE);
  if (!oc_enroll_begin (self->session, protocol, n_stages, error))
    return FALSE;
  self->enrolling = TRUE;
  return TRUE;
}

static GError *
enroll_retry (const OcEnrollResult *r)
{
  switch (r->reject)
    {
    case OC_REJECT_TIP:
      /* The protocol asks for another area in the direction order
       * 4, 1, 3, 2; the frame stays in the template but is not counted. */
      switch (r->tip_direction)
        {
        case 1:
          return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                           "This area is enrolled already; "
                                           "place the finger a bit lower");

        case 2:
          return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                           "This area is enrolled already; "
                                           "place the finger a bit higher");

        case 3:
          return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                           "This area is enrolled already; "
                                           "move the finger to the right");

        default:
          return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                           "This area is enrolled already; "
                                           "move the finger to the left");
        }

    case OC_REJECT_TOO_SHORT:
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                       "The finger covers too little of the "
                                       "sensor; press it flat");

    case OC_REJECT_NONE:
      return NULL;

    case OC_REJECT_LOW_QUALITY:
    case OC_REJECT_BAD_INPUT:
    case OC_REJECT_POOR_CAPTURE:
    case OC_REJECT_CLASS:
    case OC_REJECT_NO_FEATURES:
    case OC_REJECT_MERGE_FAILURE:
    case OC_REJECT_TEMPLATE_FULL:
    default:
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                       "Poor fingerprint image (%s); try "
                                       "again", oc_reject_to_string (r->reject));
    }
}

gboolean
goodix5125_algo_enroll_add (Goodix5125Algo       *self,
                            const guint16         frame[OC_FRAME_PIXELS],
                            const guint16        *extra,
                            Goodix5125EnrollStep *step,
                            GError              **error)
{
  gboolean ok;

  g_return_val_if_fail (self->session != NULL && self->enrolling, FALSE);
  memset (step, 0, sizeof (*step));
  if (extra)
    ok = oc_enroll_add_pair (self->session, frame, extra, &step->result, error);
  else
    ok = oc_enroll_add (self->session, frame, &step->result, error);
  if (!ok)
    return FALSE;

  step->stage = step->result.stage;
  step->complete = step->result.complete;
  if (!step->result.accepted && !step->complete)
    step->retry = enroll_retry (&step->result);
  fp_dbg ("enroll frame: %s q=%u c=%u ov=%u pov=%u stage %u/%u samples %u%s",
          step->result.accepted ? "accepted" : oc_reject_to_string (step->result.reject),
          step->result.quality, step->result.coverage, step->result.overlay,
          step->result.preoverlay, step->result.stage, step->result.n_stages,
          step->result.samples, step->complete ? " complete" : "");
  return TRUE;
}

GVariant *
goodix5125_algo_enroll_finish (Goodix5125Algo *self,
                               GError        **error)
{
  g_autoptr(GBytes) blob = NULL;

  g_return_val_if_fail (self->session != NULL && self->enrolling, NULL);
  self->enrolling = FALSE;
  blob = oc_enroll_finish (self->session, error);
  if (!blob)
    return NULL;
  return oc_print_data_new (self->session, self->sensor_id, blob);
}

void
goodix5125_algo_enroll_cancel (Goodix5125Algo *self)
{
  if (self->session)
    oc_enroll_cancel (self->session);
  self->enrolling = FALSE;
}

/* ---- learned-template store ---------------------------------------------- */

void
goodix5125_algo_set_learned_store (Goodix5125Algo *self,
                                   const gchar    *directory,
                                   guint           max_entries,
                                   guint           max_age_days)
{
  g_free (self->learned_dir);
  self->learned_dir = g_strdup (directory);
  self->learned_max_entries = max_entries ? max_entries :
                              GOODIX5125_LEARNED_MAX_ENTRIES;
  self->learned_max_age_days = max_age_days ? max_age_days :
                               GOODIX5125_LEARNED_MAX_AGE_DAYS;
}

static GBytes *
variant_bytes (GVariant *value)
{
  g_autoptr(GVariant) normal = g_variant_get_normal_form (value);

  return g_variant_get_data_as_bytes (normal);
}

static void
sha256 (const guint8 *data,
        gsize         length,
        guint8        digest[DIGEST_LEN])
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize n = DIGEST_LEN;

  g_checksum_update (checksum, data, length);
  g_checksum_get_digest (checksum, digest, &n);
}

static void
print_digest (GVariant *print,
              guint8    digest[DIGEST_LEN])
{
  g_autoptr(GBytes) bytes = variant_bytes (print);
  gsize length;
  const guint8 *data = g_bytes_get_data (bytes, &length);

  sha256 (data, length, digest);
}

static gchar *
digest_hex (const guint8 digest[DIGEST_LEN])
{
  gchar *hex = g_malloc (KEY_HEX_LEN + 1);

  for (guint i = 0; i < DIGEST_LEN; i++)
    g_snprintf (hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

gchar *
goodix5125_algo_print_key (GVariant *print)
{
  guint8 digest[DIGEST_LEN];

  g_return_val_if_fail (print != NULL, NULL);
  print_digest (print, digest);
  return digest_hex (digest);
}

static gchar *
learned_entry_path (Goodix5125Algo *self,
                    const guint8    key[DIGEST_LEN])
{
  g_autofree gchar *hex = digest_hex (key);
  g_autofree gchar *name = g_strconcat (hex, GOODIX5125_LEARNED_SUFFIX, NULL);

  return g_build_filename (self->learned_dir, name, NULL);
}

gchar *
goodix5125_algo_learned_path (Goodix5125Algo *self,
                              GVariant       *print)
{
  guint8 key[DIGEST_LEN];

  if (!self->learned_dir || !print)
    return NULL;
  print_digest (print, key);
  return learned_entry_path (self, key);
}

/* The learned template of the print with @key, or NULL (no entry, or an
 * unusable one: warning, the caller keeps the enrolled template). */
static GBytes *
learned_lookup (Goodix5125Algo *self,
                const guint8    key[DIGEST_LEN])
{
  g_autofree gchar *path = learned_entry_path (self, key);
  g_autofree gchar *contents = NULL;
  g_autofree gchar *reason = NULL;

  g_autoptr(GError) local = NULL;
  g_autoptr(GBytes) payload = NULL;
  g_autoptr(GVariant) print = NULL;
  guint8 sum[DIGEST_LEN];
  guint32 version;
  gsize length = 0;
  GBytes *blob;

  if (!g_file_get_contents (path, &contents, &length, &local))
    {
      if (!g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        fp_warn ("cannot read the learned template %s (%s); using the "
                 "enrolled template", path, local->message);
      return NULL;
    }

  if (length <= LEARNED_HEADER_SIZE ||
      memcmp (contents, GOODIX5125_LEARNED_MAGIC, 4) != 0)
    {
      reason = g_strdup ("not a learned-template file");
      goto unusable;
    }
  memcpy (&version, contents + 4, sizeof (version));
  if (GUINT32_FROM_LE (version) != GOODIX5125_LEARNED_VERSION)
    {
      reason = g_strdup_printf ("unsupported format version %u",
                                GUINT32_FROM_LE (version));
      goto unusable;
    }
  if (memcmp (contents + LEARNED_OFF_KEY, key, DIGEST_LEN) != 0)
    {
      reason = g_strdup ("entry of another print");
      goto unusable;
    }
  sha256 ((const guint8 *) contents + LEARNED_HEADER_SIZE,
          length - LEARNED_HEADER_SIZE, sum);
  if (memcmp (contents + LEARNED_OFF_SUM, sum, DIGEST_LEN) != 0)
    {
      reason = g_strdup ("checksum mismatch, the file is corrupt");
      goto unusable;
    }
  if (memcmp (contents + LEARNED_OFF_SENSOR, self->sensor_id,
              OC_SENSOR_ID_LEN) != 0)
    {
      reason = g_strdup ("learned on another sensor");
      goto unusable;
    }

  payload = g_bytes_new (contents + LEARNED_HEADER_SIZE,
                         length - LEARNED_HEADER_SIZE);
  print = g_variant_ref_sink (g_variant_new_from_bytes (
                                G_VARIANT_TYPE (LEARNED_PRINT_TYPE), payload,
                                FALSE));
  blob = oc_print_data_get_template (print, self->sensor_id, &local);
  if (!blob)
    {
      reason = g_strdup (local->message);
      goto unusable;
    }

  /* last use = mtime, for the expiry in learned_collect() */
  if (g_utime (path, NULL) != 0)
    fp_dbg ("cannot touch %s: %s", path, g_strerror (errno));
  return blob;

unusable:
  fp_warn ("ignoring the learned template %s: %s; using the enrolled template",
           path, reason);
  return NULL;
}

static gboolean
is_entry_name (const gchar *name)
{
  for (guint i = 0; i < KEY_HEX_LEN; i++)
    if (!g_ascii_isxdigit (name[i]) || g_ascii_isupper (name[i]))
      return FALSE;
  return g_str_equal (name + KEY_HEX_LEN, GOODIX5125_LEARNED_SUFFIX);
}

/* g_file_set_contents_full() writes <entry>.XXXXXX and renames it */
static gboolean
is_temp_name (const gchar *name)
{
  gsize entry_len = KEY_HEX_LEN + strlen (GOODIX5125_LEARNED_SUFFIX);
  g_autofree gchar *entry = NULL;

  if (strlen (name) != entry_len + 7 || name[entry_len] != '.')
    return FALSE;
  entry = g_strndup (name, entry_len);
  return is_entry_name (entry);
}

typedef struct
{
  gchar *path;
  gint64 mtime;
} LearnedEntry;

static void
learned_entry_clear (gpointer data)
{
  g_free (((LearnedEntry *) data)->path);
}

static gint
learned_entry_older (gconstpointer a,
                     gconstpointer b)
{
  const LearnedEntry *x = a, *y = b;

  return (x->mtime > y->mtime) - (x->mtime < y->mtime);
}

static void
learned_remove (const gchar *path,
                const gchar *why)
{
  if (g_unlink (path) == 0)
    fp_dbg ("removed the learned template %s (%s)", path, why);
  else
    fp_warn ("cannot remove %s: %s", path, g_strerror (errno));
}

/* Expiry: entries unused for max_age_days, then the least recently used ones
 * beyond max_entries; @keep (just written) is never removed. */
static void
learned_collect (Goodix5125Algo *self,
                 const gchar    *keep)
{
  g_autoptr(GArray) entries = g_array_new (FALSE, FALSE, sizeof (LearnedEntry));
  g_autoptr(GDir) dir = g_dir_open (self->learned_dir, 0, NULL);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 max_age = (gint64) self->learned_max_age_days * 24 * 3600;
  const gchar *name;

  if (!dir)
    return;
  g_array_set_clear_func (entries, learned_entry_clear);
  while ((name = g_dir_read_name (dir)))
    {
      g_autofree gchar *path = g_build_filename (self->learned_dir, name, NULL);
      GStatBuf st;

      if (g_lstat (path, &st) != 0 || !S_ISREG (st.st_mode))
        continue;
      if (is_temp_name (name))
        {
          if (now - (gint64) st.st_mtime > LEARNED_TMP_MAX_AGE_S)
            learned_remove (path, "interrupted write");
          continue;
        }
      if (!is_entry_name (name))
        continue;
      if (g_strcmp0 (path, keep) != 0 && now - (gint64) st.st_mtime > max_age)
        {
          learned_remove (path, "unused for too long");
          continue;
        }
      if (g_strcmp0 (path, keep) != 0)
        {
          LearnedEntry entry = { g_steal_pointer (&path), st.st_mtime };

          g_array_append_val (entries, entry);
        }
    }

  /* @keep counts towards the limit */
  if (entries->len + 1 > self->learned_max_entries)
    {
      guint excess = entries->len + 1 - self->learned_max_entries;

      g_array_sort (entries, learned_entry_older);
      for (guint i = 0; i < excess && i < entries->len; i++)
        learned_remove (g_array_index (entries, LearnedEntry, i).path,
                        "store limit reached");
    }
}

static gboolean
learned_store (Goodix5125Algo *self,
               const guint8    key[DIGEST_LEN],
               GVariant       *learned,
               GError        **error)
{
  g_autofree gchar *path = learned_entry_path (self, key);

  g_autoptr(GBytes) payload = variant_bytes (learned);
  g_autoptr(GByteArray) out = NULL;
  guint32 version = GUINT32_TO_LE (GOODIX5125_LEARNED_VERSION);
  guint8 sum[DIGEST_LEN];
  const guint8 *data;
  gsize length;

  data = g_bytes_get_data (payload, &length);
  sha256 (data, length, sum);
  out = g_byte_array_sized_new (LEARNED_HEADER_SIZE + length);
  g_byte_array_append (out, (const guint8 *) GOODIX5125_LEARNED_MAGIC, 4);
  g_byte_array_append (out, (const guint8 *) &version, 4);
  g_byte_array_append (out, self->sensor_id, OC_SENSOR_ID_LEN);
  g_byte_array_append (out, key, DIGEST_LEN);
  g_byte_array_append (out, sum, DIGEST_LEN);
  g_byte_array_append (out, data, length);

  if (g_mkdir_with_parents (self->learned_dir, 0700) != 0)
    {
      int saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "cannot create %s: %s", self->learned_dir,
                   g_strerror (saved_errno));
      return FALSE;
    }
  if (!g_file_set_contents_full (path, (const gchar *) out->data, out->len,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
    return FALSE;
  fp_dbg ("learned template saved to %s", path);
  learned_collect (self, path);
  return TRUE;
}

/* ---- verify / identify -------------------------------------------------- */

gboolean
goodix5125_algo_match (Goodix5125Algo      *self,
                       const guint16        frame[OC_FRAME_PIXELS],
                       GVariant *const     *prints,
                       guint                n_prints,
                       Goodix5125MatchStep *step,
                       GError             **error)
{
  g_autoptr(GPtrArray) templates = g_ptr_array_new_with_free_func (
    (GDestroyNotify) g_bytes_unref);
  g_autofree guint *owner = g_new0 (guint, MAX (n_prints, 1));
  g_autofree guint8 *keys = g_new0 (guint8, (gsize) DIGEST_LEN * MAX (n_prints, 1));
  g_autoptr(GBytes) updated = NULL;
  gint index = -1;

  g_return_val_if_fail (self->session != NULL, FALSE);
  memset (step, 0, sizeof (*step));
  step->matched = -1;

  for (guint i = 0; i < n_prints; i++)
    {
      g_autoptr(GError) local = NULL;
      GBytes *blob = prints[i] ?
                     oc_print_data_get_template (prints[i], self->sensor_id, &local) :
                     NULL;

      if (!blob)
        {
          fp_warn ("skipping print %u: %s", i,
                   local ? local->message : "no data");
          continue;
        }
      if (self->learned_dir)
        {
          guint8 *key = keys + (gsize) DIGEST_LEN * templates->len;
          GBytes *learned;

          /* the key is the print as fprintd stores it, not the learned data */
          print_digest (prints[i], key);
          learned = learned_lookup (self, key);
          if (learned)
            {
              g_bytes_unref (blob);
              blob = learned;
              step->n_learned++;
            }
        }
      owner[templates->len] = i;
      g_ptr_array_add (templates, blob);
    }
  /* Nothing to compare against (e.g. fprintd's duplicate check before the
   * first enrolment, or only prints of another sensor): no match */
  if (templates->len == 0)
    return TRUE;

  if (!oc_identify (self->session, (GBytes *const *) templates->pdata,
                    templates->len, frame, &step->result, &index, &updated,
                    error))
    return FALSE;

  if (step->result.reject != OC_REJECT_NONE)
    {
      step->retry = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                              "Poor fingerprint image (%s); "
                                              "try again",
                                              oc_reject_to_string (step->result.reject));
      fp_dbg ("match frame rejected: %s q=%u c=%u",
              oc_reject_to_string (step->result.reject),
              step->result.quality, step->result.coverage);
      return TRUE;
    }
  if (index >= 0)
    {
      step->matched = (gint) owner[index];
      if (updated)
        step->updated = oc_print_data_new (self->session, self->sensor_id, updated);
      if (step->updated && self->learned_dir)
        {
          g_autoptr(GError) local = NULL;

          step->stored = learned_store (self, keys + (gsize) DIGEST_LEN * index,
                                        step->updated, &local);
          if (!step->stored)
            fp_warn ("cannot save the learned template: %s", local->message);
        }
    }
  fp_dbg ("match: score %d subtemplate %d q=%u c=%u -> %s%s (%u of %u prints "
          "compared through learned templates)",
          step->result.score, step->result.subtemplate, step->result.quality,
          step->result.coverage, index >= 0 ? "match" : "no match",
          step->updated ? (step->stored ? " (template learned, stored)" :
                           " (template learned)") : "",
          step->n_learned, templates->len);
  return TRUE;
}
