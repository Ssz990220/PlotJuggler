// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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

// The tests express stamps as ns/s offsets from the epoch; wrap a duration as an
// absolute TimePoint (TimePoint is a time_point now, not a bare duration). The
// TransformBuffer cache-window arguments stay bare durations, so they are not
// wrapped.
constexpr TimePoint tp(std::chrono::nanoseconds ns) {
  return TimePoint{ns};
}

Transform makeTranslation(double x, double y = 0.0, double z = 0.0) {
  return Transform{{x, y, z}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

Transform makeTranslationFromStamp(TimePoint stamp) {
  return makeTranslation(static_cast<double>(stamp.time_since_epoch().count()));
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

  const std::array<TimePoint, 3> samples{tp(10ns), tp(20ns), tp(30ns)};
  for (const TimePoint stamp : samples) {
    buffer.setTransform(makeStamped("world", "A", stamp, makeTranslationFromStamp(stamp)));
  }

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);

  const std::array<std::pair<TimePoint, double>, 6> queries{
      {{tp(10ns), 10.0}, {tp(15ns), 10.0}, {tp(20ns), 20.0}, {tp(25ns), 20.0}, {tp(30ns), 30.0}, {tp(35ns), 30.0}}};

  for (const auto& [stamp, expected_x] : queries) {
    const auto transform = buffer.lookupTransform("world", "A", stamp);
    expectTranslation(transform, expected_x);
  }
}

TEST(TransformBufferTest, TreeWalkComposition) {
  TransformBuffer buffer;
  const TimePoint stamp = tp(100ns);

  buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslation(10.0, 0.0, 0.0)));
  buffer.setTransform(makeStamped("odom", "base_link", stamp, makeTranslation(0.0, 2.0, 0.0)));

  const auto transform = buffer.lookupTransform("base_link", "map", stamp);
  expectTranslation(transform, -10.0, -2.0, 0.0);
}

TEST(TransformBufferTest, ReparentingIsRejected) {
  TransformBuffer buffer;

  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0))).has_value());

  // A second publisher claims child "A" under a different parent. Rejected with
  // ReparentConflict (not thrown), so a bulk ingest can drop it and continue.
  const auto result = buffer.setTransform(makeStamped("foo", "A", tp(20ns), makeTranslation(2.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::ReparentConflict);
}

// A transform published once resolves at its stamp and every later time, with no
// static flag. This is how /tf_static is modelled now: a single-sample history
// held forward by nearest-previous. /tf_static is conventionally stamped at (or
// near) the recording start, so it covers the whole playback range.
TEST(TransformBufferTest, SingleSampleHoldsForward) {
  TransformBuffer buffer;
  const auto transform = makeTranslation(42.0, -7.0, 3.0);

  buffer.setTransform(makeStamped("world", "A", TimePoint{}, transform));

  const auto distant_future = tp(std::chrono::hours(24));
  expectTranslation(buffer.lookupTransform("world", "A", TimePoint{}), 42.0, -7.0, 3.0);
  expectTranslation(buffer.lookupTransform("world", "A", distant_future), 42.0, -7.0, 3.0);

  // Boundary of the unified model: a query strictly BEFORE the only sample has no
  // value yet. For /tf_static stamped at the recording start this slice never
  // occurs during playback. (Switch sampleAt to clamp-before-first if a late
  // static stamp must still resolve earlier — a 2-line change.)
  const auto distant_past = tp(-std::chrono::hours(24));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", distant_past).has_value());
}

// GNERSIS PR2 review item A1, reproduced at the core level. A namespaced
// "/robot1/tf_static" edge used to be string-matched against "/tf_static" in
// TransformService to decide static-ness, so namespaced static topics fell
// through to dynamic and orphaned. The buffer is now told nothing about the
// topic: a single sample at the recording start resolves for every later query
// regardless of the topic name, so the bug cannot exist.
TEST(TransformBufferTest, NamespacedStaticResolvesWithoutTopicHint) {
  TransformBuffer buffer;
  buffer.setTransform(makeStamped("base_link", "camera", TimePoint{}, makeTranslation(0.5, 0.0, 1.0)));

  for (const TimePoint stamp : {tp(0ns), tp(1'000'000'000ns), tp(999'000'000'000ns)}) {
    const auto tf = buffer.tryLookupTransform("base_link", "camera", stamp);
    ASSERT_TRUE(tf.has_value());
    expectTranslation(*tf, 0.5, 0.0, 1.0);
  }
}

// A re-latched static transform arrives several times with identical values but
// increasing stamps. No special case: the vector keeps the samples and
// nearest-previous resolves them, including far past the last stamp.
TEST(TransformBufferTest, RepublishedStaticUsesNearestPrevious) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto value = makeTranslation(9.0, 0.0, 0.0);
  for (const TimePoint stamp : {tp(0s), tp(1s), tp(2s)}) {
    buffer.setTransform(makeStamped("map", "sensor", stamp, value));
  }
  expectTranslation(buffer.lookupTransform("map", "sensor", tp(0s)), 9.0);
  expectTranslation(buffer.lookupTransform("map", "sensor", tp(5s)), 9.0);  // held forward past last stamp
  const auto distant_future = tp(std::chrono::hours(24));
  expectTranslation(buffer.lookupTransform("map", "sensor", distant_future), 9.0);
}

