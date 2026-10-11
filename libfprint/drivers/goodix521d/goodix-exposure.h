// SPDX-License-Identifier: LGPL-2.1-or-later
/* Pure host-side GF5298 mode29/mode1 exposure feedback; no device I/O. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GX_EXPOSURE_WIDTH 80
#define GX_EXPOSURE_HEIGHT 64
#define GX_EXPOSURE_PIXELS (GX_EXPOSURE_WIDTH * GX_EXPOSURE_HEIGHT)

typedef enum {
  GX_EXPOSURE_OK,
  GX_EXPOSURE_INVALID,
  GX_EXPOSURE_STATE
} GxExposureResult;

typedef struct {
  uint16_t integration, initial_dac, dac, lower, upper;
  uint16_t outer_low, outer_high, inner_low, inner_high;
  uint16_t mean, adjusting, steps, high_count, low_count, initialized;
  float slope;
} GxExposureState;

typedef struct {
  uint32_t nonzero, selected;
  bool eligible;
} GxExposureMeasurement;

/* Accepted integration is 1..510; factory integration is even2..510 (2*OTP27).
 * Integration1 is supported for the arithmetic boundary oracle. Immutable
 * factory initial DAC is 0..511.
 * The Windows call's literal151 is logging metadata, not either parameter.
 * Reset starts a new hardware epoch: it clears ALL previous controller history.
 * Clear invalidates the state and also erases its history; NULL is harmless.
 */
GxExposureResult gx_exposure_reset (GxExposureState *state,
                                    unsigned int integration,
                                    unsigned int initial_dac);
void gx_exposure_clear (GxExposureState *state);

/* Only for history-preserving vendor reinitialization/oracle equivalence.
 * Requires valid existing state. Do NOT use this for a new hardware epoch.
 */
GxExposureResult gx_exposure_reinitialize (GxExposureState *state,
                                           unsigned int integration,
                                           unsigned int initial_dac);

/* Both input planes are native ADC uint16 values in 0..4095, already decoded
 * from the wire, with exactly 5120 samples: height64, width80, rowstride80.
 * Do not rotate/transpose, clamp, replicate edges or preprocess before update.
 * The one-pixel crop is used for the mean; cutoff uses the ENTIRE input plane.
 * dacs is the four current request words in 0..511. The resulting words apply
 * to a FUTURE capture/DOWN request, never retroactively to the measured image.
 * Even ineligible/no-change feedback redistributes words around the state DAC.
 * All pointers are required. State, DACs and measurement change together only
 * on success. Input planes are never modified and may be the same buffer.
 * Mutable state/DAC/measurement regions must be disjoint from each other and
 * both input planes; any overlap is rejected before reading sample values.
 */
GxExposureResult gx_exposure_update (GxExposureState *state,
                                     const uint16_t *background,
                                     size_t background_count,
                                     const uint16_t *frame,
                                     size_t frame_count,
                                     uint16_t dacs[4],
                                     GxExposureMeasurement *measurement);

/* Shift in place, anchored at the minimum of ALL four words, not word0.
 * Upward shifts wrap to nine bits; downward shifts use uint16 subtraction.
 * Invalid arguments leave all words untouched.
 */
GxExposureResult gx_exposure_shift (uint16_t dacs[4], unsigned int target);
