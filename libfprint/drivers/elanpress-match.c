/*
 * Image processing and correlation matching for ELAN press-type sensors
 *
 * Copyright (C) 2026 Mohammad Nauman
 * Derived from work by Filip Spanne (elanpress, 04f3:0c6e) and ideas from
 * dragosol/fpmatch (04f3:0c3d, 144x64 variant).
 *
 * The imaged area of these sensors (64x88 px, ~3.2x4.4 mm on the 0c3d pad
 * this was written for) is far too small for minutiae matching: NBIS
 * bozorth3 refuses to score prints with fewer than 10 minutiae, and a patch
 * this size yields 1-5. Prints therefore store the enrolled images
 * themselves and matching is done by zero-mean normalised cross-correlation
 * of locally-normalised ridge images, searched over translation and a
 * small rotation range, coarse-to-fine.
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
 */

#include "elanpress-match.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------ params */

void
elanpress_match_params_default (ElanpressMatchParams *p)
{
  p->rot_range = 20.0;
  p->rot_step = 4.0;
  p->min_overlap = 0.40;
  p->threshold = 0.76;
  p->norm_win = 11;
  p->candidates = 4;
  p->score_mode = 0;
  p->sidelobe_r = 4;
  p->block_weight = 0.0;
  p->reg_thresh = 0.75;
  p->max_side = 256;
}

static gboolean
env_double (const char *name, double *out)
{
  const char *v = g_getenv (name);
  char *end;
  double d;

  if (!v || !*v)
    return FALSE;
  d = g_ascii_strtod (v, &end);
  if (end == v)
    return FALSE;
  *out = d;
  return TRUE;
}

void
elanpress_match_params_from_env (ElanpressMatchParams *p)
{
  double d;

  elanpress_match_params_default (p);
  if (env_double ("FP_ELANPRESS_THRESHOLD", &d))
    p->threshold = d;
  if (env_double ("FP_ELANPRESS_ROT_RANGE", &d))
    p->rot_range = d;
  if (env_double ("FP_ELANPRESS_ROT_STEP", &d) && d > 0)
    p->rot_step = d;
  if (env_double ("FP_ELANPRESS_MIN_OVERLAP", &d))
    p->min_overlap = d;
  if (env_double ("FP_ELANPRESS_NORM_WIN", &d) && d >= 3)
    p->norm_win = ((int) d) | 1;
  if (env_double ("FP_ELANPRESS_CANDIDATES", &d) && d >= 1)
    p->candidates = (int) d;
  if (env_double ("FP_ELANPRESS_SCORE_MODE", &d))
    p->score_mode = CLAMP ((int) d, 0, 2);
  if (env_double ("FP_ELANPRESS_SIDELOBE_R", &d) && d >= 1)
    p->sidelobe_r = (int) d;
  if (env_double ("FP_ELANPRESS_BLOCK_WEIGHT", &d))
    p->block_weight = CLAMP (d, 0.0, 1.0);
  if (env_double ("FP_ELANPRESS_REG_THRESH", &d))
    p->reg_thresh = d;
  if (env_double ("FP_ELANPRESS_MAX_SIDE", &d) && d >= 64)
    p->max_side = (int) d;
}

/* ------------------------------------------------------- raw frames */

/* the wire format is column-major with frame-height-tall columns (see
 * elan_save_frame in the elan driver); rotate to row-major */
void
elanpress_rotate_frame (const guint8 *raw, guint16 *out, int w, int h)
{
  const guint16 *in = (const guint16 *) raw;

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      out[y * w + x] = GUINT16_FROM_LE (in[y + x * h]);
}

void
elanpress_frame_stats (const guint16 *frame, int w, int h,
                       double *mean, double *stddev)
{
  int n = w * h;
  double s = 0, ss = 0;

  for (int i = 0; i < n; i++)
    {
      s += frame[i];
      ss += (double) frame[i] * frame[i];
    }
  *mean = s / n;
  *stddev = sqrt (MAX (ss / n - (s / n) * (s / n), 0.0));
}

/* ---------------------------------------------------- preprocessing */

/* Background subtraction, then local mean/std normalisation over a
 * norm_win x norm_win box. This flattens uneven contact pressure and
 * equalises ridge contrast so that the correlation sees ridge *shape*,
 * not brightness. Regions with no texture (finger not touching) collapse
 * towards 128 and therefore contribute nothing to the correlation. */
