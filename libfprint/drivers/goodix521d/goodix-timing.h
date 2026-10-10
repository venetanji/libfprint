/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <glib.h>

/* Caller supplies monotonic timestamps. These clocks never control deadlines. */
typedef struct {
  gint64 start_us, last_us;
  guint64 action_id;
} GxTiming;

static inline void
gx_timing_begin (GxTiming *timing, guint64 id, gint64 now)
{
  *timing = (GxTiming) { now, now, id };
}

static inline gint64
gx_timing_mark (GxTiming *timing, gint64 now)
{
  gint64 elapsed = now > timing->last_us ? now - timing->last_us : 0;
  if (now > timing->last_us)
    timing->last_us = now;
  return elapsed;
}

static inline gint64
gx_timing_total (const GxTiming *timing, gint64 now)
{
  return now > timing->start_us ? now - timing->start_us : 0;
}
