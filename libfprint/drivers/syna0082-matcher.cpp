/*
 * Independent small-area matcher for Synaptics/PQI 06cb:0082.
 *
 * SIFT feature extraction is provided by the vendored ezSIFT library
 * (Apache-2.0); the surrounding pipeline (upscale, CLAHE enhancement, kNN
 * matching, geometric verification) is implemented here.  The blob format
 * matches the previous OpenCV-based implementation.
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "syna0082-matcher.h"

#include "ezsift.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

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

constexpr guint enlarged_width = native_width * upscale;
constexpr guint enlarged_height = native_height * upscale;

struct FeaturePoint
{
  float x;
  float y;
};

struct FeatureSet
{
  std::vector<FeaturePoint> points;
  std::vector<float> descriptors;
};

struct MatchPair
{
  guint reference_index;
  guint probe_index;
};

/* One-dimensional cubic kernel (OpenCV's interpolateCubic, a = -0.75). */
static float
cubic_kernel (float           x,
              float           a)
{
  if (x < 0.0f)
    x = -x;
  if (x < 1.0f)
    return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
  if (x < 2.0f)
    return (((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a);
  return 0.0f;
}

static float
cubic_interpolate (float p0, float p1, float p2, float p3, float t)
{
  const float a = -0.75f;

  return cubic_kernel (1.0f + t, a) * p0 +
         cubic_kernel (t, a) * p1 +
         cubic_kernel (1.0f - t, a) * p2 +
         cubic_kernel (2.0f - t, a) * p3;
}

/* 4x upscale with separable cubic (Catmull-Rom, a=-0.75) interpolation,
 * matching OpenCV's cv::resize(..., cv::INTER_CUBIC) behaviour. */
static void
resize_cubic_4x (const guint8        *source,
                 std::vector<float>  *horizontal,
                 std::vector<float>  *output)
{
  const guint width = native_width;
  const guint height = native_height;
  const guint out_width = width * upscale;
  const guint out_height = height * upscale;

  horizontal->resize (width * out_height);
  output->resize (out_width * out_height);

  /* Horizontal pass on rows of the enlarged image. */
  for (guint r = 0; r < out_height; r++)
    {
      const float y = r / static_cast<float> (upscale);
      const guint ji = static_cast<guint> (y);
      const guint j0 = ji > 0 ? ji - 1 : 0;
      const guint j1 = ji;
      const guint j2 = std::min (j1 + 1, height - 1);
      const guint j3 = std::min (j1 + 2, height - 1);
      const float t = y - static_cast<float> (j1);

      for (guint c = 0; c < width; c++)
        {
          const float v = cubic_interpolate (source[j0 * width + c],
                                             source[j1 * width + c],
                                             source[j2 * width + c],
                                             source[j3 * width + c],
                                             t);
          (*horizontal)[r * width + c] = v;
        }
    }

  /* Vertical pass across columns of the enlarged image. */
  for (guint r = 0; r < out_height; r++)
    for (guint c = 0; c < out_width; c++)
      {
        const float x = c / static_cast<float> (upscale);
        const guint ii = static_cast<guint> (x);
        const guint i0 = ii > 0 ? ii - 1 : 0;
        const guint i1 = ii;
        const guint i2 = std::min (i1 + 1, width - 1);
        const guint i3 = std::min (i1 + 2, width - 1);
        const float t = x - static_cast<float> (i1);

        (*output)[r * out_width + c] =
          cubic_interpolate ((*horizontal)[r * width + i0],
                             (*horizontal)[r * width + i1],
                             (*horizontal)[r * width + i2],
                             (*horizontal)[r * width + i3],
                             t);
      }
}

/* Contrast-limited adaptive histogram equalization with a 4x8 tile grid and
 * a clip limit of 2.0, matching cv::createCLAHE (2.0, Size (4, 8)). */
static void
clahe (const std::vector<float> &input,
       std::vector<guint8>      *output)
{
  constexpr guint grid_x = 4;
  constexpr guint grid_y = 8;
  constexpr guint bins = 256;
  const guint tile_x = enlarged_width / grid_x;
  const guint tile_y = enlarged_height / grid_y;
  const float clip_limit = 2.0f;
  const float clip_value = clip_limit * (tile_x * tile_y) / bins;
  std::array<std::array<guint8, bins>, grid_x * grid_y> luts {};
  std::array<guint, bins> hist {};

  output->resize (enlarged_width * enlarged_height);

  for (guint gy = 0; gy < grid_y; gy++)
    for (guint gx = 0; gx < grid_x; gx++)
      {
        hist.fill (0);
        for (guint y = gy * tile_y; y < (gy + 1) * tile_y; y++)
          for (guint x = gx * tile_x; x < (gx + 1) * tile_x; x++)
            {
              guint value = static_cast<guint> (input[y * enlarged_width + x]);
              hist[std::min (value, bins - 1)]++;
            }

        guint excess = 0;
        for (guint b = 0; b < bins; b++)
          if (hist[b] > clip_value)
            {
              excess += hist[b] - static_cast<guint> (clip_value);
              hist[b] = static_cast<guint> (clip_value);
            }

        /* Redistribute the clipped excess evenly across all bins. */
        const guint per_bin = excess / bins;
        guint remainder = excess % bins;
        for (guint b = 0; b < bins; b++)
          {
            hist[b] += per_bin;
            if (remainder > 0)
              {
                hist[b]++;
                remainder--;
              }
          }

        /* Cumulative distribution -> per-tile LUT. */
        guint sum = 0;
        for (guint b = 0; b < bins; b++)
          {
            sum += hist[b];
            luts[gy * grid_x + gx][b] =
              static_cast<guint8> ((sum * (bins - 1)) / (tile_x * tile_y));
          }
      }

  /* Bilinear interpolation of LUTs across tile boundaries. */
  for (guint y = 0; y < enlarged_height; y++)
    for (guint x = 0; x < enlarged_width; x++)
      {
        const guint gx = std::min (x / tile_x, grid_x - 1);
        const guint gy = std::min (y / tile_y, grid_y - 1);
        const float fx = (x - gx * tile_x) / static_cast<float> (tile_x);
        const float fy = (y - gy * tile_y) / static_cast<float> (tile_y);

        const auto &lut00 = luts[gy * grid_x + gx];
        const auto &lut10 = luts[gy * grid_x + std::min (gx + 1, grid_x - 1)];
        const auto &lut01 = luts[std::min (gy + 1, grid_y - 1) * grid_x + gx];
        const auto &lut11 =
          luts[std::min (gy + 1, grid_y - 1) * grid_x +
               std::min (gx + 1, grid_x - 1)];

        const guint value = std::min (
          static_cast<guint> (input[y * enlarged_width + x]), bins - 1);
        const float top = lut00[value] + fx * (lut10[value] - lut00[value]);
        const float bottom =
          lut01[value] + fx * (lut11[value] - lut01[value]);

        (*output)[y * enlarged_width + x] =
          static_cast<guint8> (top + fy * (bottom - top));
      }
}

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
  gsize expected_length;
  guint32 count;
  const gsize row_size = (2 + descriptor_columns) * sizeof (guint32);

  if (data == nullptr || length < feature_header_size ||
      std::memcmp (data, feature_magic, sizeof (feature_magic) - 1) != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Feature header is invalid (length=%" G_GSIZE_FORMAT ")",
                   length);
      return FALSE;
    }
  offset = sizeof (feature_magic) - 1;
  if (!read_u32 (data, length, &offset, &count))
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_DATA,
                           "Feature count is missing");
      return FALSE;
    }
  if (count < minimum_features || count > maximum_features)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Feature count is invalid (count=%u)",
                   count);
      return FALSE;
    }
  expected_length = feature_header_size + static_cast<gsize> (count) * row_size;
  if (length != expected_length)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Feature length is invalid (length=%" G_GSIZE_FORMAT
                   ", expected=%" G_GSIZE_FORMAT ")",
                   length,
                   expected_length);
      return FALSE;
    }

  features->points.reserve (count);
  features->descriptors.resize (static_cast<gsize> (count) * descriptor_columns);
  for (guint row = 0; row < count; row++)
    {
      float x;
      float y;

      if (!read_float (data, length, &offset, &x) ||
          !read_float (data, length, &offset, &y) ||
          !std::isfinite (x) || !std::isfinite (y))
        {
          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_INVALID_DATA,
                       "Feature point is invalid (row=%u)",
                       row);
          return FALSE;
        }
      features->points.push_back ({x, y});
      for (guint column = 0; column < descriptor_columns; column++)
        {
          float value;

          if (!read_float (data, length, &offset, &value) ||
              !std::isfinite (value))
            {
              g_set_error (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_DATA,
                           "Feature descriptor is invalid (row=%u, column=%u)",
                           row,
                           column);
              return FALSE;
            }
          features->descriptors[static_cast<gsize> (row) * descriptor_columns +
                                column] = value;
        }
    }
  return TRUE;
}

