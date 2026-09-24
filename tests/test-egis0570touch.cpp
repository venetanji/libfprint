/* Tests of storage and failure handling, not biometric accuracy. LGPL-2.1-or-later. */
#include "drivers/egis0570touch-match.h"
#include <vector>
#include <cstring>
#include <limits>

static void empty_input ()
{
  std::vector<uint16_t> blank (EGIS0570TOUCH_WIDTH * EGIS0570TOUCH_HEIGHT, 100);
  g_assert_null (fpi_egis0570touch_features_extract (nullptr, 1, blank.data ()));
  g_assert_null (fpi_egis0570touch_features_extract (blank.data (), 0, blank.data ()));
  g_assert_null (fpi_egis0570touch_features_extract (blank.data (), 1, blank.data ()));
  g_assert_cmpuint (fpi_egis0570touch_features_compare (nullptr, nullptr), ==, 0);
  g_assert_null (fpi_egis0570touch_features_deserialise (nullptr));
}

static void storage_roundtrip ()
{
  std::vector<uint16_t> frame (EGIS0570TOUCH_WIDTH * EGIS0570TOUCH_HEIGHT);
  std::vector<uint16_t> background (frame.size (), 0);
  guint32 state = 123456789;
  for (auto &pixel : frame)
    {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      pixel = state & 255;
    }
  g_autoptr(Egis0570TouchFeatures) original =
    fpi_egis0570touch_features_extract (frame.data (), 1, background.data ());
  g_assert_nonnull (original);
  g_autoptr(GBytes) blob = fpi_egis0570touch_features_serialise (original);
  g_assert_nonnull (blob);
  g_autoptr(Egis0570TouchFeatures) restored = fpi_egis0570touch_features_deserialise (blob);
  g_assert_nonnull (restored);
  g_assert_cmpuint (fpi_egis0570touch_features_keypoint_count (restored), ==,
                   fpi_egis0570touch_features_keypoint_count (original));
  guint before = fpi_egis0570touch_features_compare (original, original);
  guint after = fpi_egis0570touch_features_compare (original, restored);
  g_assert_cmpuint (before, >, 0);
  g_assert_cmpuint (before, ==, after);

  gsize size;
  const auto *bytes = static_cast<const guint8 *> (g_bytes_get_data (blob, &size));
  g_assert_cmphex (bytes[0], ==, 0x31);
  g_assert_cmphex (bytes[1], ==, 0x30);
  g_assert_cmphex (bytes[2], ==, 0x54);
  g_assert_cmphex (bytes[3], ==, 0x45);
  g_assert_cmphex (bytes[4], ==, 0x01);
  g_assert_cmphex (bytes[5], ==, 0x00);
  for (gsize len : { gsize (0), gsize (7), size - 1 })
    {
      g_autoptr(GBytes) truncated = g_bytes_new (bytes, len);
      g_assert_null (fpi_egis0570touch_features_deserialise (truncated));
    }
  std::vector<guint8> damaged (bytes, bytes + size);
  damaged[0] ^= 1;
  g_autoptr(GBytes) wrong_magic = g_bytes_new (damaged.data (), damaged.size ());
  g_assert_null (fpi_egis0570touch_features_deserialise (wrong_magic));
  damaged.assign (bytes, bytes + size);
  float invalid = std::numeric_limits<float>::quiet_NaN ();
  memcpy (damaged.data () + 8, &invalid, sizeof (invalid));
  g_autoptr(GBytes) bad_coordinate = g_bytes_new (damaged.data (), damaged.size ());
  g_assert_null (fpi_egis0570touch_features_deserialise (bad_coordinate));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/egis0570touch/empty-input", empty_input);
  g_test_add_func ("/egis0570touch/storage-roundtrip", storage_roundtrip);
  return g_test_run ();
}
