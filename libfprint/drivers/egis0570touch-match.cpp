/*
 * Descriptor matching for the ELAN 04f3:0c63
 * Copyright (C) 2026 Michael Brunner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Adaptation note: the original ELAN measurements do not establish accuracy
 * for this Egis sensor. All preprocessing constants remain experimental.
 */

#include "egis0570touch-match.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <exception>
#include <memory>
#include <vector>

namespace {

/* Starting parameters inherited from the ELAN reference implementation.
 * They have not been validated for Egis authentication accuracy. */
constexpr double kClaheClip = 3.0;
constexpr int kClaheGrid = 8;

/* Enlarge the cropped 108x51 image before extracting SIFT descriptors. */
constexpr int kUpscale = 2;
constexpr double kSiftContrast = 0.04;
constexpr double kSiftEdge = 12.0;
constexpr double kSiftSigma = 1.2;
constexpr int kSiftOctaveLayers = 3;
constexpr float kRatio = 0.70f;

/* Check descriptor correspondences against a similarity transform. */
constexpr double kRansacReproj = 3.0;
constexpr size_t kRansacMinMatches = 4;

constexpr uint32_t kBlobMagic = 0x45543031; /* Egis experimental touch v1 */
constexpr uint16_t kBlobVersion = 1;

constexpr int kWidth = EGIS0570TOUCH_WIDTH - 2 * EGIS0570TOUCH_BORDER;
constexpr int kHeight = EGIS0570TOUCH_HEIGHT - 2 * EGIS0570TOUCH_BORDER;
constexpr size_t kFramePixels =
  static_cast<size_t> (EGIS0570TOUCH_WIDTH) * EGIS0570TOUCH_HEIGHT;

void
write_u16_le (uint8_t *buffer, uint16_t value)
{
  value = GUINT16_TO_LE (value);
  memcpy (buffer, &value, sizeof (value));
}

void
write_u32_le (uint8_t *buffer, uint32_t value)
{
  value = GUINT32_TO_LE (value);
  memcpy (buffer, &value, sizeof (value));
}

uint16_t
read_u16_le (const uint8_t *buffer)
{
  uint16_t value;
  memcpy (&value, buffer, sizeof (value));
  return GUINT16_FROM_LE (value);
}

uint32_t
read_u32_le (const uint8_t *buffer)
{
  uint32_t value;
  memcpy (&value, buffer, sizeof (value));
  return GUINT32_FROM_LE (value);
}

void
write_float_le (uint8_t *buffer, float value)
{
  static_assert (sizeof (float) == sizeof (uint32_t));
  uint32_t bits;
  memcpy (&bits, &value, sizeof (bits));
  write_u32_le (buffer, bits);
}

float
read_float_le (const uint8_t *buffer)
{
  const uint32_t bits = read_u32_le (buffer);
  float value;
  memcpy (&value, &bits, sizeof (value));
  return value;
}

/* Linear interpolated percentile, matching numpy's default so the C++ and the
 * Python reference implementation agree on the same input. */
double
percentile (std::vector<float> &sorted, double q)
{
  if (sorted.empty ())
    return 0.0;
  const double pos = q / 100.0 * static_cast<double> (sorted.size () - 1);
  const size_t lo = static_cast<size_t> (pos);
  const size_t hi = std::min (lo + 1, sorted.size () - 1);
  return sorted[lo] + (pos - static_cast<double> (lo)) * (sorted[hi] - sorted[lo]);
}

/* Background subtraction, border crop, normalisation, upscale, CLAHE. */
cv::Mat
prepare (const uint16_t *frame, const uint16_t *background)
{
  cv::Mat signal (kHeight, kWidth, CV_32F);
  std::vector<float> values;
  values.reserve (static_cast<size_t> (kHeight) * kWidth);

  for (int y = 0; y < kHeight; y++)
    {
      for (int x = 0; x < kWidth; x++)
        {
          const size_t idx =
            static_cast<size_t> (y + EGIS0570TOUCH_BORDER) * EGIS0570TOUCH_WIDTH +
            (x + EGIS0570TOUCH_BORDER);
          /* Kept signed: the difference is genuinely negative in the valleys,
           * and clamping it away costs contrast. */
          const float v = static_cast<float> (frame[idx]) -
                          static_cast<float> (background[idx]);
          signal.at<float> (y, x) = v;
          values.push_back (v);
        }
    }

  std::sort (values.begin (), values.end ());
  const double lo = percentile (values, 1.0);
  const double hi = percentile (values, 99.0);
  const double span = std::max (hi - lo, 1e-9);

  cv::Mat scaled (kHeight, kWidth, CV_8U);
  for (int y = 0; y < kHeight; y++)
    for (int x = 0; x < kWidth; x++)
      {
        double t = (signal.at<float> (y, x) - lo) / span;
        t = std::clamp (t, 0.0, 1.0);
        scaled.at<uint8_t> (y, x) = static_cast<uint8_t> (t * 255.0);
      }

  cv::Mat upscaled;
  cv::resize (scaled, upscaled, cv::Size (), kUpscale, kUpscale, cv::INTER_CUBIC);

  cv::Mat equalised;
  cv::createCLAHE (kClaheClip, cv::Size (kClaheGrid, kClaheGrid))
    ->apply (upscaled, equalised);

  return equalised;
}

} /* namespace */