guint8 *
elanpress_process_frame (const guint16 *frame, const guint16 *bg,
                         int w, int h, int norm_win, ElanpressQuality *q)
{
  int n = w * h;
  int sw = w + 1;
  int r = MAX (norm_win, 3) / 2;
  float *d = calloc ((size_t) n, sizeof (float));
  /* calloc rather than g_new0: static analysers model calloc as zeroing
   * memory, which lets them follow the integral-image recurrence below */
  double *S = calloc ((size_t) sw * (h + 1), sizeof (double));
  double *SS = calloc ((size_t) sw * (h + 1), sizeof (double));
  guint8 *out = g_new (guint8, n);
  double sum = 0, sq = 0, mean, gstd, eps;
  int textured = 0;

  for (int i = 0; i < n; i++)
    {
      d[i] = (float) frame[i] - (float) (bg ? bg[i] : 0);
      sum += d[i];
      sq += (double) d[i] * d[i];
    }
  mean = sum / n;
  gstd = sqrt (MAX (sq / n - mean * mean, 0.0));

  /* integral images with a zero first row and column, built from running
   * row sums so every value read was written earlier in these loops */
  for (int x = 0; x < sw; x++)
    {
      S[x] = 0;
      SS[x] = 0;
    }
  for (int y = 0; y < h; y++)
    {
      double rs = 0, rss = 0;
      size_t row = (size_t) (y + 1) * sw, prev = (size_t) y * sw;

      S[row] = 0;
      SS[row] = 0;
      for (int x = 0; x < w; x++)
        {
          double p = d[y * w + x];

          rs += p;
          rss += p * p;
          S[row + x + 1] = rs + S[prev + x + 1];
          SS[row + x + 1] = rss + SS[prev + x + 1];
        }
    }

  /* eps keeps flat, finger-free regions from being amplified into noise */
  eps = 0.25 * gstd + 1.0;

  for (int y = 0; y < h; y++)
    {
      int y0 = MAX (y - r, 0), y1 = MIN (y + r + 1, h);
      for (int x = 0; x < w; x++)
        {
          int x0 = MAX (x - r, 0), x1 = MIN (x + r + 1, w);
          double cnt = (double) (y1 - y0) * (x1 - x0);
          double s = S[(size_t) y1 * sw + x1] - S[(size_t) y0 * sw + x1] -
                     S[(size_t) y1 * sw + x0] + S[(size_t) y0 * sw + x0];
          double ss = SS[(size_t) y1 * sw + x1] - SS[(size_t) y0 * sw + x1] -
                      SS[(size_t) y1 * sw + x0] + SS[(size_t) y0 * sw + x0];
          double m = s / cnt;
          double sd = sqrt (MAX (ss / cnt - m * m, 0.0));
          double z = (d[y * w + x] - m) / (sd + eps);
          double v = 128.0 + 40.0 * z;

          if (sd > 0.35 * gstd)
            textured++;
          out[y * w + x] = (guint8) CLAMP (v, 0.0, 255.0);
        }
    }

  if (q)
    {
      q->mean = mean;
      q->contrast = gstd;
      q->coverage = (double) textured / n;
    }

  free (d);
  free (S);
  free (SS);
  return out;
}

double
elanpress_similarity (const guint8 *a, const guint8 *b, int w, int h)
{
  int n = w * h;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, ma, mb, va, vb, cov;

  for (int i = 0; i < n; i++)
    {
      sa += a[i];
      sb += b[i];
      saa += (double) a[i] * a[i];
      sbb += (double) b[i] * b[i];
      sab += (double) a[i] * b[i];
    }
  ma = sa / n;
  mb = sb / n;
  va = saa / n - ma * ma;
  vb = sbb / n - mb * mb;
  cov = sab / n - ma * mb;
  if (va <= 0 || vb <= 0)
    return 0.0;
  return cov / sqrt (va * vb);
}

/* ---------------------------------------------------------- layers */

typedef struct
{
  int     w, h;
  gint16 *v;   /* value - 128, 0 where invalid */
  guint8 *m;   /* validity mask */
} Layer;

static void
layer_init (Layer *l, int w, int h)
{
  l->w = w;
  l->h = h;
  l->v = g_new0 (gint16, (size_t) w * h);
  l->m = g_new0 (guint8, (size_t) w * h);
}

static void
layer_clear (Layer *l)
{
  g_clear_pointer (&l->v, g_free);
  g_clear_pointer (&l->m, g_free);
}

