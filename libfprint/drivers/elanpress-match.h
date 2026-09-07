/*
 * Image processing and correlation matching for ELAN press-type sensors
 *
 * Copyright (C) 2026 Mohammad Nauman
 * Derived from work by Filip Spanne (elanpress, 04f3:0c6e) and ideas from
 * dragosol/fpmatch (04f3:0c3d, 144x64 variant).
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

#pragma once

#include <glib.h>

/* version tag for the serialized print data */
#define ELANPRESS_PRINT_VERSION 2

typedef struct
{
  double mean;      /* mean of (frame - background), raw sensor units */
  double contrast;  /* std-dev of (frame - background), raw sensor units */
  double coverage;  /* fraction of pixels carrying ridge texture, 0..1 */
} ElanpressQuality;

typedef struct
{
  double rot_range;    /* degrees, search is +/- this */
  double rot_step;     /* degrees */
  double min_overlap;  /* fraction of the frame area that must overlap */
  double threshold;    /* accept when best score >= threshold */
  int    norm_win;     /* local normalisation window, pixels (odd) */
  int    candidates;   /* coarse candidates refined at full resolution */
  int    score_mode;   /* 0 peak NCC, 1 peak - sidelobe, 2 peak / sidelobe */
  int    sidelobe_r;   /* half-res px excluded around the peak for the sidelobe */
  double block_weight; /* score = (1-w) * peak + w * block_min */
  double reg_thresh;   /* mosaic: registration score needed to merge an image */
  int    max_side;     /* mosaic: canvas side limit, px */
} ElanpressMatchParams;

typedef struct
{
  double score;        /* the decision score, per score_mode */
  double peak;         /* best NCC */
  double sidelobe;     /* best coarse NCC away from the peak */
  double block_min;    /* at the peak: lowest NCC over a 3x3 grid of the overlap */
  double block_mean;   /* at the peak: mean NCC over that grid */
  int    dx, dy;       /* template offset relative to probe, full-res px */
  double rot;          /* probe rotation, degrees */
  int    overlap;      /* pixels that took part in the score */
} ElanpressMatchResult;

typedef struct _ElanpressProbe ElanpressProbe;

void      elanpress_match_params_default (ElanpressMatchParams *p);
void      elanpress_match_params_from_env (ElanpressMatchParams *p);

/* wire format -> row-major 16-bit frame */
void      elanpress_rotate_frame (const guint8 *raw,
                                  guint16      *out,
                                  int           w,
                                  int           h);

/* background subtraction + local normalisation -> 8-bit image; never NULL */
guint8 *  elanpress_process_frame (const guint16    *frame,
                                   const guint16    *bg,
                                   int               w,
                                   int               h,
                                   int               norm_win,
                                   ElanpressQuality *q);

/* raw frame statistics (no background), used to sanity-check a background */
void      elanpress_frame_stats (const guint16 *frame,
                                 int            w,
                                 int            h,
                                 double        *mean,
                                 double        *stddev);

/* zero-shift normalised cross-correlation of two processed images, -1..1 */
double    elanpress_similarity (const guint8 *a,
                                const guint8 *b,
                                int           w,
                                int           h);

ElanpressProbe *elanpress_probe_new (const guint8               *img,
                                     int                         w,
                                     int                         h,
                                     const ElanpressMatchParams *p);
void      elanpress_probe_free (ElanpressProbe *probe);

/* best correlation of the probe against one template image */
double    elanpress_probe_match (const ElanpressProbe *probe,
                                 const guint8         *tmpl,
                                 ElanpressMatchResult *res);

/* best correlation of the probe against a canvas of any size; mask (may be
 * NULL) marks the canvas pixels that hold image data */
double    elanpress_probe_match_canvas (const ElanpressProbe *probe,
                                        const guint8         *px,
                                        const guint8         *mask,
                                        int                   tw,
                                        int                   th,
                                        ElanpressMatchResult *res);

/* A mosaic fragment: enrolled images registered onto one canvas. */
typedef struct
{
  int     w, h;
  guint8 *px;
  guint8 *mask;
  int     n_images;
} ElanpressCanvas;

ElanpressCanvas *elanpress_canvas_new_from_image (const guint8 *img,
                                                  int           w,
                                                  int           h);
void            elanpress_canvas_free (ElanpressCanvas *c);

/* register every image against the growing fragments; an image whose best
 * registration scores below reg_thresh starts a new fragment. Returns an
 * array of ElanpressCanvas, cropped to their content. */
GPtrArray *     elanpress_mosaic_build (GPtrArray                  *images,
                                        int                         w,
                                        int                         h,
                                        const ElanpressMatchParams *p,
                                        double                      reg_thresh,
                                        int                         max_side);

/* convenience: prepare, match, free */
double    elanpress_match (const guint8               *probe,
                           const guint8               *tmpl,
                           int                         w,
                           int                         h,
                           const ElanpressMatchParams *p,
                           ElanpressMatchResult       *res);

/* PGM helpers (8-bit and 16-bit big-endian) */
gboolean  elanpress_pgm_write8 (const char   *path,
                                const guint8 *px,
                                int           w,
                                int           h);
gboolean  elanpress_pgm_write16 (const char    *path,
                                 const guint16 *px,
                                 int            w,
                                 int            h);
guint8 *  elanpress_pgm_read8 (const char *path,
                               int        *w,
                               int        *h);
guint16 * elanpress_pgm_read16 (const char *path,
                                int        *w,
                                int        *h);