// A dynamic edge that stops updating keeps resolving at and after its last sample
// forever — even under a finite (streaming) window — because eviction runs only
// when THAT edge is written and never empties it. This is what the static flag
// used to guarantee, now applied to every edge uniformly.
TEST(TransformBufferTest, StoppedDynamicKeepsResolvingUnderFiniteWindow) {
  TransformBuffer buffer(10ns);  // tiny rolling window
  for (const TimePoint stamp : {tp(0ns), tp(5ns), tp(10ns)}) {
    buffer.setTransform(makeStamped("odom", "base", stamp, makeTranslationFromStamp(stamp)));
  }
  // The edge stops here. A query far in the future still holds the last sample.
  ASSERT_TRUE(buffer.tryLookupTransform("odom", "base", tp(1'000'000ns)).has_value());
  expectTranslation(buffer.lookupTransform("odom", "base", tp(1'000'000ns)), 10.0);
}

// Under a finite window, a time jump much larger than the window evicts stale
// samples, but the most recent one is pinned (samples.size() > 1 guard): the edge
// is never emptied, so lookups at/after the jump resolve. Scrubbing back below the
// retained tail orphans — the inherent cost of a finite window, absent under kKeepAll.
TEST(TransformBufferTest, FiniteWindowPinsMostRecentSample) {
  TransformBuffer buffer(10ns);
  buffer.setTransform(makeStamped("map", "odom", tp(0ns), makeTranslation(0.0)));
  buffer.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(100.0)));  // jump >> window

  // Most recent sample pinned and resolving...
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(200ns)).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(200ns)), 100.0);
  // ...older sample evicted: a scrub back below the retained tail has no value.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(50ns)).has_value());
}

TEST(TransformBufferTest, Introspection) {
  TransformBuffer buffer;
  const TimePoint stamp_a = tp(11ns);

  buffer.setTransform(makeStamped("world", "A", stamp_a, makeTranslation(1.0)));
  buffer.setTransform(makeStamped("world", "B", tp(12ns), makeTranslation(2.0)));
  buffer.setTransform(makeStamped("A", "C", tp(13ns), makeTranslation(3.0)));

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

  // B is now just a single-sample edge stamped at 12ns (no static special-case),
  // so its latest sample is 12ns rather than the old static sentinel TimePoint{}.
  const auto latest_b = buffer.getLatestSample("B");
  EXPECT_TRUE(latest_b.has_value());
  if (latest_b.has_value()) {
    EXPECT_EQ(*latest_b, tp(12ns));
  }
}

TEST(TransformBufferTest, TryLookupNoThrow) {
  TransformBuffer buffer;

  buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0)));

  EXPECT_FALSE(buffer.tryLookupTransform("world", "missing", tp(10ns)).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "missing", tp(10ns)), std::runtime_error);

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW(buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);
}

TEST(TransformBufferTest, CyclicTreeDoesNotHang) {
  TransformBuffer buffer;

  // Malformed TF tree: A and B parent each other. Neither call is a reparent
  // (each child's parent is set exactly once), so the reparent guard does not
  // reject it. chainToRoot must still terminate.
  buffer.setTransform(makeStamped("B", "A", tp(10ns), makeTranslation(1.0)));
  buffer.setTransform(makeStamped("A", "B", tp(10ns), makeTranslation(2.0)));

  // Walking A toward its root traverses the cycle; this must return cleanly
  // rather than spin forever. No path exists to an unrelated frame.
  EXPECT_FALSE(buffer.tryLookupTransform("A", "unrelated", tp(10ns)).has_value());
}

TEST(TransformBufferTest, SelfParentIgnored) {
  TransformBuffer buffer;

  // A frame relative to itself is the identity and must not register a
  // self-loop in the parent map; it is reported as a dropped SelfLoop edge.
  const auto result = buffer.setTransform(makeStamped("A", "A", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::SelfLoop);

  EXPECT_FALSE(buffer.getParent("A").has_value());
}

// Reproduces the "dynamic-frame object only renders at the end of the timeline"
// bug: the default 10s rolling window, fed a whole file's TF in time order,
// trims each dynamic edge to its tail so lookups before [t_end - window] fail.
TEST(TransformBufferTest, DefaultWindowEvictsHistoryAfterBulkIngest) {
  TransformBuffer buffer;  // default 10s window
  const std::array<TimePoint, 4> stamps{tp(0s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // cutoff = 12s - 10s = 2s, so the 0s sample is evicted: a lookup at 1s fails...
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());
  // ...while the retained tail still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s)).has_value());
}

// The fix: kKeepAll disables eviction, so the full history stays queryable —
// what the TransformService uses for bulk-ingested files.
TEST(TransformBufferTest, KeepAllRetainsFullHistoryAfterBulkIngest) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const std::array<TimePoint, 4> stamps{tp(0s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // Early sample retained: lookup at 1s holds the 0s value (ZOH).
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(1s)), 0.0);
  expectTranslation(
      buffer.lookupTransform("map", "odom", tp(6s)), static_cast<double>(tp(5s).time_since_epoch().count()));
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s)).has_value());
}

TEST(TransformBufferTest, QuaternionNormalize) {
  TransformBuffer buffer;
  const Transform unnormalized{{1.0, 2.0, 3.0}, glm::dquat{2.0, 0.0, 0.0, 0.0}};

  buffer.setTransform(makeStamped("world", "A", tp(10ns), unnormalized));

  const auto transform = buffer.lookupTransform("world", "A", tp(10ns));
  EXPECT_NEAR(quaternionNorm(transform.q), 1.0, kTolerance);
}

}  // namespace
}  // namespace PJ
