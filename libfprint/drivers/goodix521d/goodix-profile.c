// SPDX-License-Identifier: LGPL-2.1-or-later
/* GF5298 bounded host-side profile contracts, developed for this driver. */
/* GF5298 fresh transform-disabled host contracts, qualified by the v5 handoff.
 * Pure bounded operations: no transport, persistent calibration or global image. */
#include "goodix-profile.h"
#include <string.h>

static guint16
le16 (const guint8 *p)
{
  return p[0] | ((guint16) p[1] << 8);
}

static void
put16 (guint8 *p, guint16 v)
{
  p[0] = v & 255;
  p[1] = v >> 8;
}

GxResult
gx_fields (const guint8 *otp, gsize len, GxFields *out)
{
  if (!out)
    return GX_PROFILE;
  memset (out, 0, sizeof *out);
  if (!otp || len != 64)
    return GX_LENGTH;
  if (otp[17] && otp[22] && otp[31])
    {
      out->image[0] = otp[22] | ((otp[17] & 1) << 8);
      out->fdt[0] = otp[31] | ((otp[17] & 64) << 2);
      out->sources[0] = GX_FALLBACK;
    }
  if (otp[23] && otp[26] && otp[33])
    {
      out->image[0] = otp[26] | ((otp[23] & 1) << 8);
      out->fdt[0] = otp[33] | ((otp[23] & 64) << 2);
      out->sources[0] = GX_PRIMARY;
    }
  for (guint i = 1; i < 4; i++)
    if (otp[40] && otp[40 + i] && otp[43 + i])
      {
        out->fdt[i] = otp[40 + i] | (((otp[40] >> (i - 1)) & 1) << 8);
        out->image[i] = otp[43 + i] | (((otp[40] >> (i + 2)) & 1) << 8);
        out->sources[i] = GX_SECONDARY;
      }
  guint mask = otp[23] & 62;
  guint n = (mask >> 1) + 1;
  out->integration = 2u * otp[27];
  out->timing = mask ? n / 3 : 0;
  out->fdt_tcode = otp[32];
  out->capture_mode = otp[23] >> 7;
  out->initial_image_dac = out->image[0];
  out->delta_down = mask ? n / 3 : 13;
  out->delta_up_signed = (gint32) out->delta_down - 2;
  out->delta_up_windows = (guint16) out->delta_up_signed;
  out->delta_image = 200;
  out->delta_navigation = mask ? (128u * n) / 100 : 40;
  out->fdt_offset = otp[40] >> 6;
  return GX_OK;
}

guint
gx_conservative_policy (const GxFields *f)
{
  if (!f)
    return 31;
  guint failures = 0;
  if (!f->integration || f->integration > 510 || f->integration % 2)
    failures |= 1;
  if (!f->timing || f->timing > 10)
    failures |= 2;
  if (f->delta_up_signed < 0)
    failures |= 4;
  for (guint i = 0; i < 4; i++)
    {
      if (!f->fdt[i] || f->fdt[i] > 511)
        failures |= 8;
      if (!f->image[i] || f->image[i] > 511)
        failures |= 16;
    }
  return failures;
}

static void
config_checksum (guint8 *out)
{
  guint32 sum = 0xa5a5;
  for (guint i = 0; i < 254; i += 2)
    sum += le16 (out + i);
  put16 (out + 254, (guint16) (0u - sum));
}

static gboolean
replace_record (guint8 *out, guint begin, guint end, guint16 reg, guint16 value)
{
  for (guint pos = begin; pos + 4 <= end; pos += 4)
    if (le16 (out + pos) == reg)
      {
        put16 (out + pos + 2, value);
        return TRUE;
      }
  return FALSE;
}

GxResult
gx_config (const guint8 *base, gsize len, const char *qualified_sha,
           const GxFields *f, guint8 out[256])
{
  if (!out)
    return GX_PROFILE;
  /* Stage edits so a later missing record never exposes a partial config. */
  guint8 tmp[256];
  if (!base || len != 256 || !f || !qualified_sha)
    {
      memset (out, 0, 256);
      return GX_LENGTH;
    }
  memcpy (tmp, base, 256);
  memset (out, 0, 256);
  g_autofree char *actual = g_compute_checksum_for_data (G_CHECKSUM_SHA256, tmp, 256);
  if (strcmp (actual, qualified_sha))
    return GX_CONFIG;
  guint fb = tmp[5], fe = fb + tmp[6], ib = tmp[9], ie = ib + tmp[10];
  if (fb < 11 || ib < 11 || fe > 254 || ie > 254 ||
      (fe - fb) % 4 || (ie - ib) % 4 || (fb < ie && ib < fe))
    return GX_CONFIG;
  if (f->integration > 510 || f->integration % 2 || f->fdt_tcode > 255 ||
      f->delta_down > 13 || f->fdt_offset > 3)
    return GX_PROFILE;
  if ((f->integration && !replace_record (tmp, ib, ie, 0x005c, f->integration)) ||
      (f->fdt_tcode && !replace_record (tmp, fb, fe, 0x005c, f->fdt_tcode)) ||
      (f->delta_down && !replace_record (tmp, fb, fe, 0x0082, (f->delta_down << 8) | 0x80)) ||
      (f->fdt_offset && !replace_record (tmp, fb, fe, 0x0056, 0x1406 + f->fdt_offset)))
    return GX_CONFIG;
  config_checksum (tmp);
  memcpy (out, tmp, 256);
  return GX_OK;
}