static gboolean
extract_features (const guint8 *pixels,
                  FeatureSet   *features,
                  GError      **error)
{
  std::vector<float> enlarged;
  std::vector<float> horizontal;
  std::vector<guint8> normalized;
  ezsift::Image<unsigned char> sift_input (enlarged_width, enlarged_height);
  std::list<ezsift::SiftKeypoint> keypoints;

  resize_cubic_4x (pixels, &horizontal, &enlarged);
  clahe (enlarged, &normalized);

  /* Resolution-independent first octave doubling matches OpenCV's SIFT
   * defaults; contrast threshold 8.0 and edge threshold 15.0 mirror the
   * previous cv::SIFT configuration on the CLAHE-normalized raster. */
  ezsift::init_sift_parameters (true, 8.0f, 15.0f, 0.73f);
  for (gsize i = 0; i < normalized.size (); i++)
    sift_input.data[i] = normalized[i];
  ezsift::sift_cpu (sift_input, keypoints, true);

  guint count = 0;
  features->points.reserve (keypoints.size ());
  features->descriptors.reserve (keypoints.size () * descriptor_columns);
  for (const auto &keypoint : keypoints)
    {
      if (count >= maximum_features)
        break;
      features->points.push_back ({keypoint.c, keypoint.r});
      features->descriptors.insert (features->descriptors.end (),
                                    keypoint.descriptors,
                                    keypoint.descriptors +
                                      descriptor_columns);
      count++;
    }

  if (count < minimum_features)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Fingerprint contains too few stable features");
      return FALSE;
    }
  return TRUE;
}