static void
layer_from_image (Layer *l, const guint8 *img, const guint8 *mask, int w, int h)
{
  layer_init (l, w, h);
  for (int i = 0; i < w * h; i++)
    {
      if (mask && !mask[i])
        continue;
      l->v[i] = (gint16) img[i] - 128;
      l->m[i] = 1;
    }
}

/* rotate img by deg around its centre into a same-size layer; pixels whose
 * source falls outside the image are masked out */
static void
layer_rotated (Layer *l, const guint8 *img, int w, int h, double deg)
{
  double th = deg * M_PI / 180.0;
  double c = cos (th), s = sin (th);
  double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

  layer_init (l, w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        double dx = x - cx, dy = y - cy;
        double sx = c * dx + s * dy + cx;
        double sy = -s * dx + c * dy + cy;
        int x0 = (int) floor (sx), y0 = (int) floor (sy);
        double fx = sx - x0, fy = sy - y0;
        double v;

        if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h)
          continue;
        v = (1 - fx) * (1 - fy) * img[y0 * w + x0] +
            fx * (1 - fy) * img[y0 * w + x0 + 1] +
            (1 - fx) * fy * img[(y0 + 1) * w + x0] +
            fx * fy * img[(y0 + 1) * w + x0 + 1];
        l->v[y * w + x] = (gint16) lrint (v - 128.0);
        l->m[y * w + x] = 1;
      }
}

/* 2x2 box downsample; a coarse pixel is valid only if all four are */
static void
layer_half (Layer *l, const Layer *src)
{
  int w = src->w / 2, h = src->h / 2;

  layer_init (l, w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const gint16 *p = src->v + (2 * y) * src->w + 2 * x;
        const guint8 *m = src->m + (2 * y) * src->w + 2 * x;

        if (m[0] && m[1] && m[src->w] && m[src->w + 1])
          {
            l->v[y * w + x] = (p[0] + p[1] + p[src->w] + p[src->w + 1]) / 4;
            l->m[y * w + x] = 1;
          }
      }
}

/* NCC of a (fixed) against b shifted by (dx, dy): a(x, y) pairs with
 * b(x - dx, y - dy). Returns -2 when the overlap is below min_n. */
static double
layer_ncc (const Layer *a, const Layer *b, int dx, int dy, int min_n,
           int *n_out)
{
  int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
  int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
  gint64 sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int n = 0;
  double ma, mb, va, vb, cov;

  if (x1 - x0 <= 0 || y1 - y0 <= 0)
    return -2.0;
  if ((x1 - x0) * (y1 - y0) < min_n)
    return -2.0;

  for (int y = y0; y < y1; y++)
    {
      const gint16 *av = a->v + y * a->w;
      const guint8 *am = a->m + y * a->w;
      const gint16 *bv = b->v + (y - dy) * b->w - dx;
      const guint8 *bm = b->m + (y - dy) * b->w - dx;

      for (int x = x0; x < x1; x++)
        {
          if (am[x] && bm[x])
            {
              int p = av[x], q = bv[x];
              sa += p;
              sb += q;
              saa += p * p;
              sbb += q * q;
              sab += p * q;
              n++;
            }
        }
    }

  if (n_out)
    *n_out = n;
  if (n < min_n)
    return -2.0;

  ma = (double) sa / n;
  mb = (double) sb / n;
  va = (double) saa / n - ma * ma;
  vb = (double) sbb / n - mb * mb;
  cov = (double) sab / n - ma * mb;
  if (va <= 1e-9 || vb <= 1e-9)
    return -2.0;
  return cov / sqrt (va * vb);
}

/* NCC of a against shifted b restricted to the a-rectangle [x0,x1)x[y0,y1) */
static double
layer_ncc_rect (const Layer *a, const Layer *b, int dx, int dy,
                int x0, int y0, int x1, int y1, int min_n)
{
  gint64 sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int n = 0;
  double ma, mb, va, vb, cov;

  x0 = MAX (x0, MAX (0, dx));
  x1 = MIN (x1, MIN (a->w, b->w + dx));
  y0 = MAX (y0, MAX (0, dy));
  y1 = MIN (y1, MIN (a->h, b->h + dy));
  for (int y = y0; y < y1; y++)
    {
      const gint16 *av = a->v + y * a->w;
      const guint8 *am = a->m + y * a->w;
      const gint16 *bv = b->v + (y - dy) * b->w - dx;
      const guint8 *bm = b->m + (y - dy) * b->w - dx;

      for (int x = x0; x < x1; x++)
        if (am[x] && bm[x])
          {
            int p = av[x], q = bv[x];
            sa += p;
            sb += q;
            saa += p * p;
            sbb += q * q;
            sab += p * q;
            n++;
          }
    }
  if (n < min_n)
    return -2.0;
  ma = (double) sa / n;
  mb = (double) sb / n;
  va = (double) saa / n - ma * ma;
  vb = (double) sbb / n - mb * mb;
  cov = (double) sab / n - ma * mb;
  if (va <= 1e-9 || vb <= 1e-9)
    return -2.0;
  return cov / sqrt (va * vb);
}