GxResult
gx_submission (guint index, const guint16 words[4], const guint8 *seed,
               gsize seed_len, GxSubmission *out)
{
  if (!out)
    return GX_PROFILE;
  memset (out, 0, sizeof *out);
  if (index >= 12 || !words || !seed || seed_len != 16)
    return GX_PROFILE;
  for (guint i = 0; i < 4; i++)
    if (words[i] > 511)
      return GX_PROFILE;
  const guint8 opcodes[12] = {0x36, 0x36, 0x80, 0x20, 0x80, 0x36,
                             0x36, 0x80, 0x20, 0x80, 0x36, 0x36};
  out->opcode = opcodes[index];
  if (out->opcode == 0x80)
    {
      const guint8 write[5] = {0, 0x2c, 2, 0x0a, 3};
      memcpy (out->payload, write, 5);
      out->payload[4] = (index == 4 || index == 9) ? 2 : 3;
      out->payload_len = 5;
    }
  else
    {
      out->payload[0] = out->opcode == 0x20 ? (index == 3 ? 1 : 0x81) :
                        ((index == 5 || index == 6) ? 0x8d : 0x0d);
      out->payload[1] = out->opcode == 0x20 ? 3 : 1;
      for (guint i = 0; i < 4; i++)
        put16 (out->payload + 2 + 2 * i, words[i]);
      out->payload_len = out->opcode == 0x20 ? 10 : 27;
      if (out->opcode == 0x36)
        {
          memcpy (out->payload + 10, seed, 16);
          out->payload[26] = index == 1 || index == 6 || index == 11;
        }
    }
  out->frame[0] = 0xa0;
  put16 (out->frame + 1, out->payload_len + 4);
  out->frame[3] = (out->frame[0] + out->frame[1] + out->frame[2]) & 255;
  out->frame[4] = out->opcode;
  put16 (out->frame + 5, out->payload_len + 1);
  memcpy (out->frame + 7, out->payload, out->payload_len);
  guint sum = 0;
  for (guint i = 4; i < out->payload_len + 7; i++)
    sum += out->frame[i];
  out->frame[out->payload_len + 7] = (0xaa - sum) & 255;
  out->logical_len = out->payload_len + 8;
  return GX_OK;
}

static GxResult
outer (const guint8 *frame, gsize len, guint8 tag)
{
  if (!frame || len < 4 || len > G_MAXUINT16 + 4u)
    return GX_LENGTH;
  if (frame[0] != tag || frame[3] != ((frame[0] + frame[1] + frame[2]) & 255))
    return GX_PROFILE;
  if (le16 (frame + 1) + 4u != len)
    return GX_LENGTH;
  return GX_OK;
}