static double
descriptor_distance (const FeatureSet &reference,
                     guint             reference_index,
                     const FeatureSet &probe,
                     guint             probe_index)
{
  double sum = 0.0;
  const float *a =
    &reference.descriptors[static_cast<gsize> (reference_index) *
                           descriptor_columns];
  const float *b = &probe.descriptors[static_cast<gsize> (probe_index) *
                                      descriptor_columns];

  for (guint column = 0; column < descriptor_columns; column++)
    {
      const double d = a[column] - b[column];
      sum += d * d;
    }
  return std::sqrt (sum);
}

/* Brute-force kNN (k=2) with a Lowe-ratio gate, matching
 * cv::BFMatcher (L2) + the 0.72 ratio threshold. */
static void
match_pairs (const FeatureSet       &reference,
             const FeatureSet       &probe,
             std::vector<MatchPair> &matches)
{
  for (guint query = 0; query < reference.points.size (); query++)
    {
      double best = std::numeric_limits<double>::infinity ();
      double second = std::numeric_limits<double>::infinity ();
      guint best_index = 0;

      for (guint candidate = 0; candidate < probe.points.size (); candidate++)
        {
          const double distance =
            descriptor_distance (reference, query, probe, candidate);
          if (distance < best)
            {
              second = best;
              best = distance;
              best_index = candidate;
            }
          else if (distance < second)
            {
              second = distance;
            }
        }

      if (best < ratio_threshold * second)
        matches.push_back ({query, best_index});
    }
}

