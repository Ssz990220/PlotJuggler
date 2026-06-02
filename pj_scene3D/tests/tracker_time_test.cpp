// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/tracker_time.h"

#include <gtest/gtest.h>

namespace pj::scene3d {
namespace {

// Absolute ns timestamps mirroring the real recording from the bug report:
// a global "map" latched at an early stamp, a live playhead ~90s later.
constexpr int64_t kLatched = 1777642313165786980;  // map's single timestamp
constexpr int64_t kLivePlayhead = 1777642403234063104;
constexpr int64_t kLiveLast = 1777642431235149568;

TEST(ClampTrackerTime, EmptyPassesThrough) {
  EXPECT_EQ(clampTrackerTimeToRanges(42, {}), 42);
}

TEST(ClampTrackerTime, InvertedRangeIgnored) {
  // Only an inverted range → no usable bound → pass through.
  EXPECT_EQ(clampTrackerTimeToRanges(10, {{5, 1}}), 10);
}

TEST(ClampTrackerTime, SpanningClampsBothEnds) {
  const std::vector<EntityTimeRange> r{{100, 200}};
  EXPECT_EQ(clampTrackerTimeToRanges(50, r), 100);   // below → lo
  EXPECT_EQ(clampTrackerTimeToRanges(150, r), 150);  // inside → unchanged
  EXPECT_EQ(clampTrackerTimeToRanges(250, r), 200);  // above → hi
}

// The regression. A latched (zero-span) entity must NOT cap hi, so the live
// playhead is not dragged back to the latched stamp. Pre-fix this returned
// kLatched, leaving TF/live data to render at a stale time where nothing exists.
TEST(ClampTrackerTime, LatchedDoesNotDragLivePlayheadBack) {
  const std::vector<EntityTimeRange> r{{kLatched, kLatched}};
  EXPECT_EQ(clampTrackerTimeToRanges(kLivePlayhead, r), kLivePlayhead);
}

// But a latched entity still lowers lo, so the slider minimum snaps onto its
// exact ns (the precision-shortfall fix the clamp originally existed for).
TEST(ClampTrackerTime, LatchedSnapsMinimumUpToItsStamp) {
  const std::vector<EntityTimeRange> r{{kLatched, kLatched}};
  EXPECT_EQ(clampTrackerTimeToRanges(kLatched - 300, r), kLatched);
}

// Latched + live entity: latched lowers lo, the live one bounds hi.
TEST(ClampTrackerTime, LatchedLowersLoLiveBoundsHi) {
  const std::vector<EntityTimeRange> r{{kLatched, kLatched}, {kLivePlayhead, kLiveLast}};
  EXPECT_EQ(clampTrackerTimeToRanges(kLivePlayhead + 100, r), kLivePlayhead + 100);  // inside [lo,hi]
  EXPECT_EQ(clampTrackerTimeToRanges(kLiveLast + 5'000, r), kLiveLast);              // above → live hi
  EXPECT_EQ(clampTrackerTimeToRanges(kLatched - 5, r), kLatched);                    // below → latched lo
}

}  // namespace
}  // namespace pj::scene3d
