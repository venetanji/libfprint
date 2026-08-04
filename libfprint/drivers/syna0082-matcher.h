/*
 * Independent small-area matcher for Synaptics/PQI 06cb:0082.
 *
 * This implementation contains no proprietary Synaptics code or data.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct
{
  guint   matches;
  guint   inliers;
  gdouble inlier_ratio;
  gdouble scale;
  gdouble rotation;
  gdouble coverage;
  gboolean accepted;
} Syna0082MatchScore;

GBytes *syna0082_matcher_extract (const guint8 *pixels,
                                  guint         width,
                                  guint         height,
                                  GError      **error);

gboolean syna0082_matcher_compare (const guint8       *reference,
                                   gsize               reference_length,
                                   const guint8       *probe,
                                   gsize               probe_length,
                                   Syna0082MatchScore *score,
                                   GError             **error);

G_END_DECLS
