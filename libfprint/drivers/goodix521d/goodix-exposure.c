// SPDX-License-Identifier: LGPL-2.1-or-later
/* GF5298 exposure arithmetic, independently implemented from the qualified
 * mode29/mode1 contract. Oracle regressions use authored synthetic planes.
 * This module contains no vendor code, device access or image preprocessing.
 */
#include "goodix-exposure.h"

#include <float.h>
#include <string.h>

#if defined(__FAST_MATH__)
#error "GF5298 exposure feedback requires strict floating-point arithmetic"
#endif
_Static_assert (sizeof (float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
                FLT_MAX_EXP == 128, "GF5298 exposure requires binary32 float");

/* Force the SSE single-precision rounding points even on excess-precision
 * targets. The subsequent integer conversion truncates toward zero.
 */
static float
float32_divide (float numerator, float denominator)
{
  volatile float result = numerator / denominator;
  return result;
}

static bool
valid_parameters (unsigned int integration, unsigned int initial_dac)
{
  return integration >= 1 && integration <= 510 && initial_dac <= 511;
}

static void
initialize (GxExposureState *state, unsigned int integration,
            unsigned int initial_dac)
{
  /* The qualified constant is a DOUBLE, rounded only after multiplication. */
  volatile float slope = (float) ((double) integration * 0.326);
  int lower = (int) initial_dac - (int) float32_divide (300.0f, slope) - 1;
  int upper = (int) initial_dac + (int) float32_divide (800.0f, slope) + 1;

  /* For the accepted range these results fit signed16 before the clamps. */
  state->integration = (uint16_t) integration;
  state->initial_dac = (uint16_t) initial_dac;
  state->dac = (uint16_t) initial_dac;
  state->lower = (uint16_t) (lower < 0 ? 0 : lower);
  state->upper = (uint16_t) (upper > 511 ? 511 : upper);
  state->outer_low = 800;
  state->outer_high = 3000;
  state->inner_low = 1100;
  state->inner_high = 2700;
  state->mean = 0;
  state->slope = slope;
  state->initialized = 1;
}

static bool
valid_state (const GxExposureState *state)
{
  GxExposureState expected = {0};

  if (state->initialized != 1 ||
      !valid_parameters (state->integration, state->initial_dac) ||
      state->dac > 511 || state->mean > 4095 || state->adjusting > 1 ||
      state->high_count > 3 || state->low_count > 3)
    return false;

  initialize (&expected, state->integration, state->initial_dac);
  return state->slope == expected.slope &&
         state->lower == expected.lower && state->upper == expected.upper &&
         state->outer_low == expected.outer_low &&
         state->outer_high == expected.outer_high &&
         state->inner_low == expected.inner_low &&
         state->inner_high == expected.inner_high;
}

GxExposureResult
gx_exposure_reset (GxExposureState *state, unsigned int integration,
                   unsigned int initial_dac)
{
  GxExposureState next;

  if (!state || !valid_parameters (integration, initial_dac))
    return GX_EXPOSURE_INVALID;

  /* Explicitly zero padding too, making cleared/new-epoch state deterministic. */
  memset (&next, 0, sizeof next);
  initialize (&next, integration, initial_dac);
  memcpy (state, &next, sizeof next);
  return GX_EXPOSURE_OK;
}

void
gx_exposure_clear (GxExposureState *state)
{
  if (state)
    memset (state, 0, sizeof *state);
}

GxExposureResult
gx_exposure_reinitialize (GxExposureState *state, unsigned int integration,
                          unsigned int initial_dac)
{
  GxExposureState next;

  if (!state || !valid_parameters (integration, initial_dac))
    return GX_EXPOSURE_INVALID;
  if (!valid_state (state))
    return GX_EXPOSURE_STATE;

  next = *state;
  /* The vendor initializer preserves adjusting/steps/high_count/low_count. */
  initialize (&next, integration, initial_dac);
  *state = next;
  return GX_EXPOSURE_OK;
}

static bool
valid_words (const uint16_t dacs[4])
{
  for (size_t i = 0; i < 4; i++)
    if (dacs[i] > 511)
      return false;
  return true;
}

GxExposureResult
gx_exposure_shift (uint16_t dacs[4], unsigned int target)
{
  uint16_t minimum;

  if (!dacs || target > 511 || !valid_words (dacs))
    return GX_EXPOSURE_INVALID;

  minimum = dacs[0];
  for (size_t i = 1; i < 4; i++)
    if (dacs[i] < minimum)
      minimum = dacs[i];

  for (size_t i = 0; i < 4; i++)
    {
      if (target > minimum)
        dacs[i] = (uint16_t) ((dacs[i] + (target - minimum)) & 511u);
      else
        dacs[i] = (uint16_t) (dacs[i] - (minimum - target));
    }
  return GX_EXPOSURE_OK;
}

static void
measure (const uint16_t *background, const uint16_t *frame,
         GxExposureState *state, GxExposureMeasurement *measurement)
{
  int32_t high_sum = 0, low_sum = 0, cutoff = 0;
  int32_t high_count = 0, low_count = 0, negative_count = 0;
  uint32_t sum = 0;
  const float interior = (GX_EXPOSURE_WIDTH - 2) * (GX_EXPOSURE_HEIGHT - 2);

  /* All input values are <=4095, so signed32 sums cannot overflow. */
  for (size_t i = 0; i < GX_EXPOSURE_PIXELS; i++)
    {
      int32_t difference = (int32_t) background[i] - frame[i];

      if (difference > 3800)
        {
          high_sum += difference;
          high_count++;
        }
      else
        {
          low_sum += difference;
          low_count++;
        }
      if (difference < 0)
        negative_count++;
    }

  if (high_count)
    {
      int32_t high_mean = high_sum / high_count;
      int32_t low_mean = low_count ? low_sum / low_count : 0;

      cutoff = 3800 - (high_mean - low_mean) / 4;
    }

  for (size_t y = 1; y < GX_EXPOSURE_HEIGHT - 1; y++)
    for (size_t x = 1; x < GX_EXPOSURE_WIDTH - 1; x++)
      {
        size_t i = y * GX_EXPOSURE_WIDTH + x;
        int32_t difference = (int32_t) background[i] - frame[i];
        bool selected = cutoff != 0 && difference >= cutoff;

        if (frame[i])
          measurement->nonzero++;
        if (negative_count * 10 > GX_EXPOSURE_PIXELS && difference < 0)
          selected = false;
        /* Failure of the cutoff branch falls back to frame<=3800. */
        if (selected || frame[i] <= 3800)
          {
            sum += frame[i];
            measurement->selected++;
          }
      }

  state->mean = measurement->selected ?
                (uint16_t) (sum / measurement->selected) : 0;
  /* Ratios are binary32; comparisons are against the pinned DOUBLE constants. */
  measurement->eligible =
    (double) float32_divide ((float) measurement->nonzero, interior) <= 0.95 ||
    (double) float32_divide ((float) measurement->selected, interior) > 0.4;
}

static void
adjust (GxExposureState *state, bool increase)
{
  uint16_t distance = increase ? (uint16_t) (state->mean - state->inner_high) :
                                (uint16_t) (state->inner_low - state->mean);
  unsigned int delta, next;

  if (distance > 300)
    distance = 300;
  delta = (unsigned int) float32_divide ((float) distance, state->slope);
  if (!delta)
    delta = 1;

  /* These globals are WORD counters despite being spaced four bytes apart. */
  state->steps = (uint16_t) (state->steps + 1u);
  if (increase)
    {
      next = state->dac + delta;
      state->dac = (uint16_t) (next < state->upper ? next : state->upper);
    }
  else
    state->dac = state->dac >= state->lower + delta ?
                 (uint16_t) (state->dac - delta) : state->lower;
}

static void
feedback (GxExposureState *state)
{
  if (state->mean > state->outer_high)
    {
      if (state->low_count < 3)
        state->low_count = 0;
      else
        state->adjusting = 0;
      if (state->high_count < 3)
        state->high_count++;
      if (state->high_count < 3)
        return;
      state->low_count = 0;
      if (!state->adjusting)
        {
          state->adjusting = 1;
          state->steps = 0;
        }
      adjust (state, true);
    }
  else if (state->mean < state->outer_low)
    {
      if (state->high_count < 3)
        state->high_count = 0;
      else
        state->adjusting = 0;
      if (state->low_count < 3)
        state->low_count++;
      if (state->low_count < 3)
        return;
      state->high_count = 0;
      if (!state->adjusting)
        {
          state->adjusting = 1;
          state->steps = 0;
        }
      adjust (state, false);
    }
  else if (state->adjusting == 1)
    {
      if (state->mean >= state->inner_low && state->mean <= state->inner_high)
        {
          state->adjusting = 0;
          state->high_count = 0;
          state->low_count = 0;
        }
      else
        adjust (state, state->mean > state->inner_high);
    }
  else
    {
      if (state->high_count < 3)
        state->high_count = 0;
      if (state->low_count < 3)
        state->low_count = 0;
    }
}

/* Integer address differences avoid undefined ordering/subtraction of pointers
 * to unrelated objects, and avoid overflowing either region's end address.
 */
static bool
overlap (const void *first, size_t first_size,
         const void *second, size_t second_size)
{
  uintptr_t a = (uintptr_t) first, b = (uintptr_t) second;

  return a <= b ? b - a < first_size : a - b < second_size;
}

static bool
outputs_overlap (const GxExposureState *state, const uint16_t *background,
                 const uint16_t *frame, const uint16_t *dacs,
                 const GxExposureMeasurement *measurement)
{
  const void *outputs[] = { state, dacs, measurement };
  const size_t sizes[] = { sizeof *state, 4 * sizeof *dacs, sizeof *measurement };
  const size_t plane_size = GX_EXPOSURE_PIXELS * sizeof *frame;

  for (size_t i = 0; i < 3; i++)
    {
      if (overlap (outputs[i], sizes[i], background, plane_size) ||
          overlap (outputs[i], sizes[i], frame, plane_size))
        return true;
      for (size_t j = i + 1; j < 3; j++)
        if (overlap (outputs[i], sizes[i], outputs[j], sizes[j]))
          return true;
    }
  return false;
}

GxExposureResult
gx_exposure_update (GxExposureState *state, const uint16_t *background,
                    size_t background_count, const uint16_t *frame,
                    size_t frame_count, uint16_t dacs[4],
                    GxExposureMeasurement *measurement)
{
  GxExposureState next;
  GxExposureMeasurement measured = {0};
  uint16_t next_dacs[4];

  if (!state || !background || !frame || !dacs || !measurement ||
      background_count != GX_EXPOSURE_PIXELS || frame_count != GX_EXPOSURE_PIXELS ||
      outputs_overlap (state, background, frame, dacs, measurement) ||
      !valid_words (dacs))
    return GX_EXPOSURE_INVALID;
  if (!valid_state (state))
    return GX_EXPOSURE_STATE;
  for (size_t i = 0; i < GX_EXPOSURE_PIXELS; i++)
    if (background[i] > 4095 || frame[i] > 4095)
      return GX_EXPOSURE_INVALID;

  next = *state;
  memcpy (next_dacs, dacs, sizeof next_dacs);
  measure (background, frame, &next, &measured);
  if (measured.eligible)
    feedback (&next);
  next.dac &= 511;
  /* Always distribute, even if measurement was ineligible or DAC unchanged. */
  gx_exposure_shift (next_dacs, next.dac);
  *state = next;
  memcpy (dacs, next_dacs, sizeof next_dacs);
  *measurement = measured;
  return GX_EXPOSURE_OK;
}