static const guint32 ransac_max_iterations = 3000;
static const double ransac_confidence = 0.997;

/* Compute the similarity transform (rotation + scale + translation) that
 * maps reference point a onto probe point A and reference point b onto probe
 * point B, returned as the 2x3 matrix [a -b tx; b a ty] coefficients
 * (scale*s, scale*c, tx, ty). */
static gboolean
similarity_from_pair (const FeaturePoint &reference_a,
                      const FeaturePoint &probe_a,
                      const FeaturePoint &reference_b,
                      const FeaturePoint &probe_b,
                      float              *sa, float *sb, float *tx, float *ty)
{
  const float dx = reference_b.x - reference_a.x;
  const float dy = reference_b.y - reference_a.y;
  const float dxp = probe_b.x - probe_a.x;
  const float dyp = probe_b.y - probe_a.y;
  const float denom = dx * dx + dy * dy;

  if (denom < 1e-9f)
    return FALSE;

  *sa = (dxp * dx + dyp * dy) / denom;
  *sb = (dyp * dx - dxp * dy) / denom;
  *tx = probe_a.x - (*sa * reference_a.x - *sb * reference_a.y);
  *ty = probe_a.y - (*sb * reference_a.x + *sa * reference_a.y);
  return TRUE;
}

static float
similarity_error (const FeaturePoint &reference,
                  const FeaturePoint &probe,
                  float               sa, float sb, float tx, float ty)
{
  const float ex = probe.x - (sa * reference.x - sb * reference.y + tx);
  const float ey = probe.y - (sb * reference.x + sa * reference.y + ty);
  return std::hypot (ex, ey);
}

/* RANSAC geometric verification of the candidate matches: fit a similarity
 * transform from two point pairs, count inliers within the threshold, and
 * keep the highest-consensus model.  Mirrors cv::estimateAffinePartial2D
 * (cv::RANSAC, threshold 8.0, confidence 0.997). */
static gboolean
ransac_similarity (const std::vector<FeaturePoint> &reference_points,
                   const std::vector<FeaturePoint> &probe_points,
                   std::vector<gboolean>           *inlier_mask,
                   float                           *sa, float *sb,
                   float                           *tx, float *ty,
                   guint                           *inlier_count)
{
  const guint count = reference_points.size ();
  std::vector<gboolean> best_mask (count, FALSE);
  std::vector<gboolean> current_mask (count, FALSE);
  float best_sa = 1.0f, best_sb = 0.0f, best_tx = 0.0f, best_ty = 0.0f;
  guint best_inliers = 0;

  /* Simple deterministic pseudo-randomness; no shuffling of the source. */
  guint32 state = 0x9e3779b9U;

  auto next_random = [&state] ()
  {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };

  for (guint iteration = 0; iteration < ransac_max_iterations; iteration++)
    {
      guint i = next_random () % count;
      guint j = next_random () % count;

      if (i == j)
        continue;

      float candidate_sa, candidate_sb, candidate_tx, candidate_ty;
      if (!similarity_from_pair (reference_points[i], probe_points[i],
                                 reference_points[j], probe_points[j],
                                 &candidate_sa, &candidate_sb,
                                 &candidate_tx, &candidate_ty))
        continue;

      guint inliers = 0;
      current_mask.assign (count, FALSE);
      for (guint k = 0; k < count; k++)
        if (similarity_error (reference_points[k], probe_points[k],
                              candidate_sa, candidate_sb,
                              candidate_tx, candidate_ty) < ransac_threshold)
          {
            inliers++;
            current_mask[k] = TRUE;
          }

      if (inliers > best_inliers)
        {
          best_inliers = inliers;
          best_mask = current_mask;
          best_sa = candidate_sa;
          best_sb = candidate_sb;
          best_tx = candidate_tx;
          best_ty = candidate_ty;
        }

      /* Early exit when the inlier ratio is high enough that the outcome is
       * statistically certain within the requested confidence. */
      const double inlier_ratio =
        static_cast<double> (best_inliers) / count;
      if (inlier_ratio > 0.0)
        {
          const double needed =
            std::log (1.0 - ransac_confidence) /
            std::log (1.0 - inlier_ratio * inlier_ratio);
          if (iteration + 1 >= needed &&
              best_inliers >= minimum_inliers)
            break;
        }
    }

  if (best_inliers < minimum_inliers)
    return FALSE;

  for (guint k = 0; k < count; k++)
    if (best_mask[k])
      {
        (*inlier_mask)[k] = TRUE;
        (*inlier_count)++;
      }
  *sa = best_sa;
  *sb = best_sb;
  *tx = best_tx;
  *ty = best_ty;
  return TRUE;
}

