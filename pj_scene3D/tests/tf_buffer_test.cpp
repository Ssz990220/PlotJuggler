// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/tf/tf_buffer.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace PJ {
namespace {

using pj::scene3d::Duration;
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
    (void)buffer.setTransform(makeStamped("world", "A", stamp, makeTranslationFromStamp(stamp)));
  }

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);

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

  (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslation(10.0, 0.0, 0.0)));
  (void)buffer.setTransform(makeStamped("odom", "base_link", stamp, makeTranslation(0.0, 2.0, 0.0)));

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

  (void)buffer.setTransform(makeStamped("world", "A", TimePoint{}, transform));

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
  (void)buffer.setTransform(makeStamped("base_link", "camera", TimePoint{}, makeTranslation(0.5, 0.0, 1.0)));

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
    (void)buffer.setTransform(makeStamped("map", "sensor", stamp, value));
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
    (void)buffer.setTransform(makeStamped("odom", "base", stamp, makeTranslationFromStamp(stamp)));
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
  (void)buffer.setTransform(makeStamped("map", "odom", tp(0ns), makeTranslation(0.0)));
  (void)buffer.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(100.0)));  // jump >> window

  // Most recent sample pinned and resolving...
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(200ns)).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(200ns)), 100.0);
  // ...older sample evicted: a scrub back below the retained tail has no value.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(50ns)).has_value());
}

TEST(TransformBufferTest, Introspection) {
  TransformBuffer buffer;
  const TimePoint stamp_a = tp(11ns);

  (void)buffer.setTransform(makeStamped("world", "A", stamp_a, makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "B", tp(12ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("A", "C", tp(13ns), makeTranslation(3.0)));

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

// The out-param getAllFrames overload (used by the axis pass to reuse capacity
// across frames) must produce the same frame set as the by-value version and
// must CLEAR pre-existing content in the caller's vector first.
TEST(TransformBufferTest, GetAllFramesOutParamMatchesAndClears) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("world", "A", tp(11ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "B", tp(12ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("A", "C", tp(13ns), makeTranslation(3.0)));

  // Pre-seed stale entries to prove the overload clears before refilling.
  std::vector<std::string> scratch{"stale_one", "stale_two"};
  buffer.getAllFrames(scratch);

  const std::set<std::string> got(scratch.begin(), scratch.end());
  const std::set<std::string> expected{"world", "A", "B", "C"};
  EXPECT_EQ(scratch.size(), 4U);
  EXPECT_EQ(got, expected);

  // Same content as the by-value overload it now delegates to.
  const auto by_value = buffer.getAllFrames();
  EXPECT_EQ(std::set<std::string>(by_value.begin(), by_value.end()), got);

  // Reuse on an empty buffer empties the vector (clear actually runs).
  TransformBuffer empty;
  empty.getAllFrames(scratch);
  EXPECT_TRUE(scratch.empty());
}

TEST(TransformBufferTest, TryLookupNoThrow) {
  TransformBuffer buffer;

  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0)));

  EXPECT_FALSE(buffer.tryLookupTransform("world", "missing", tp(10ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "missing", tp(10ns)), std::runtime_error);

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);
}

TEST(TransformBufferTest, CyclicTreeDoesNotHang) {
  TransformBuffer buffer;

  // Malformed TF tree: A and B parent each other. Neither call is a reparent
  // (each child's parent is set exactly once), so the reparent guard does not
  // reject it. chainToRoot must still terminate.
  (void)buffer.setTransform(makeStamped("B", "A", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("A", "B", tp(10ns), makeTranslation(2.0)));

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
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
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
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
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

  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), unnormalized));

  const auto transform = buffer.lookupTransform("world", "A", tp(10ns));
  EXPECT_NEAR(quaternionNorm(transform.q), 1.0, kTolerance);
}

// A zero-length quaternion (a realistic wire value: an uninitialized publisher or
// a proto3 decoder that doesn't default w=1) would normalize to all-NaN and
// silently poison every lookup through the edge. It must be rejected, not stored.
TEST(TransformBufferTest, ZeroQuaternionRejected) {
  TransformBuffer buffer;
  const Transform zero_rot{{1.0, 2.0, 3.0}, glm::dquat{0.0, 0.0, 0.0, 0.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), zero_rot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::InvalidRotation);
  // No edge stored, so the frame is unknown.
  EXPECT_FALSE(buffer.getParent("A").has_value());
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(10ns)).has_value());
}