struct _Egis0570TouchFeatures
{
  std::vector<cv::KeyPoint> keypoints;
  cv::Mat descriptors;   /* CV_32F, one row per keypoint */
};

Egis0570TouchFeatures *
fpi_egis0570touch_features_extract (const uint16_t *frames,
                                  guint           frame_count,
                                  const uint16_t *background) try
{
  if (frames == nullptr || background == nullptr || frame_count == 0)
    return nullptr;

  auto sift = cv::SIFT::create (0, kSiftOctaveLayers, kSiftContrast,
                                kSiftEdge, kSiftSigma);

  std::vector<cv::KeyPoint> best_keypoints;
  cv::Mat best_descriptors;

  /* Experimental selection heuristic: retain the most keypoints.
   * Keypoint count does not guarantee image quality or matching accuracy. */
  for (guint i = 0; i < frame_count; i++)
    {
      const cv::Mat image = prepare (frames + i * kFramePixels, background);

      std::vector<cv::KeyPoint> keypoints;
      cv::Mat descriptors;
      sift->detectAndCompute (image, cv::noArray (), keypoints, descriptors);

      if (keypoints.size () > best_keypoints.size ())
        {
          best_keypoints = std::move (keypoints);
          best_descriptors = descriptors.clone ();
        }
    }

  if (best_keypoints.empty ())
    return nullptr;

  auto features = std::make_unique<_Egis0570TouchFeatures> ();
  features->keypoints = std::move (best_keypoints);
  features->descriptors = std::move (best_descriptors);
  return features.release ();
}
catch (const std::exception &)
{
  /* No C++ exception may escape into the C driver; NULL requests a retry. */
  return nullptr;
}

guint
fpi_egis0570touch_features_keypoint_count (Egis0570TouchFeatures *features)
{
  return features ? static_cast<guint> (features->keypoints.size ()) : 0;
}

/* Blob layout, little endian:
 *   magic u32 | version u16 | count u16 | count * (x f32, y f32) | count * 128 u8
 *
 * Only the coordinates are kept from each keypoint; scale, angle and response
 * are not needed once the descriptor exists. Descriptors are stored as bytes:
 * SIFT descriptors are rounded and clamped to bytes. This is the existing
 * version-1 format; changing it would require template migration.
 */
GBytes *
fpi_egis0570touch_features_serialise (Egis0570TouchFeatures *features) try
{
  if (features == nullptr || features->keypoints.empty () ||
      features->descriptors.cols != 128)
    return nullptr;

  const uint16_t count = static_cast<uint16_t> (
    std::min<size_t> (features->keypoints.size (), 4096));

  const size_t size = 4 + 2 + 2 +
                      static_cast<size_t> (count) * 2 * sizeof (float) +
                      static_cast<size_t> (count) * 128;

  g_autofree uint8_t *buffer = static_cast<uint8_t *> (g_malloc (size));
  size_t offset = 0;

  write_u32_le (buffer + offset, kBlobMagic); offset += 4;
  write_u16_le (buffer + offset, kBlobVersion); offset += 2;
  write_u16_le (buffer + offset, count); offset += 2;

  for (uint16_t i = 0; i < count; i++)
    {
      write_float_le (buffer + offset, features->keypoints[i].pt.x);
      offset += sizeof (float);
      write_float_le (buffer + offset, features->keypoints[i].pt.y);
      offset += sizeof (float);
    }

  for (uint16_t i = 0; i < count; i++)
    {
      const float *row = features->descriptors.ptr<float> (i);
      for (int d = 0; d < 128; d++)
        buffer[offset + d] = static_cast<uint8_t> (
          std::clamp (static_cast<int> (row[d] + 0.5f), 0, 255));
      offset += 128;
    }

  return g_bytes_new_take (g_steal_pointer (&buffer), size);
}
catch (const std::exception &)
{
  return nullptr;
}