static void
encode_features (const FeatureSet &features,
                 std::vector<guint8> *encoded)
{
  (*encoded).reserve (feature_header_size +
                      features.points.size () *
                        (2 + descriptor_columns) * sizeof (guint32));
  encoded->insert (encoded->end (),
                   feature_magic,
                   feature_magic + sizeof (feature_magic) - 1);
  append_u32 (*encoded, features.points.size ());
  for (gsize row = 0; row < features.points.size (); row++)
    {
      append_float (*encoded, features.points[row].x);
      append_float (*encoded, features.points[row].y);
      for (guint column = 0; column < descriptor_columns; column++)
        append_float (*encoded,
                      features.descriptors[static_cast<gsize> (row) *
                                             descriptor_columns +
                                           column]);
    }
}
} // namespace

GBytes *
syna0082_matcher_extract (const guint8 *pixels,
                          guint         width,
                          guint         height,
                          GError      **error)
{
  std::vector<guint8> encoded;
  FeatureSet features;

  if (pixels == nullptr || width != native_width || height != native_height)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_ARGUMENT,
                           "Matcher requires a 56x144 grayscale raster");
      return nullptr;
    }

  if (!extract_features (pixels, &features, error))
    return nullptr;
  encode_features (features, &encoded);
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
  std::vector<MatchPair> matches;
  std::vector<FeaturePoint> reference_points;
  std::vector<FeaturePoint> probe_points;
  std::vector<gboolean> inlier_mask;
  guint inlier_count = 0;
  float sa = 1.0f, sb = 0.0f, tx = 0.0f, ty = 0.0f;
  std::vector<FeaturePoint> inlier_points;

  g_return_val_if_fail (score != nullptr, FALSE);
  *score = {};

  if (!decode_features (reference,
                        reference_length,
                        &reference_features,
                        error))
    {
      if (error != nullptr && *error != nullptr)
        g_prefix_error (error, "Invalid 06cb:0082 reference: ");
      return FALSE;
    }
  if (!decode_features (probe, probe_length, &probe_features, error))
    {
      if (error != nullptr && *error != nullptr)
        g_prefix_error (error, "Invalid 06cb:0082 probe: ");
      return FALSE;
    }

  match_pairs (reference_features, probe_features, matches);
  score->matches = matches.size ();
  if (matches.size () < 3)
    return TRUE;

  reference_points.reserve (matches.size ());
  probe_points.reserve (matches.size ());
  for (const auto &match : matches)
    {
      reference_points.push_back (reference_features.points[match.reference_index]);
      probe_points.push_back (probe_features.points[match.probe_index]);
    }

  inlier_mask.assign (matches.size (), FALSE);
  if (!ransac_similarity (reference_points, probe_points, &inlier_mask,
                          &sa, &sb, &tx, &ty, &inlier_count))
    return TRUE;

  inlier_points.reserve (inlier_count);
  for (gsize index = 0; index < matches.size (); index++)
    if (inlier_mask[index])
      inlier_points.push_back (reference_points[index]);

  score->inliers = inlier_count;
  score->inlier_ratio = static_cast<double> (inlier_count) / matches.size ();
  score->scale = std::hypot (sa, sb);
  score->rotation = std::atan2 (sb, sa) * 180.0 / G_PI;
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