TEST(TransformBufferTest, NaNQuaternionRejected) {
  TransformBuffer buffer;
  const double nan = std::nan("");
  const Transform nan_rot{{1.0, 2.0, 3.0}, glm::dquat{nan, 0.0, 0.0, 1.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), nan_rot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::InvalidRotation);
  EXPECT_FALSE(buffer.getParent("A").has_value());
}

TEST(TransformBufferTest, NonFiniteTranslationRejected) {
  TransformBuffer buffer;
  const double inf = std::numeric_limits<double>::infinity();
  const Transform bad_t{{std::nan(""), 0.0, inf}, glm::dquat{1.0, 0.0, 0.0, 0.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), bad_t));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::NonFiniteTranslation);
  EXPECT_FALSE(buffer.getParent("A").has_value());
}

TEST(TransformBufferTest, EmptyFrameNameRejected) {
  TransformBuffer buffer;

  const auto empty_parent = buffer.setTransform(makeStamped("", "child", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(empty_parent.has_value());
  EXPECT_EQ(empty_parent.error(), SetTransformError::InvalidFrameName);

  const auto empty_child = buffer.setTransform(makeStamped("parent", "", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(empty_child.has_value());
  EXPECT_EQ(empty_child.error(), SetTransformError::InvalidFrameName);

  // Neither phantom "" frame leaked into the buffer.
  EXPECT_TRUE(buffer.getAllFrames().empty());
}

// areConnected is a pure reachability predicate (no time arg): true once a common
// ancestor exists, regardless of per-edge sample times.
TEST(TransformBufferTest, AreConnectedReportsReachability) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("map", "odom", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "drone", tp(10ns), makeTranslation(1.0)));

  EXPECT_TRUE(buffer.areConnected("map", "base"));
  EXPECT_TRUE(buffer.areConnected("base", "map"));
  EXPECT_TRUE(buffer.areConnected("base", "base"));   // identity
  EXPECT_FALSE(buffer.areConnected("map", "drone"));  // separate forest
  EXPECT_FALSE(buffer.areConnected("map", "ghost"));  // unknown frame
}

// setCacheWindow trims existing histories with the same rule as ingest eviction,
// always keeping the last sample per edge so recent lookups still succeed.
TEST(TransformBufferTest, SetCacheWindowTrimsButKeepsLastSample) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const std::array<TimePoint, 4> stamps{tp(0s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // Under kKeepAll the full history is present: an early lookup resolves.
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());

  buffer.setCacheWindow(10s);
  EXPECT_EQ(buffer.cacheWindow(), Duration(10s));

  // cutoff = 12s - 10s = 2s, so the 0s sample is evicted retroactively.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());
  // The retained tail (and so any recent stamp) still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s)).has_value());
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s)).has_value());
}

// A single-sample (static) edge survives any window: setCacheWindow never empties
// an edge, so a static frame stays resolvable forever.
TEST(TransformBufferTest, SetCacheWindowKeepsStaticEdge) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("base_link", "camera", tp(0s), makeTranslation(0.5)));

  buffer.setCacheWindow(1ns);  // aggressively small window
  const auto distant_future = tp(std::chrono::hours(24));
  ASSERT_TRUE(buffer.tryLookupTransform("base_link", "camera", distant_future).has_value());
  expectTranslation(buffer.lookupTransform("base_link", "camera", distant_future), 0.5);
}

// revision() bumps on every successful insert/replace and on clear(), but NOT on
// a rejected edge — it is a change-detection token for callers gating recompute.
TEST(TransformBufferTest, RevisionBumpsOnMutationNotOnReject) {
  TransformBuffer buffer;
  const uint64_t initial = buffer.revision();

  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0))).has_value());
  const uint64_t after_insert = buffer.revision();
  EXPECT_GT(after_insert, initial);

  // In-place replace at the same stamp is still a mutation: bump.
  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(2.0))).has_value());
  EXPECT_GT(buffer.revision(), after_insert);
  const uint64_t after_replace = buffer.revision();

  // A rejected edge (self-loop) does not bump.
  EXPECT_FALSE(buffer.setTransform(makeStamped("B", "B", tp(20ns), makeTranslation(1.0))).has_value());
  EXPECT_EQ(buffer.revision(), after_replace);

  // A rejected invalid-rotation edge does not bump either.
  const Transform zero_rot{{0.0, 0.0, 0.0}, glm::dquat{0.0, 0.0, 0.0, 0.0}};
  EXPECT_FALSE(buffer.setTransform(makeStamped("world", "C", tp(30ns), zero_rot)).has_value());
  EXPECT_EQ(buffer.revision(), after_replace);

  // clear() bumps.
  buffer.clear();
  EXPECT_GT(buffer.revision(), after_replace);
}

}  // namespace
}  // namespace PJ
