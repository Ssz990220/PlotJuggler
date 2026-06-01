#include "pj_scene3d_core/tf/tf_buffer.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace PJ {
namespace {

using pj::scene3d::SetTransformError;
using pj::scene3d::StampedTransform;
using pj::scene3d::TimePoint;
using pj::scene3d::Transform;
using pj::scene3d::TransformBuffer;
using namespace std::chrono_literals;

constexpr double kTolerance = 1e-9;

Transform makeTranslation(double x, double y = 0.0, double z = 0.0) {
  return Transform{{x, y, z}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

Transform makeTranslationFromStamp(TimePoint stamp) {
  return makeTranslation(static_cast<double>(stamp.count()));
}

StampedTransform makeStamped(
    const std::string& parent, const std::string& child, TimePoint stamp, const Transform& transform) {
  return StampedTransform{stamp, parent, child, transform};
}

void expectTranslation(const Transform& transform, double x, double y = 0.0, double z = 0.0) {
  EXPECT_NEAR(transform.t.x, x, kTolerance);
  EXPECT_NEAR(transform.t.y, y, kTolerance);
  EXPECT_NEAR(transform.t.z, z, kTolerance);
}

double quaternionNorm(const glm::dquat& q) {
  return std::sqrt((q.w * q.w) + (q.x * q.x) + (q.y * q.y) + (q.z * q.z));
}

TEST(TransformBufferTest, ZOHBoundary) {
  TransformBuffer buffer;

  const std::array<TimePoint, 3> samples{10ns, 20ns, 30ns};
  for (const TimePoint stamp : samples) {
    buffer.setTransform(makeStamped("world", "A", stamp, makeTranslationFromStamp(stamp)));
  }

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", 5ns).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "A", 5ns), std::runtime_error);

  const std::array<std::pair<TimePoint, double>, 6> queries{
      {{10ns, 10.0}, {15ns, 10.0}, {20ns, 20.0}, {25ns, 20.0}, {30ns, 30.0}, {35ns, 30.0}}};

  for (const auto& [stamp, expected_x] : queries) {
    const auto transform = buffer.lookupTransform("world", "A", stamp);
    expectTranslation(transform, expected_x);
  }
}

TEST(TransformBufferTest, TreeWalkComposition) {
  TransformBuffer buffer;
  const TimePoint stamp = 100ns;

  buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslation(10.0, 0.0, 0.0)));
  buffer.setTransform(makeStamped("odom", "base_link", stamp, makeTranslation(0.0, 2.0, 0.0)));

  const auto transform = buffer.lookupTransform("base_link", "map", stamp);
  expectTranslation(transform, -10.0, -2.0, 0.0);
}

TEST(TransformBufferTest, ReparentingIsRejected) {
  TransformBuffer buffer;

  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", 10ns, makeTranslation(1.0))).has_value());

  // A second publisher claims child "A" under a different parent. Rejected with
  // ReparentConflict (not thrown), so a bulk ingest can drop it and continue.
  const auto result = buffer.setTransform(makeStamped("foo", "A", 20ns, makeTranslation(2.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::ReparentConflict);
}

TEST(TransformBufferTest, StaticTransform) {
  TransformBuffer buffer;
  const auto transform = makeTranslation(42.0, -7.0, 3.0);

  buffer.setTransform(makeStamped("world", "A", 123ns, transform), true);

  const auto distant_future = std::chrono::duration_cast<TimePoint>(std::chrono::hours(24));
  const auto distant_past = -std::chrono::duration_cast<TimePoint>(std::chrono::hours(24));

  expectTranslation(buffer.lookupTransform("world", "A", TimePoint{}), 42.0, -7.0, 3.0);
  expectTranslation(buffer.lookupTransform("world", "A", distant_future), 42.0, -7.0, 3.0);
  expectTranslation(buffer.lookupTransform("world", "A", distant_past), 42.0, -7.0, 3.0);
}

TEST(TransformBufferTest, Introspection) {
  TransformBuffer buffer;
  const TimePoint stamp_a = 11ns;

  buffer.setTransform(makeStamped("world", "A", stamp_a, makeTranslation(1.0)));
  buffer.setTransform(makeStamped("world", "B", 12ns, makeTranslation(2.0)), true);
  buffer.setTransform(makeStamped("A", "C", 13ns, makeTranslation(3.0)));

  const auto all_frames = buffer.getAllFrames();
  const std::set<std::string> frames(all_frames.begin(), all_frames.end());
  const std::set<std::string> expected_frames{"world", "A", "B", "C"};

  EXPECT_EQ(all_frames.size(), 4U);
  EXPECT_EQ(frames, expected_frames);

  const auto parent = buffer.getParent("A");
  EXPECT_TRUE(parent.has_value());
  if (parent.has_value()) {
    EXPECT_EQ(*parent, "world");
  }
  EXPECT_FALSE(buffer.getParent("nonexistent").has_value());

  const auto latest_a = buffer.getLatestSample("A");
  EXPECT_TRUE(latest_a.has_value());
  if (latest_a.has_value()) {
    EXPECT_EQ(*latest_a, stamp_a);
  }
  EXPECT_FALSE(buffer.getLatestSample("nonexistent").has_value());

  const auto latest_b = buffer.getLatestSample("B");
  EXPECT_TRUE(latest_b.has_value());
  if (latest_b.has_value()) {
    EXPECT_EQ(*latest_b, TimePoint{});
  }
}

TEST(TransformBufferTest, TryLookupNoThrow) {
  TransformBuffer buffer;

  buffer.setTransform(makeStamped("world", "A", 10ns, makeTranslation(1.0)));

  EXPECT_FALSE(buffer.tryLookupTransform("world", "missing", 10ns).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "missing", 10ns), std::runtime_error);

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", 5ns).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "A", 5ns), std::runtime_error);
}

TEST(TransformBufferTest, CyclicTreeDoesNotHang) {
  TransformBuffer buffer;

  // Malformed TF tree: A and B parent each other. Neither call is a reparent
  // (each child's parent is set exactly once), so the reparent guard does not
  // reject it. chainToRoot must still terminate.
  buffer.setTransform(makeStamped("B", "A", 10ns, makeTranslation(1.0)));
  buffer.setTransform(makeStamped("A", "B", 10ns, makeTranslation(2.0)));

  // Walking A toward its root traverses the cycle; this must return cleanly
  // rather than spin forever. No path exists to an unrelated frame.
  EXPECT_FALSE(buffer.tryLookupTransform("A", "unrelated", 10ns).has_value());
}

TEST(TransformBufferTest, SelfParentIgnored) {
  TransformBuffer buffer;

  // A frame relative to itself is the identity and must not register a
  // self-loop in the parent map; it is reported as a dropped SelfLoop edge.
  const auto result = buffer.setTransform(makeStamped("A", "A", 10ns, makeTranslation(1.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::SelfLoop);

  EXPECT_FALSE(buffer.getParent("A").has_value());
}

// Reproduces the "dynamic-frame object only renders at the end of the timeline"
// bug: the default 10s rolling window, fed a whole file's TF in time order,
// trims each dynamic edge to its tail so lookups before [t_end - window] fail.
TEST(TransformBufferTest, DefaultWindowEvictsHistoryAfterBulkIngest) {
  TransformBuffer buffer;  // default 10s window
  const std::array<TimePoint, 4> stamps{0s, 5s, 11s, 12s};
  for (const TimePoint stamp : stamps) {
    buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // cutoff = 12s - 10s = 2s, so the 0s sample is evicted: a lookup at 1s fails...
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", 1s).has_value());
  // ...while the retained tail still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", 11s).has_value());
}

// The fix: kKeepAll disables eviction, so the full history stays queryable —
// what the TransformService uses for bulk-ingested files.
TEST(TransformBufferTest, KeepAllRetainsFullHistoryAfterBulkIngest) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const std::array<TimePoint, 4> stamps{0s, 5s, 11s, 12s};
  for (const TimePoint stamp : stamps) {
    buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // Early sample retained: lookup at 1s holds the 0s value (ZOH).
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", 1s).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", 1s), 0.0);
  expectTranslation(buffer.lookupTransform("map", "odom", 6s), static_cast<double>(TimePoint(5s).count()));
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", 12s).has_value());
}

TEST(TransformBufferTest, QuaternionNormalize) {
  TransformBuffer buffer;
  const Transform unnormalized{{1.0, 2.0, 3.0}, glm::dquat{2.0, 0.0, 0.0, 0.0}};

  buffer.setTransform(makeStamped("world", "A", 10ns, unnormalized));

  const auto transform = buffer.lookupTransform("world", "A", 10ns);
  EXPECT_NEAR(quaternionNorm(transform.q), 1.0, kTolerance);
}

}  // namespace
}  // namespace PJ
