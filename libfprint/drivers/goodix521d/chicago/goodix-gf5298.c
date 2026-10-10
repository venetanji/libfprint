/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GF5298 mode17 arithmetic, expressed independently from the observed
 * algorithm contract. No vendor instructions or private fixtures are linked. */
#include <string.h>
#include "goodix-gf5298.h"

guint16
goodix_gf5298_build_mask (const guint16 current[GOODIX_CHICAGO_PIXELS],
                         const guint16 offset[GOODIX_CHICAGO_PIXELS],
                         guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  gint16 difference[GOODIX_CHICAGO_PIXELS];
  gint row_mean[64];
  gint minimum = G_MAXINT, maximum = G_MININT;
  gint first = 0, fourth = 0;
  gint threshold, high_sum = 0, low_sum = 0;
  guint high_count = 0, low_count = 0;
  memset (mask, 0, GOODIX_CHICAGO_PIXELS);
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    difference[i] = (gint16) (offset[i] - current[i]);
  for (guint by = 0; by < 64; by += 16)
    for (guint bx = 0; bx < 80; bx += 16)
      {
        gint sum = 0;
        guint count = 0;
        for (guint y = by; y < by + 16; y++)
          for (guint x = bx; x < bx + 16; x++)
            if (current[y * 80 + x] >= 500)
              {
                sum += difference[y * 80 + x];
                count++;
              }
        gint mean = sum / (gint) MAX (count, 1u);
        minimum = MIN (minimum, mean);
        maximum = MAX (maximum, mean);
      }
  for (guint y = 0; y < 64; y++)
    {
      gint sum = 0;
      for (guint x = 0; x < 80; x++)
        sum += difference[y * 80 + x];
      row_mean[y] = sum / 80;
    }
  /* Compare the first/fourth row in each group of four, below row24. */
  for (guint group = 6; group < 16; group++)
    {
      first += row_mean[group * 4];
      fourth += row_mean[group * 4 + 3];
    }
  if (ABS (first / 10 - fourth / 10) > 400)
    return 0;
  /* Mode17 selects the 1730 baseline, with a 70% contrast threshold. */
  if (maximum - minimum > 1211)
    threshold = maximum > 4095 && minimum < 4095 ?
                maximum - (maximum - minimum) / 2 :
                minimum + (maximum - minimum) / 2;
  else
    threshold = minimum - 250;
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    if (difference[i] > threshold)
      {
        high_sum += difference[i];
        high_count++;
      }
    else
      {
        low_sum += difference[i];
        low_count++;
      }
  if (high_count == 0)
    return 0;
  gint high_mean = high_sum / (gint) high_count;
  gint low_mean = low_count ? low_sum / (gint) low_count : high_mean + 600;
  if (high_count < low_count + 150)
    threshold += (high_mean - low_mean) / 5;
  else
    threshold -= (high_mean - low_mean) / 4;
  threshold = (gint16) threshold;
  guint below = 0;
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    {
      if (difference[i] > threshold && current[i] < 3800)
        mask[i] = 255;
      else if (difference[i] < threshold && current[i] < 3800)
        below++;
    }
  if (below * 100 > GOODIX_CHICAGO_PIXELS * 30)
    {
      threshold = (gint16) (minimum + 150);
      for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
        mask[i] = difference[i] > threshold && current[i] < 3800 ? 255 : 0;
    }
  return (guint16) threshold;
}

void
goodix_gf5298_fill_mask (const guint16 current[GOODIX_CHICAGO_PIXELS],
                        guint8 mask[GOODIX_CHICAGO_PIXELS])
{
  /* In-place row-major filling is intentional: previously filled positions
   * participate in later searches, as do the original active endpoints. */
  for (gint y = 0; y < 64; y++)
    for (gint x = 0; x < 80; x++)
      if (!mask[y * 80 + x])
        {
          gint up = 0, down = 64, left = 0, right = 80;
          for (gint yy = y; yy >= 0; yy--)
            if (mask[yy * 80 + x]) { up = yy; break; }
          for (gint yy = y; yy < 64; yy++)
            if (mask[yy * 80 + x]) { down = yy; break; }
          for (gint xx = x; xx >= 0; xx--)
            if (mask[y * 80 + xx]) { left = xx; break; }
          for (gint xx = x; xx < 80; xx++)
            if (mask[y * 80 + xx]) { right = xx; break; }
          if ((up >= 2 && down <= 61) || (left >= 2 && right <= 77))
            mask[y * 80 + x] = 255;
        }
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    if (current[i] <= 100 || current[i] >= 3800)
      mask[i] = 0;
}

void
goodix_gf5298_corrected_signal (const guint16 current[GOODIX_CHICAGO_PIXELS],
                               const guint16 offset[GOODIX_CHICAGO_PIXELS],
                               guint16 signal[GOODIX_CHICAGO_PIXELS])
{
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    {
      /* The working words are tripled before the signed32 subtraction.
       * Keep the observed uint16 intermediate contract explicit. */
      gint value = (guint16) (3u * offset[i]) -
                   (guint16) (3u * current[i]) - 12285;
      signal[i] = value > 0 ? (guint16) value : 0;
    }
}

static guint16
median3 (guint16 a, guint16 b, guint16 c)
{
  return MAX (MIN (a, b), MIN (MAX (a, b), c));
}

void
goodix_gf5298_local_gain (const guint16 signal[GOODIX_CHICAGO_PIXELS],
                         guint16 gain[GOODIX_CHICAGO_PIXELS])
{
  guint16 normalized[GOODIX_CHICAGO_PIXELS];
  guint16 horizontal[GOODIX_CHICAGO_PIXELS];
  guint16 filtered[GOODIX_CHICAGO_PIXELS];
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    {
      guint32 value = ((guint32) signal[i] << 13);
      normalized[i] = gain[i] ? (value + (gain[i] >> 1)) / gain[i] : value;
    }
  memcpy (horizontal, normalized, sizeof (horizontal));
  memcpy (filtered, normalized, sizeof (filtered));
  for (guint y = 0; y < 64; y++)
    for (guint x = 1; x < 79; x++)
      {
        guint i = y * 80 + x;
        horizontal[i] = median3 (normalized[i - 1], normalized[i], normalized[i + 1]);
      }
  for (guint y = 1; y < 63; y++)
    for (guint x = 1; x < 79; x++)
      {
        guint i = y * 80 + x;
        filtered[i] = median3 (horizontal[i - 80], horizontal[i], horizontal[i + 80]);
      }
  for (guint i = 0; i < GOODIX_CHICAGO_PIXELS; i++)
    if (normalized[i] && filtered[i] &&
        ABS ((gint) normalized[i] - filtered[i]) > 1800)
      gain[i] = MIN (32767u, ((guint32) gain[i] * normalized[i] +
                             (filtered[i] >> 1)) / filtered[i]);
}

gboolean
goodix_gf5298_select_alternate (gint primary, gint alternate, gint correlation)
{
  const gint reference = 800, low = 1800, high = 3600;
  gint threshold;
  if (primary < reference)
    return FALSE;
  if (primary < low)
    return alternate < ((reference * 205) >> 8) && correlation < 235;
  if (primary < high)
    {
      threshold = (((primary - low) * reference * 179) / (high - low) +
                   reference * 205) >> 8;
      return alternate < threshold;
    }
  if (primary >= high + low)
    return TRUE;
  threshold = (primary - high) * reference * 2 / (high - low) +
              ((3 * reference * 128) >> 8);
  return alternate < threshold;
}
