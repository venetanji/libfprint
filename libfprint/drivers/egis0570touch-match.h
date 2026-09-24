/* EXPERIMENTAL: adapted from smox/libfprint-elan-04f3-0c63, commit
 * 0f18837da8d691946673710d63a81b39d0782fca. Geometry adapted for Egis 0570.
 * The original ELAN measurements below do NOT validate these parameters
 * for Egis. Authentication thresholds require independent evaluation.
 */
/*
 * Descriptor matching for the ELAN 04f3:0c63 - C interface
 * Copyright (C) 2026 Michael Brunner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Geometry and crop used by the Egis capture path. */
#define EGIS0570TOUCH_WIDTH 114
#define EGIS0570TOUCH_HEIGHT 57
/* Ignore edge pixels that are dominated by the sensor border. */
#define EGIS0570TOUCH_BORDER 3

/* Opaque handle holding the keypoints and descriptors of one capture. */
typedef struct _Egis0570TouchFeatures Egis0570TouchFeatures;

/**
 * fpi_egis0570touch_features_extract:
 * @frames: @frame_count raw frames, each EGIS0570TOUCH_WIDTH * EGIS0570TOUCH_HEIGHT
 *   uint16 samples in row order
 * @frame_count: number of frames captured for this press
 * @background: the empty-sensor reference of the same session
 *
 * Picks the frame yielding the most keypoints and extracts its features.
 *
 * Returns: (transfer full) (nullable): the features, or %NULL on failure
 */
Egis0570TouchFeatures *fpi_egis0570touch_features_extract (const uint16_t *frames,
                                                           guint           frame_count,
                                                           const uint16_t *background);

/**
 * fpi_egis0570touch_features_keypoint_count:
 *
 * Returns: how many keypoints the capture yielded
 */
guint fpi_egis0570touch_features_keypoint_count (Egis0570TouchFeatures *features);

/**
 * fpi_egis0570touch_features_serialise:
 *
 * Returns: (transfer full): a self-describing byte blob for storage
 */
GBytes *fpi_egis0570touch_features_serialise (Egis0570TouchFeatures *features);

/**
 * fpi_egis0570touch_features_deserialise:
 *
 * Returns: (transfer full) (nullable): the features, or %NULL if the blob is
 *   malformed or of an unknown version
 */
Egis0570TouchFeatures *fpi_egis0570touch_features_deserialise (GBytes *blob);

/**
 * fpi_egis0570touch_features_compare:
 *
 * Matches @probe against @gallery: nearest-neighbour descriptor matching, a
 * Lowe ratio test, then a RANSAC similarity fit over the surviving pairs.
 *
 * Returns: the number of geometrically consistent correspondences
 */
guint fpi_egis0570touch_features_compare (Egis0570TouchFeatures *probe,
                                          Egis0570TouchFeatures *gallery);

void fpi_egis0570touch_features_free (Egis0570TouchFeatures *features);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Egis0570TouchFeatures, fpi_egis0570touch_features_free)

G_END_DECLS
