// SPDX-License-Identifier: LGPL-2.1-or-later
/* GF5298 bounded host-side profile contracts, developed for this driver. */
/* Qualified fresh GF5298 host profile. No USB, TLS handshake or key I/O. */
#pragma once
#include <glib.h>

#define GX_PIXELS 5120
#define GX_BLOB_SIZE 7684
#define GX_PINNED_TEMPLATE_SHA "101c07451512f8ea7220b8988a04f8d044d2b91918d082a94fba68bdcc9bdd72"

typedef enum { GX_OK, GX_LENGTH, GX_CRC, GX_PROFILE, GX_CONFIG, GX_OWNER,
               GX_HANDSHAKE, GX_TRUSTED_READ } GxResult;
typedef enum { GX_MISSING, GX_FALLBACK, GX_PRIMARY, GX_SECONDARY } GxSource;
typedef struct {
  guint16 image[4], fdt[4];
  guint32 sources[4];
  guint16 integration, timing, fdt_tcode, capture_mode, initial_image_dac;
  guint16 delta_down, delta_up_windows, delta_image, delta_navigation, fdt_offset;
  gint32 delta_up_signed;
} GxFields;
typedef struct {
  guint64 generation;
  guint8 prefix[5], marker, valid;
  guint8 pixels[GX_PIXELS * 2]; /* Explicit little-endian uint16 raster, 80x64. */
} GxImage;
typedef struct {
  guint8 opcode, payload[27], frame[64];
  gsize payload_len, logical_len;
} GxSubmission;

GxResult gx_fields (const guint8 *otp, gsize len, GxFields *out);
guint gx_conservative_policy (const GxFields *fields);
GxResult gx_config (const guint8 *base, gsize len, const char *qualified_sha,
                    const GxFields *fields, guint8 out[256]);
GxResult gx_submission (guint index, const guint16 words[4],
                        const guint8 *seed, gsize seed_len, GxSubmission *out);
GxResult gx_image_blob (const guint8 *blob, gsize len, const guint8 prefix[5],
                        guint64 owner, guint64 current, GxImage *out);
GxResult gx_image_a0 (const guint8 *frame, gsize len, guint64 owner,
                      guint64 current, GxImage *out);
GxResult gx_b2_layout (const guint8 *frame, gsize len, gsize *cipher_offset,
                       gsize *cipher_len, gsize *capacity);
/* Trusted read must come from a separately verified authenticated provider.
 * This boundary checks lengths/ownership; it performs no authentication. */
GxResult gx_image_b2_trusted (const guint8 *frame, gsize len, guint state,
                              const guint8 *plaintext, gsize available,
                              gssize returned, guint64 owner, guint64 current,
                              GxImage *out);
GxResult gx_background_gates (const guint8 a[16], const guint8 b[16],
                              const guint8 c[16], const GxImage *p,
                              const GxImage *q, guint16 threshold,
                              guint64 current, guint64 diagnostics[5]);
/* Translate the current checksummed DOWN event into a two-phase UP request. */
GxResult gx_up_request (const guint8 factory[64], const guint8 *down,
                        gsize length, guint phase, guint8 request[40]);