/* consistency of the peak alignment over a 3x3 grid of the overlap */
static void
layer_block_stats (const Layer *a, const Layer *b, int dx, int dy,
                   double *bmin, double *bmean)
{
  int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
  int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
  int n = 0;
  double sum = 0, mn = 2.0;

  for (int by = 0; by < 3; by++)
    for (int bx = 0; bx < 3; bx++)
      {
        int bx0 = x0 + (x1 - x0) * bx / 3, bx1 = x0 + (x1 - x0) * (bx + 1) / 3;
        int by0 = y0 + (y1 - y0) * by / 3, by1 = y0 + (y1 - y0) * (by + 1) / 3;
        double s = layer_ncc_rect (a, b, dx, dy, bx0, by0, bx1, by1, 64);

        if (s <= -2.0)
          continue;
        sum += s;
        mn = MIN (mn, s);
        n++;
      }
  *bmin = n ? mn : -1.0;
  *bmean = n ? sum / n : -1.0;
}

/* ----------------------------------------------------------- probe */

struct _ElanpressProbe
{
  int                  w, h, n_rot;
  double              *angles;
  Layer               *full;   /* per rotation, full resolution */
  Layer               *half;   /* per rotation, half resolution */
  ElanpressMatchParams p;
};

ElanpressProbe *
elanpress_probe_new (const guint8 *img, int w, int h,
                     const ElanpressMatchParams *p)
{
  ElanpressProbe *pr = g_new0 (ElanpressProbe, 1);
  int n_rot;

  pr->w = w;
  pr->h = h;
  pr->p = *p;
  if (pr->p.rot_step <= 0)
    pr->p.rot_step = 4.0;
  n_rot = (int) floor (pr->p.rot_range / pr->p.rot_step + 1e-9);
  pr->n_rot = 2 * n_rot + 1;
  pr->angles = g_new (double, pr->n_rot);
  pr->full = g_new0 (Layer, pr->n_rot);
  pr->half = g_new0 (Layer, pr->n_rot);
  for (int i = 0; i < pr->n_rot; i++)
    {
      pr->angles[i] = (i - n_rot) * pr->p.rot_step;
      if (pr->angles[i] == 0.0)
        layer_from_image (&pr->full[i], img, NULL, w, h);
      else
        layer_rotated (&pr->full[i], img, w, h, pr->angles[i]);
      layer_half (&pr->half[i], &pr->full[i]);
    }
  return pr;
}

void
elanpress_probe_free (ElanpressProbe *pr)
{
  if (!pr)
    return;
  for (int i = 0; i < pr->n_rot; i++)
    {
      layer_clear (&pr->full[i]);
      layer_clear (&pr->half[i]);
    }
  g_free (pr->full);
  g_free (pr->half);
  g_free (pr->angles);
  g_free (pr);
}

typedef struct
{
  double score;
  int    rot, dx, dy;
} Cand;

static void
cand_insert (Cand *c, int k, double score, int rot, int dx, int dy)
{
  /* replace a nearby candidate if this one is better, else the worst */
  int worst = 0;

  for (int i = 0; i < k; i++)
    {
      if (c[i].rot == rot && abs (c[i].dx - dx) <= 1 && abs (c[i].dy - dy) <= 1)
        {
          if (score > c[i].score)
            {
              c[i].score = score;
              c[i].dx = dx;
              c[i].dy = dy;
            }
          return;
        }
      if (c[i].score < c[worst].score)
        worst = i;
    }
  if (score > c[worst].score)
    {
      c[worst].score = score;
      c[worst].rot = rot;
      c[worst].dx = dx;
      c[worst].dy = dy;
    }
}

