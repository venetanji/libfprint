/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "goodix-chicago-calibration.h"
G_BEGIN_DECLS
/* Fixed 80x64 mode17 capture mask. Offsets are prepared ImageBase+7095.
 * Returns the signed-16 threshold as its raw uint16 representation. */
guint16 goodix_gf5298_build_mask (const guint16 current[GOODIX_CHICAGO_PIXELS],
                                 const guint16 offset[GOODIX_CHICAGO_PIXELS],
                                 guint8 mask[GOODIX_CHICAGO_PIXELS]);
/* Fill bounded horizontal/vertical gaps, then remove invalid ADC pixels. */
void goodix_gf5298_fill_mask (const guint16 current[GOODIX_CHICAGO_PIXELS],
                             guint8 mask[GOODIX_CHICAGO_PIXELS]);
/* Signed mode17 correction before gain adaptation. */
void goodix_gf5298_corrected_signal (const guint16 current[GOODIX_CHICAGO_PIXELS],
                                    const guint16 offset[GOODIX_CHICAGO_PIXELS],
                                    guint16 signal[GOODIX_CHICAGO_PIXELS]);
/* Per-frame local gain using a separable median-of-three, not median-of-nine. */
void goodix_gf5298_local_gain (const guint16 signal[GOODIX_CHICAGO_PIXELS],
                              guint16 gain[GOODIX_CHICAGO_PIXELS]);
/* Fixed GF5298 reference800/low1800/high3600 policy; no fitted thresholds. */
gboolean goodix_gf5298_select_alternate (gint primary, gint alternate,
                                        gint correlation);
G_END_DECLS
