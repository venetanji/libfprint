/*
 * Independent small-area matcher for Synaptics/PQI 06cb:0082.
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "syna0082-matcher.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
constexpr guint native_width = 56;
constexpr guint native_height = 144;
constexpr guint upscale = 4;
constexpr guint descriptor_columns = 128;
constexpr guint maximum_features = 700;
constexpr guint minimum_features = 8;
constexpr double ratio_threshold = 0.72;
constexpr double ransac_threshold = 8.0;
constexpr guint minimum_inliers = 6;
constexpr double minimum_inlier_ratio = 0.65;
constexpr double minimum_coverage = 0.015;
constexpr char feature_magic[] = "SY82FT01";
constexpr gsize feature_header_size = 12;

struct FeatureSet
{
  std::vector<cv::Point2f> points;
  cv::Mat descriptors;
};

static void
append_u32 (std::vector<guint8> &output,
            guint32              value)
{
  const guint32 encoded = GUINT32_TO_LE (value);
  const auto *bytes = reinterpret_cast<const guint8 *> (&encoded);
  output.insert (output.end (), bytes, bytes + sizeof (encoded));
}

static void
append_float (std::vector<guint8> &output,
              float                value)
{
  guint32 bits;

  static_assert (sizeof (bits) == sizeof (value));
  std::memcpy (&bits, &value, sizeof (bits));
  append_u32 (output, bits);
}

static gboolean
read_u32 (const guint8 *data,
          gsize         length,
          gsize        *offset,
          guint32      *value)
{
  guint32 encoded;

  if (*offset > length || length - *offset < sizeof (encoded))
    return FALSE;
  std::memcpy (&encoded, data + *offset, sizeof (encoded));
  *offset += sizeof (encoded);
  *value = GUINT32_FROM_LE (encoded);
  return TRUE;
}

static gboolean
read_float (const guint8 *data,
            gsize         length,
            gsize        *offset,
            float        *value)
{
  guint32 bits;

  if (!read_u32 (data, length, offset, &bits))
    return FALSE;
  std::memcpy (value, &bits, sizeof (bits));
  return TRUE;
}

static gboolean
decode_features (const guint8 *data,
                 gsize         length,
                 FeatureSet   *features,
                 GError      **error)
{
  gsize offset = 0;
  guint32 count;
  const gsize row_size = (2 + descriptor_columns) * sizeof (guint32);

  if (length < feature_header_size ||
      std::memcmp (data, feature_magic, sizeof (feature_magic) - 1) != 0)
    goto invalid;
  offset = sizeof (feature_magic) - 1;
  if (!read_u32 (data, length, &offset, &count) ||
      count < minimum_features || count > maximum_features ||
      length - offset != static_cast<gsize> (count) * row_size)
    goto invalid;

  features->points.reserve (count);
  features->descriptors = cv::Mat (count, descriptor_columns, CV_32F);
  for (guint row = 0; row < count; row++)
    {
      float x;
      float y;

      if (!read_float (data, length, &offset, &x) ||
          !read_float (data, length, &offset, &y) ||
          !std::isfinite (x) || !std::isfinite (y))
        goto invalid;
      features->points.emplace_back (x, y);
      for (guint column = 0; column < descriptor_columns; column++)
        {
          float value;

          if (!read_float (data, length, &offset, &value) ||
              !std::isfinite (value))
            goto invalid;
          features->descriptors.at<float> (row, column) = value;
        }
    }
  return TRUE;

invalid:
  g_set_error_literal (error,
                       G_IO_ERROR,
                       G_IO_ERROR_INVALID_DATA,
                       "Invalid 06cb:0082 feature template");
  return FALSE;
}
}

GBytes *
syna0082_matcher_extract (const guint8 *pixels,
                          guint         width,
                          guint         height,
                          GError      **error)
{
  cv::Mat native;
  cv::Mat enlarged;
  cv::Mat normalized;
  std::vector<cv::KeyPoint> keypoints;
  cv::Mat descriptors;
  std::vector<guint8> encoded;

  if (pixels == nullptr || width != native_width || height != native_height)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_ARGUMENT,
                           "Matcher requires a 56x144 grayscale raster");
      return nullptr;
    }

  native = cv::Mat (height,
                    width,
                    CV_8UC1,
                    const_cast<guint8 *> (pixels));
  cv::resize (native,
              enlarged,
              cv::Size (),
              upscale,
              upscale,
              cv::INTER_CUBIC);
  cv::createCLAHE (2.0, cv::Size (4, 8))->apply (enlarged, normalized);
  cv::SIFT::create (maximum_features, 3, 0.01, 15, 1.6)
    ->detectAndCompute (normalized,
                        cv::noArray (),
                        keypoints,
                        descriptors);

  if (keypoints.size () < minimum_features || descriptors.empty ())
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Fingerprint contains too few stable features");
      return nullptr;
    }

  encoded.reserve (feature_header_size +
                   keypoints.size () *
                   (2 + descriptor_columns) * sizeof (guint32));
  encoded.insert (encoded.end (),
                  feature_magic,
                  feature_magic + sizeof (feature_magic) - 1);
  append_u32 (encoded, keypoints.size ());
  for (gsize row = 0; row < keypoints.size (); row++)
    {
      append_float (encoded, keypoints[row].pt.x);
      append_float (encoded, keypoints[row].pt.y);
      for (guint column = 0; column < descriptor_columns; column++)
        append_float (encoded, descriptors.at<float> (row, column));
    }

  return g_bytes_new_take (g_memdup2 (encoded.data (), encoded.size ()),
                           encoded.size ());
}

gboolean
syna0082_matcher_compare (const guint8       *reference,
                          gsize               reference_length,
                          const guint8       *probe,
                          gsize               probe_length,
                          Syna0082MatchScore *score,
                          GError             **error)
{
  FeatureSet reference_features;
  FeatureSet probe_features;
  std::vector<std::vector<cv::DMatch>> pairs;
  std::vector<cv::DMatch> matches;
  std::vector<cv::Point2f> reference_points;
  std::vector<cv::Point2f> probe_points;
  cv::Mat inlier_mask;
  cv::Mat transform;

  g_return_val_if_fail (score != nullptr, FALSE);
  *score = {};

  if (!decode_features (reference,
                        reference_length,
                        &reference_features,
                        error) ||
      !decode_features (probe, probe_length, &probe_features, error))
    return FALSE;

  cv::BFMatcher (cv::NORM_L2).knnMatch (reference_features.descriptors,
                                       probe_features.descriptors,
                                       pairs,
                                       2);
  for (const auto &pair : pairs)
    if (pair.size () == 2 &&
        pair[0].distance < ratio_threshold * pair[1].distance)
      matches.push_back (pair[0]);
  score->matches = matches.size ();
  if (matches.size () < 3)
    return TRUE;

  reference_points.reserve (matches.size ());
  probe_points.reserve (matches.size ());
  for (const auto &match : matches)
    {
      reference_points.push_back (reference_features.points[match.queryIdx]);
      probe_points.push_back (probe_features.points[match.trainIdx]);
    }

  transform = cv::estimateAffinePartial2D (reference_points,
                                           probe_points,
                                           inlier_mask,
                                           cv::RANSAC,
                                           ransac_threshold,
                                           3000,
                                           0.997,
                                           10);
  if (transform.empty () || inlier_mask.empty ())
    return TRUE;

  std::vector<cv::Point2f> inlier_points;
  for (gsize index = 0; index < matches.size (); index++)
    if (inlier_mask.at<guint8> (index) != 0)
      inlier_points.push_back (reference_points[index]);

  score->inliers = inlier_points.size ();
  score->inlier_ratio = static_cast<double> (score->inliers) / matches.size ();
  score->scale = std::hypot (transform.at<double> (0, 0),
                             transform.at<double> (1, 0));
  score->rotation = std::atan2 (transform.at<double> (1, 0),
                                transform.at<double> (0, 0)) * 180.0 / G_PI;
  if (inlier_points.size () >= 2)
    {
      float minimum_x = inlier_points[0].x;
      float maximum_x = inlier_points[0].x;
      float minimum_y = inlier_points[0].y;
      float maximum_y = inlier_points[0].y;

      for (const auto &point : inlier_points)
        {
          minimum_x = std::min (minimum_x, point.x);
          maximum_x = std::max (maximum_x, point.x);
          minimum_y = std::min (minimum_y, point.y);
          maximum_y = std::max (maximum_y, point.y);
        }
      score->coverage =
        ((maximum_x - minimum_x) * (maximum_y - minimum_y)) /
        ((native_width * upscale) * (native_height * upscale));
    }
  score->accepted = score->inliers >= minimum_inliers &&
                    score->inlier_ratio >= minimum_inlier_ratio &&
                    score->scale >= 0.70 && score->scale <= 1.35 &&
                    score->coverage >= minimum_coverage;
  return TRUE;
}