double
elanpress_probe_match_canvas (const ElanpressProbe *pr, const guint8 *px,
                              const guint8 *mask, int tw, int th,
                              ElanpressMatchResult *res)
{
  Layer tfull, thalf;
  int k = MAX (pr->p.candidates, 1);
  Cand *cands = g_new (Cand, k);
  int pw = pr->half[0].w, ph = pr->half[0].h;
  int hw, hh, mw, mh, min_n_half, min_n_full, best_cand = -1;
  float *maps = NULL;
  ElanpressMatchResult best = { -2.0, -2.0, -2.0, -1.0, -1.0, 0, 0, 0.0, 0 };
  int best_rot_idx = -1;

  layer_from_image (&tfull, px, mask, tw, th);
  layer_half (&thalf, &tfull);
  hw = thalf.w;
  hh = thalf.h;
  /* offsets range over every placement with any overlap */
  mw = pw + hw - 1;
  mh = ph + hh - 1;
  min_n_half = (int) (pr->p.min_overlap * pw * ph);
  min_n_full = (int) (pr->p.min_overlap * pr->w * pr->h);
  if (pr->p.score_mode != 0)
    {
      maps = g_new (float, (size_t) pr->n_rot * mw * mh);
      for (size_t i = 0; i < (size_t) pr->n_rot * mw * mh; i++)
        maps[i] = -2.0f;
    }

  for (int i = 0; i < k; i++)
    {
      cands[i].score = -3.0;
      cands[i].rot = -1;
      cands[i].dx = cands[i].dy = 0;
    }

  /* coarse: every rotation, every translation, at half resolution.
   * probe(x, y) pairs with canvas(x - dx, y - dy). */
  for (int r = 0; r < pr->n_rot; r++)
    {
      const Layer *p_h = &pr->half[r];

      for (int dy = -(hh - 1); dy <= ph - 1; dy++)
        for (int dx = -(hw - 1); dx <= pw - 1; dx++)
          {
            double s = layer_ncc (p_h, &thalf, dx, dy, min_n_half, NULL);
            if (s > -2.0)
              {
                if (maps)
                  maps[((size_t) r * mh + (dy + hh - 1)) * mw + (dx + hw - 1)] = (float) s;
                cand_insert (cands, k, s, r, dx, dy);
              }
          }
    }

  /* fine: refine each candidate at full resolution */
  for (int i = 0; i < k; i++)
    {
      const Layer *pf;

      if (cands[i].rot < 0)
        continue;
      pf = &pr->full[cands[i].rot];
      for (int dy = 2 * cands[i].dy - 2; dy <= 2 * cands[i].dy + 2; dy++)
        for (int dx = 2 * cands[i].dx - 2; dx <= 2 * cands[i].dx + 2; dx++)
          {
            int n = 0;
            double s = layer_ncc (pf, &tfull, dx, dy, min_n_full, &n);

            if (s > best.peak)
              {
                best.peak = s;
                best.dx = dx;
                best.dy = dy;
                best.rot = pr->angles[cands[i].rot];
                best.overlap = n;
                best_cand = i;
                best_rot_idx = cands[i].rot;
              }
          }
    }

  if (best_rot_idx >= 0)
    layer_block_stats (&pr->full[best_rot_idx], &tfull, best.dx, best.dy,
                       &best.block_min, &best.block_mean);

  /* Sidelobe: the best coarse score anywhere except a window around the
   * peak, over every rotation (experimental, score_mode 1 / 2). */
  if (maps && best_cand >= 0)
    {
      int cdx = cands[best_cand].dx, cdy = cands[best_cand].dy;
      int R = MAX (pr->p.sidelobe_r, 1);

      for (int r = 0; r < pr->n_rot; r++)
        for (int y = 0; y < mh; y++)
          for (int x = 0; x < mw; x++)
            {
              int dx = x - (hw - 1), dy = y - (hh - 1);
              float s;

              if (abs (dx - cdx) <= R && abs (dy - cdy) <= R)
                continue;
              s = maps[((size_t) r * mh + y) * mw + x];
              if (s > best.sidelobe)
                best.sidelobe = s;
            }
    }

  switch (pr->p.score_mode)
    {
    case 1:
      best.score = best.sidelobe > -2.0 ? best.peak - best.sidelobe : best.peak;
      break;

    case 2:
      best.score = best.sidelobe > 0.05 ? best.peak / best.sidelobe - 1.0 : best.peak;
      break;

    default:
      best.score = best.peak;
    }
  if (pr->p.block_weight > 0 && best.block_min > -1.0)
    best.score = (1.0 - pr->p.block_weight) * best.score +
                 pr->p.block_weight * best.block_min;

  g_free (maps);
  g_free (cands);
  layer_clear (&tfull);
  layer_clear (&thalf);

  if (res)
    *res = best;
  return best.score;
}