GxResult
gx_image_blob (const guint8 *blob, gsize len, const guint8 prefix[5],
               guint64 owner, guint64 current, GxImage *out)
{
  if (!out)
    return GX_PROFILE;
  memset (out, 0, sizeof *out);
  if (!owner || owner != current)
    return GX_OWNER;
  if (!blob || len != GX_BLOB_SIZE || !prefix)
    return GX_LENGTH;
  guint32 crc = 0xffffffff;
  for (guint i = 0; i < GX_BLOB_SIZE - 4; i++)
    {
      crc ^= (guint32) blob[i] << 24;
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
  const guint8 *t = blob + GX_BLOB_SIZE - 4;
  guint32 stored = ((guint32) t[2] << 24) | ((guint32) t[3] << 16) |
                   ((guint32) t[0] << 8) | t[1];
  if (crc != stored)
    return GX_CRC;
  for (guint j = 0; j < 1280; j++)
    {
      const guint8 *b = blob + j * 6;
      guint16 samples[4] = {((b[0] & 15) << 8) | b[1], (b[3] << 4) | (b[0] >> 4),
                           ((b[5] & 15) << 8) | b[2], (b[4] << 4) | (b[5] >> 4)};
      for (guint k = 0; k < 4; k++)
        {
          guint i = j * 4 + k;
          put16 (out->pixels + 2 * ((i & 63) * 80 + (i >> 6)), samples[k]);
        }
    }
  memcpy (out->prefix, prefix, 5);
  out->marker = prefix[0] == 0xaa;
  out->generation = current;
  out->valid = TRUE;
  return GX_OK;
}

GxResult
gx_image_a0 (const guint8 *frame, gsize len, guint64 owner, guint64 current,
             GxImage *out)
{
  if (!out)
    return GX_PROFILE;
  memset (out, 0, sizeof *out);
  GxResult result = outer (frame, len, 0xa0);
  if (result != GX_OK)
    return result;
  if (len != 7696 || frame[4] != 0x20 || le16 (frame + 5) != 7689)
    return GX_LENGTH;
  return gx_image_blob (frame + 12, GX_BLOB_SIZE, frame + 7, owner, current, out);
}

GxResult
gx_b2_layout (const guint8 *frame, gsize len, gsize *cipher_offset,
              gsize *cipher_len, gsize *capacity)
{
  if (!cipher_offset || !cipher_len || !capacity)
    return GX_PROFILE;
  *cipher_offset = *cipher_len = *capacity = 0;
  GxResult result = outer (frame, len, 0xb2);
  if (result != GX_OK)
    return result;
  guint length = le16 (frame + 1);
  if (length <= 9 || length > G_MAXUINT16 / 2 || frame[5] != 0x20)
    return GX_LENGTH;
  *cipher_offset = 13;
  *cipher_len = length - 9;
  *capacity = 2u * length;
  return GX_OK;
}

GxResult
gx_image_b2_trusted (const guint8 *frame, gsize len, guint state,
                     const guint8 *plaintext, gsize available, gssize returned,
                     guint64 owner, guint64 current, GxImage *out)
{
  if (!out)
    return GX_PROFILE;
  memset (out, 0, sizeof *out);
  gsize offset, cipher_len, capacity;
  GxResult result = gx_b2_layout (frame, len, &offset, &cipher_len, &capacity);
  if (result != GX_OK)
    return result;
  if (state < 16)
    return GX_HANDSHAKE;
  if (!plaintext || returned <= 0 || (gsize) returned > available ||
      (gsize) returned > capacity || returned != GX_BLOB_SIZE ||
      (gsize) returned > G_MAXUINT16 - 5u)
    return GX_TRUSTED_READ;
  return gx_image_blob (plaintext, returned, frame + 8, owner, current, out);
}

GxResult
gx_background_gates (const guint8 a[16], const guint8 b[16], const guint8 c[16],
                     const GxImage *p, const GxImage *q, guint16 threshold,
                     guint64 current, guint64 diagnostics[5])
{
  if (!diagnostics)
    return GX_PROFILE;
  memset (diagnostics, 0, 5 * sizeof *diagnostics);
  if (!a || !b || !c || !p || !q || threshold > 10)
    return GX_PROFILE;
  if (!current || !p->valid || !q->valid ||
      p->generation != current || q->generation != current)
    return GX_OWNER;
  guint maximum_ab = 0, maximum_bc = 0, usable = 0;
  guint64 sum = 0;
  for (guint i = 0; i < 8; i++)
    {
      gint av = le16 (a + 2 * i) >> 1, bv = le16 (b + 2 * i) >> 1;
      gint cv = le16 (c + 2 * i) >> 1;
      maximum_ab = MAX (maximum_ab, (guint) ABS (av - bv));
      maximum_bc = MAX (maximum_bc, (guint) ABS (bv - cv));
    }
  for (guint y = 2; y < 62; y++)
    for (guint x = 2; x < 78; x++)
      {
        guint pos = 2 * (y * 80 + x);
        gint pv = le16 (p->pixels + pos), qv = le16 (q->pixels + pos);
        if (pv > 4095 || qv > 4095)
          return GX_PROFILE;
        sum += ABS (pv - qv);
        usable += pv > 100 && pv < 3800;
      }
  diagnostics[0] = maximum_ab;
  diagnostics[1] = sum;
  diagnostics[2] = maximum_bc;
  diagnostics[3] = usable;
  diagnostics[4] = (maximum_ab <= threshold ? 1 : 0) |
                   (sum < 912000 ? 2 : 0) |
                   (maximum_bc <= threshold ? 4 : 0) |
                   (usable >= 3877 ? 8 : 0);
  return GX_OK;
}

GxResult
gx_up_request (const guint8 factory[64], const guint8 *down,
               gsize length, guint phase, guint8 request[40])
{
  GxFields fields;
  if (!request) return GX_PROFILE;
  memset(request,0,40);
  if (!down || length!=20) return GX_LENGTH;
  if (!factory || phase>1 || le16(down)!=2 || gx_fields(factory,64,&fields)!=GX_OK)
    return GX_PROFILE;
  request[0]=0x8e; request[1]=1; request[26]=phase;
  guint16 touch=le16(down+2);
  for (guint i=0;i<4;i++) put16(request+2+i*2,fields.fdt[i]);
  for (guint i=0;i<8;i++) {
    guint16 sample=le16(down+4+i*2);
    guint16 value=(touch&(1u<<i)) ? (guint16)((sample>>1)+fields.timing) :
                                                 (guint16)(fields.timing-2);
    put16(request+10+i*2,(guint16)((value<<8)|value));
  }
  return GX_OK;
}

GxResult
gx_fdt_seed (const guint8 *raw, gsize length, guint8 seed[16])
{
  if (!seed) return GX_PROFILE;
  memset(seed,0,16);
  if (!raw || length!=16) return GX_LENGTH;
  for (guint i=0;i<8;i++) put16(seed+2*i,(guint16)((le16(raw+2*i)>>1)*257u));
  return GX_OK;
}

GxResult
gx_down_request (const guint8 factory[64], const guint8 *seed,
                 gsize length, guint phase, guint8 request[40])
{
  GxFields fields;
  if (!request) return GX_PROFILE;
  memset(request,0,40);
  if (!factory || gx_fields(factory,64,&fields)!=GX_OK) return GX_PROFILE;
  return gx_down_request_with_dacs(factory,seed,length,phase,fields.image,request);
}

GxResult
gx_contact_image_request (const guint16 image_dacs[4], guint8 request[10])
{
  if (!request) return GX_PROFILE;
  memset(request,0,10);
  if (!image_dacs) return GX_PROFILE;
  for (guint i=0;i<4;i++) if (image_dacs[i]>511) return GX_PROFILE;
  request[0]=0x45;request[1]=3;
  for (guint i=0;i<4;i++) put16(request+2+2*i,image_dacs[i]);
  return GX_OK;
}

GxResult
gx_down_request_with_dacs (const guint8 factory[64], const guint8 *seed,
                           gsize length, guint phase,
                           const guint16 image_dacs[4], guint8 request[40])
{
  GxFields fields;
  if (!request) return GX_PROFILE;
  memset(request,0,40);
  if (!seed || length!=16) return GX_LENGTH;
  if (!factory || phase>1 || gx_fields(factory,64,&fields)!=GX_OK) return GX_PROFILE;
  if (!image_dacs) return GX_PROFILE;
  for (guint i=0;i<4;i++) if (image_dacs[i]>511) return GX_PROFILE;
  request[0]=0x9c;request[1]=1;request[26]=phase;
  memcpy(request+10,seed,16);
  request[28]=fields.capture_mode?5:10;request[29]=3;request[38]=3;
  for(guint i=0;i<4;i++) {
    put16(request+2+2*i,fields.fdt[i]);put16(request+30+2*i,image_dacs[i]);
  }
  return GX_OK;
}

GxResult
gx_contact_manual_request (const guint8 factory[64], const guint8 *seed,
                           gsize length, guint phase, guint8 request[27])
{
  GxFields fields;
  if (!request) return GX_PROFILE;
  memset(request,0,27);
  if (!seed || length!=16) return GX_LENGTH;
  if (!factory || phase>1 || gx_fields(factory,64,&fields)!=GX_OK) return GX_PROFILE;
  request[0]=0x0d;request[1]=1;request[26]=phase;
  memcpy(request+10,seed,16);
  for(guint i=0;i<4;i++) put16(request+2+2*i,fields.fdt[i]);
  return GX_OK;
}

GxResult
gx_preburst_fdt_equal (const guint8 down[16], const guint8 manual[16],
                       guint16 threshold, gboolean *equal)
{
  if (!equal) return GX_PROFILE;
  *equal=FALSE;
  if (!down || !manual || threshold>10) return GX_PROFILE;
  for(guint i=0;i<8;i++)
    if(ABS((gint)(le16(down+2*i)>>1)-(gint)(le16(manual+2*i)>>1))>threshold)
      return GX_OK;
  *equal=TRUE;
  return GX_OK;
}

GxResult
gx_raw_range_counts (const guint16 frame[GX_PIXELS], guint counts[3])
{
  if (!counts) return GX_PROFILE;
  memset(counts,0,3*sizeof *counts);
  if (!frame) return GX_PROFILE;
  for(guint y=2;y<62;y++) for(guint x=2;x<78;x++) {
    guint value=frame[y*80+x];
    counts[value<=100?0:value>=3800?2:1]++;
  }
  return GX_OK;
}