Egis0570TouchFeatures *
fpi_egis0570touch_features_deserialise (GBytes *blob) try
{
  if (blob == nullptr)
    return nullptr;
  gsize size = 0;
  const auto *buffer = static_cast<const uint8_t *> (g_bytes_get_data (blob, &size));

  if (buffer == nullptr || size < 8)
    return nullptr;

  const uint32_t magic = read_u32_le (buffer);
  const uint16_t version = read_u16_le (buffer + 4);
  const uint16_t count = read_u16_le (buffer + 6);

  if (magic != kBlobMagic || version != kBlobVersion || count == 0 || count > 4096)
    return nullptr;

  const size_t expected = 8 + static_cast<size_t> (count) * 2 * sizeof (float) +
                          static_cast<size_t> (count) * 128;
  if (size != expected)
    return nullptr;

  auto features = std::make_unique<_Egis0570TouchFeatures> ();
  features->keypoints.reserve (count);
  features->descriptors = cv::Mat (count, 128, CV_32F);

  size_t offset = 8;
  for (uint16_t i = 0; i < count; i++)
    {
      float xy[2];
      xy[0] = read_float_le (buffer + offset);
      offset += sizeof (float);
      xy[1] = read_float_le (buffer + offset);
      offset += sizeof (float);
      if (!std::isfinite (xy[0]) || !std::isfinite (xy[1]) ||
          xy[0] < 0 || xy[1] < 0 ||
          xy[0] >= kWidth * kUpscale || xy[1] >= kHeight * kUpscale)
        return nullptr;
      features->keypoints.emplace_back (cv::Point2f (xy[0], xy[1]), 1.0f);
    }

  for (uint16_t i = 0; i < count; i++)
    {
      float *row = features->descriptors.ptr<float> (i);
      for (int d = 0; d < 128; d++)
        row[d] = static_cast<float> (buffer[offset + d]);
      offset += 128;
    }

  return features.release ();
}
catch (const std::exception &)
{
  return nullptr;
}

guint
fpi_egis0570touch_features_compare (Egis0570TouchFeatures *probe,
                                  Egis0570TouchFeatures *gallery) try
{
  if (probe == nullptr || gallery == nullptr)
    return 0;
  if (probe->descriptors.rows < 2 || gallery->descriptors.rows < 2)
    return 0;

  std::vector<std::vector<cv::DMatch> > candidates;
  cv::BFMatcher (cv::NORM_L2).knnMatch (probe->descriptors,
                                        gallery->descriptors,
                                        candidates, 2);

  /* Ridge patterns are highly self-similar, so most nearest neighbours are
   * ambiguous. The ratio test discards those; what survives still has to pass
   * the geometric check below. */
  std::vector<cv::Point2f> source, target;
  for (const auto &pair : candidates)
    {
      if (pair.size () < 2)
        continue;
      if (pair[0].distance >= kRatio * pair[1].distance)
        continue;
      source.push_back (probe->keypoints[pair[0].queryIdx].pt);
      target.push_back (gallery->keypoints[pair[0].trainIdx].pt);
    }

  if (source.size () < kRansacMinMatches)
    return 0;

  /* Count only correspondences consistent with the geometric model. */
  cv::Mat inliers;
  cv::estimateAffinePartial2D (source, target, inliers, cv::RANSAC,
                               kRansacReproj, 5000, 0.99);

  if (inliers.empty ())
    return 0;

  return static_cast<guint> (cv::countNonZero (inliers));
}
catch (const std::exception &)
{
  /* A failed comparison must never authenticate. */
  return 0;
}

void
fpi_egis0570touch_features_free (Egis0570TouchFeatures *features)
{
  delete features;
}