double
elanpress_probe_match (const ElanpressProbe *pr, const guint8 *tmpl,
                       ElanpressMatchResult *res)
{
  return elanpress_probe_match_canvas (pr, tmpl, NULL, pr->w, pr->h, res);
}

/* ---------------------------------------------------------- mosaic */

ElanpressCanvas *
elanpress_canvas_new_from_image (const guint8 *img, int w, int h)
{
  ElanpressCanvas *c = g_new0 (ElanpressCanvas, 1);

  c->w = w;
  c->h = h;
  c->px = g_memdup2 (img, (gsize) w * h);
  c->mask = g_new (guint8, (gsize) w * h);
  memset (c->mask, 1, (gsize) w * h);
  c->n_images = 1;
  return c;
}

void
elanpress_canvas_free (ElanpressCanvas *c)
{
  if (!c)
    return;
  g_free (c->px);
  g_free (c->mask);
  g_free (c);
}

/* Paste img, rotated by rot degrees, so that rotated-probe pixel (x, y)
 * lands on canvas pixel (x - dx, y - dy) -- the geometry layer_ncc scores.
 * The canvas grows as needed up to max_side; pixels already holding data
 * are kept (no blending, so ridges stay sharp). */
static gboolean
canvas_paste (ElanpressCanvas *c, const guint8 *img, int w, int h,
              int dx, int dy, double rot, int max_side)
{
  Layer l;
  int x0 = -dx, y0 = -dy;              /* canvas position of probe pixel (0,0) */
  int nx0 = MIN (0, x0), ny0 = MIN (0, y0);
  int nx1 = MAX (c->w, x0 + w), ny1 = MAX (c->h, y0 + h);
  int nw = nx1 - nx0, nh = ny1 - ny0;

  if (nw > max_side || nh > max_side)
    return FALSE;

  if (nw != c->w || nh != c->h)
    {
      guint8 *px = g_new0 (guint8, (gsize) nw * nh);
      guint8 *mask = g_new0 (guint8, (gsize) nw * nh);

      for (int y = 0; y < c->h; y++)
        {
          memcpy (px + (gsize) (y - ny0) * nw - nx0, c->px + (gsize) y * c->w, c->w);
          memcpy (mask + (gsize) (y - ny0) * nw - nx0, c->mask + (gsize) y * c->w, c->w);
        }
      g_free (c->px);
      g_free (c->mask);
      c->px = px;
      c->mask = mask;
      c->w = nw;
      c->h = nh;
      x0 -= nx0;
      y0 -= ny0;
    }

  if (rot == 0.0)
    layer_from_image (&l, img, NULL, w, h);
  else
    layer_rotated (&l, img, w, h, rot);

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        int cx = x0 + x, cy = y0 + y;

        if (!l.m[y * w + x])
          continue;
        if (c->mask[cy * c->w + cx])
          continue;
        c->px[cy * c->w + cx] = (guint8) (l.v[y * w + x] + 128);
        c->mask[cy * c->w + cx] = 1;
      }
  layer_clear (&l);
  c->n_images++;
  return TRUE;
}

