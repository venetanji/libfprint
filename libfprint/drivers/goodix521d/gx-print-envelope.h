/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define GX_PRINT_ENVELOPE_TYPE "(usuuuayayay)"
#define GX_PRINT_ENVELOPE_VERSION 1u
#define GX_PRINT_MAX_PAYLOAD (16u * 1024u * 1024u)
typedef struct {
 const gchar *backend;
 guint32 backend_version, profile, preprocess_version;
 guint8 sensor_id[16], config_digest[32];
} GxPrintIdentity;
/* Returns a full (non-floating) reference. Caller must obtain completed from
 * the engine's natural result and call oc_enroll_finish_complete first.
 * Config digest binds stable sensor/configuration compatibility, NOT adaptive
 * calibration bytes, which change legitimately across rebases. */
GVariant *gx_print_envelope_export(const GxPrintIdentity *identity,
 gboolean completed,GBytes *native_payload,GError **error);
/* Returns new GBytes or an error. Caller supplies the expected identity from
 * the current driver, never copied from the untrusted envelope. Validates
 * exact native Chicago payload layout/CRC. No enrollment-template learning. */
GBytes *gx_print_envelope_import(GVariant *data,const GxPrintIdentity *expected,
 GError **error);
G_END_DECLS