GPtrArray *
elanpress_mosaic_build (GPtrArray *images, int w, int h,
                        const ElanpressMatchParams *p,
                        double reg_thresh, int max_side)
{
  GPtrArray *frags = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_canvas_free);
  GPtrArray *pending = g_ptr_array_new ();

  for (guint i = 0; i < images->len; i++)
    g_ptr_array_add (pending, g_ptr_array_index (images, i));

  /* two passes: an image that fails to register at first may register
   * once its neighbours have grown a fragment */
  for (int pass = 0; pass < 2 && pending->len > 0; pass++)
    {
      GPtrArray *still = g_ptr_array_new ();

      for (guint i = 0; i < pending->len; i++)
        {
          const guint8 *img = g_ptr_array_index (pending, i);
          ElanpressProbe *probe = elanpress_probe_new (img, w, h, p);
          ElanpressMatchResult best = { -2.0, -2.0, -2.0, -1.0, -1.0, 0, 0, 0.0, 0 };
          int best_f = -1;

          for (guint f = 0; f < frags->len; f++)
            {
              ElanpressCanvas *c = g_ptr_array_index (frags, f);
              ElanpressMatchResult r;

              elanpress_probe_match_canvas (probe, c->px, c->mask, c->w, c->h, &r);
              if (r.peak > best.peak)
                {
                  best = r;
                  best_f = f;
                }
            }
          elanpress_probe_free (probe);

          if (best_f >= 0 && best.peak >= reg_thresh &&
              canvas_paste (g_ptr_array_index (frags, best_f), img, w, h,
                            best.dx, best.dy, best.rot, max_side))
            continue;

          if (pass == 0 && frags->len > 0)
            g_ptr_array_add (still, (gpointer) img);     /* retry later */
          else
            g_ptr_array_add (frags, elanpress_canvas_new_from_image (img, w, h));
        }
      g_ptr_array_unref (pending);
      pending = still;
    }
  g_ptr_array_unref (pending);

  return frags;
}

double
elanpress_match (const guint8 *probe, const guint8 *tmpl, int w, int h,
                 const ElanpressMatchParams *p, ElanpressMatchResult *res)
{
  ElanpressProbe *pr = elanpress_probe_new (probe, w, h, p);
  double s = elanpress_probe_match (pr, tmpl, res);

  elanpress_probe_free (pr);
  return s;
}

/* ------------------------------------------------------------- PGM */

gboolean
elanpress_pgm_write8 (const char *path, const guint8 *px, int w, int h)
{
  FILE *f = fopen (path, "wb");

  if (!f)
    return FALSE;
  fprintf (f, "P5\n%d %d\n255\n", w, h);
  fwrite (px, 1, (size_t) w * h, f);
  return fclose (f) == 0;
}

gboolean
elanpress_pgm_write16 (const char *path, const guint16 *px, int w, int h)
{
  FILE *f = fopen (path, "wb");

  if (!f)
    return FALSE;
  fprintf (f, "P5\n%d %d\n65535\n", w, h);
  for (int i = 0; i < w * h; i++)
    {
      guint8 b[2] = { px[i] >> 8, px[i] & 0xff };
      fwrite (b, 1, 2, f);
    }
  return fclose (f) == 0;
}

static gboolean
pgm_header (FILE *f, int *w, int *h, int *maxval)
{
  char magic[3] = { 0 };
  int c;

  if (fscanf (f, "%2s", magic) != 1 || strcmp (magic, "P5") != 0)
    return FALSE;
  /* skip whitespace and comments */
  for (int i = 0; i < 3; i++)
    {
      int *dst = i == 0 ? w : i == 1 ? h : maxval;
      do
        {
          c = fgetc (f);
          if (c == '#')
            while (c != '\n' && c != EOF)
              c = fgetc (f);
        }
      while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
      if (c == EOF)
        return FALSE;
      ungetc (c, f);
      if (fscanf (f, "%d", dst) != 1)
        return FALSE;
    }
  /* exactly one whitespace byte separates the header from the pixels */
  if (fgetc (f) == EOF)
    return FALSE;
  return *w > 0 && *h > 0;
}

guint8 *
elanpress_pgm_read8 (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  int maxval;
  guint8 *px;

  if (!f)
    return NULL;
  if (!pgm_header (f, w, h, &maxval) || maxval != 255)
    {
      fclose (f);
      return NULL;
    }
  px = g_new (guint8, (size_t) *w * *h);
  if (fread (px, 1, (size_t) *w * *h, f) != (size_t) *w * *h)
    {
      g_free (px);
      px = NULL;
    }
  fclose (f);
  return px;
}

guint16 *
elanpress_pgm_read16 (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  int maxval;
  guint16 *px;

  if (!f)
    return NULL;
  if (!pgm_header (f, w, h, &maxval) || maxval <= 255)
    {
      fclose (f);
      return NULL;
    }
  px = g_new (guint16, (size_t) *w * *h);
  for (int i = 0; i < *w * *h; i++)
    {
      int hi = fgetc (f);
      int lo = hi == EOF ? EOF : fgetc (f);
      if (hi == EOF || lo == EOF)
        {
          g_free (px);
          fclose (f);
          return NULL;
        }
      px[i] = (guint16) ((hi << 8) | lo);
    }
  fclose (f);
  return px;
}
