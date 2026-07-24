// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Focused unit tests for the OrderedEntries value type: they exercise the
// ordering invariants (timestamp-ascending storage + a uid_order side index that
// is a strictly-ascending-by-UID permutation) DIRECTLY through the encapsulated
// API, across in-order and out-of-order pushes, eviction, re-UID, rebuild, shift,
// and the cross-series bulk ops. The ObjectStore black-box tests cover the same
// behavior end to end; these pin the invariant at the value-type boundary.

#include "pj_datastore/ordered_entries.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <vector>

namespace PJ {
namespace {

// Build an entry at `ts` with a freshly minted (globally increasing) UID, exactly
// as ObjectStore's push paths do. The payload is left default (irrelevant here).
ObjectEntry makeEntry(Timestamp ts, SequentialUID* out_uid = nullptr) {
  ObjectEntry entry;
  entry.timestamp = ts;
  entry.sequential_uid = SequentialUID::getNext();
  if (out_uid != nullptr) {
    *out_uid = entry.sequential_uid;
  }
  return entry;
}

// Walk the UID cursor from the front (firstUid + nextUidAfter), returning the
// UIDs in ascending-UID (== arrival) order.
std::vector<SequentialUID> uidWalk(const OrderedEntries& oe) {
  std::vector<SequentialUID> out;
  for (SequentialUID u = oe.firstUid(); u.valid(); u = oe.nextUidAfter(u)) {
    out.push_back(u);
  }
  return out;
}

// The stored timestamps, in array order (== ascending).
std::vector<Timestamp> stamps(const OrderedEntries& oe) {
  return {oe.timestamps().begin(), oe.timestamps().end()};
}

TEST(OrderedEntriesTest, InOrderPushKeepsTimestampsAndUidCursor) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(30, &c));

  EXPECT_EQ(oe.size(), 3u);
  EXPECT_FALSE(oe.empty());
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_EQ(oe.frontTimestamp(), 10);
  EXPECT_EQ(oe.backTimestamp(), 30);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c}));
  EXPECT_EQ(oe.firstUid(), a);

  ASSERT_NE(oe.atUid(b), nullptr);
  EXPECT_EQ(oe.atUid(b)->timestamp, 20);
  ASSERT_NE(oe.atIndex(2), nullptr);
  EXPECT_EQ(oe.atIndex(2)->timestamp, 30);
  EXPECT_EQ(oe.atIndex(3), nullptr);  // out of range
}

TEST(OrderedEntriesTest, IndexAtOrBeforeMatchesAtOrBeforeSemantics) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(20));
  oe.push(makeEntry(30));

  EXPECT_FALSE(oe.indexAtOrBefore(5).has_value());  // before first
  EXPECT_EQ(oe.indexAtOrBefore(10), std::optional<size_t>(0));
  EXPECT_EQ(oe.indexAtOrBefore(25), std::optional<size_t>(1));
  EXPECT_EQ(oe.indexAtOrBefore(30), std::optional<size_t>(2));
  EXPECT_EQ(oe.indexAtOrBefore(999), std::optional<size_t>(2));
  EXPECT_FALSE(OrderedEntries{}.indexAtOrBefore(0).has_value());  // empty
}

TEST(OrderedEntriesTest, OutOfOrderPushSortsTimestampsButKeepsArrivalUidOrder) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));  // in order
  oe.push(makeEntry(30, &b));  // in order
  oe.push(makeEntry(20, &c));  // OUT of order: sorts between a and b by timestamp

  // Timestamps ascending; the middle slot is the out-of-order entry (uid c).
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_EQ(oe.entryAt(1).sequential_uid, c);
  EXPECT_EQ(oe.entryAt(1).timestamp, 20);

  // UID cursor is arrival order (a < b < c), independent of timestamp order.
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c}));
  ASSERT_NE(oe.atUid(c), nullptr);
  EXPECT_EQ(oe.atUid(c)->timestamp, 20);
  ASSERT_NE(oe.atUid(b), nullptr);
  EXPECT_EQ(oe.atUid(b)->timestamp, 30);
}

TEST(OrderedEntriesTest, RangeByTimeIsHalfOpenAscendingAndDecodeFree) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  oe.push(makeEntry(20, &c));  // out of order

  const auto window = oe.rangeByTime(15, 30);  // (15, 30] -> ts 20, 30
  ASSERT_EQ(window.size(), 2u);
  EXPECT_EQ(window[0].uid, c);  // ascending timestamp order
  EXPECT_EQ(window[0].timestamp, 20);
  EXPECT_EQ(window[1].uid, b);
  EXPECT_EQ(window[1].timestamp, 30);

  EXPECT_TRUE(oe.rangeByTime(30, 30).empty());    // hi == lo -> empty
  EXPECT_TRUE(oe.rangeByTime(40, 5).empty());     // reversed -> empty
  EXPECT_TRUE(oe.rangeByTime(100, 200).empty());  // nothing in window
}

TEST(OrderedEntriesTest, MaxUidAtOrBeforeIsHighWaterNotLatestTimestamp) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(15, &c));  // OUT of order: newest UID, but middle timestamp

  // Entries by timestamp: 10(a), 15(c), 20(b). At-or-before 20 covers all three,
  // so the high-water UID is c (the newest arrival), NOT b (the latest timestamp).
  EXPECT_EQ(oe.maxUidAtOrBefore(20), c);
  ASSERT_TRUE(oe.indexAtOrBefore(20).has_value());
  EXPECT_EQ(oe.entryAt(*oe.indexAtOrBefore(20)).sequential_uid, b);  // latest-timestamp differs
  EXPECT_EQ(oe.maxUidAtOrBefore(10), a);
  EXPECT_FALSE(oe.maxUidAtOrBefore(5).valid());  // nothing at or before
}

TEST(OrderedEntriesTest, EvictFrontKeepsCursorConsistentAcrossInversion) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  oe.push(makeEntry(20, &c));  // out of order -> array: 10(a),20(c),30(b)

  oe.evictFront();  // drops the ts-10 entry (uid a)
  EXPECT_EQ(oe.size(), 2u);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 30}));
  EXPECT_EQ(oe.firstUid(), b);  // smallest RETAINED uid is b (c arrived later)
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{b, c}));
  EXPECT_EQ(oe.atUid(a), nullptr);  // evicted
  ASSERT_NE(oe.atUid(c), nullptr);
  EXPECT_EQ(oe.atUid(c)->timestamp, 20);
}

TEST(OrderedEntriesTest, ReuidAllMakesUidOrderIdentity) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(30));
  oe.push(makeEntry(20));  // out of order -> uid_order non-identity

  oe.reuidAll();
  // Fresh UIDs in array (timestamp) order: uid strictly ascends with index, and
  // the cursor walk equals the array order.
  ASSERT_EQ(oe.size(), 3u);
  EXPECT_LT(oe.entryAt(0).sequential_uid, oe.entryAt(1).sequential_uid);
  EXPECT_LT(oe.entryAt(1).sequential_uid, oe.entryAt(2).sequential_uid);
  EXPECT_EQ(
      uidWalk(oe), (std::vector<SequentialUID>{
                       oe.entryAt(0).sequential_uid, oe.entryAt(1).sequential_uid, oe.entryAt(2).sequential_uid}));
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));  // timestamps untouched
}

TEST(OrderedEntriesTest, ShiftSlidesTimestampsAndPayloadStampShiftLeavingUidOrder) {
  OrderedEntries oe;
  SequentialUID a, b;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));

  EXPECT_FALSE(oe.shift(0));                // no-op
  EXPECT_FALSE(OrderedEntries{}.shift(5));  // empty no-op

  EXPECT_TRUE(oe.shift(5));
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{15, 25}));
  EXPECT_EQ(oe.entryAt(0).payload_stamp_shift, 5);
  EXPECT_EQ(oe.entryAt(1).payload_stamp_shift, 5);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b}));  // uid order unchanged

  EXPECT_TRUE(oe.shift(5));  // accumulates
  EXPECT_EQ(oe.entryAt(0).payload_stamp_shift, 10);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 30}));
}

TEST(OrderedEntriesTest, AppendReuidFromConcatenatesAndClearsSource) {
  OrderedEntries dst;
  dst.push(makeEntry(10));
  dst.push(makeEntry(30));
  dst.push(makeEntry(20));  // dst prefix has an inversion

  OrderedEntries src;
  src.push(makeEntry(100));
  src.push(makeEntry(110));

  dst.appendReuidFrom(src);
  EXPECT_TRUE(src.empty());
  EXPECT_EQ(dst.size(), 5u);
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30, 100, 110}));
  // The combined uid_order is strictly ascending -> the cursor visits all 5.
  EXPECT_EQ(uidWalk(dst).size(), 5u);
  // atUid resolves the last appended (max-timestamp) entry.
  EXPECT_EQ(dst.entryAt(4).timestamp, 110);
}

TEST(OrderedEntriesTest, AdoptReuidFromReplacesTargetWithIdentityOrder) {
  OrderedEntries dst;
  dst.push(makeEntry(5));  // will be discarded

  OrderedEntries src;
  src.push(makeEntry(10));
  src.push(makeEntry(20));
  src.push(makeEntry(30));

  dst.adoptReuidFrom(src);
  EXPECT_TRUE(src.empty());
  EXPECT_EQ(dst.size(), 3u);
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_LT(dst.entryAt(0).sequential_uid, dst.entryAt(1).sequential_uid);
  EXPECT_EQ(uidWalk(dst).size(), 3u);
}

TEST(OrderedEntriesTest, MoveEntriesIntoThenAssignSortedReuidRebuildsDestination) {
  OrderedEntries dst;
  dst.push(makeEntry(10));
  dst.push(makeEntry(40));

  OrderedEntries srcA;
  srcA.push(makeEntry(20));
  srcA.push(makeEntry(30));

  std::vector<ObjectEntry> pool;
  pool.reserve(dst.size() + srcA.size());
  dst.moveEntriesInto(pool);   // dst now empty
  srcA.moveEntriesInto(pool);  // srcA now empty
  EXPECT_TRUE(dst.empty());
  EXPECT_TRUE(srcA.empty());
  ASSERT_EQ(pool.size(), 4u);

  std::stable_sort(
      pool.begin(), pool.end(), [](const ObjectEntry& l, const ObjectEntry& r) { return l.timestamp < r.timestamp; });
  dst.assignSortedReuid(std::move(pool));
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30, 40}));
  EXPECT_LT(dst.entryAt(0).sequential_uid, dst.entryAt(3).sequential_uid);  // identity uids
  EXPECT_EQ(uidWalk(dst).size(), 4u);
}

TEST(OrderedEntriesTest, DetachRestoreRoundTripPreservesWalkAndInversion) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(30));
  oe.push(makeEntry(20));  // inversion: uid_order not identity
  const std::vector<SequentialUID> before = uidWalk(oe);
  const std::vector<Timestamp> before_ts = stamps(oe);

  std::deque<ObjectEntry> entries_out;
  std::vector<Timestamp> stamps_out;
  oe.detachInto(entries_out, stamps_out);
  EXPECT_TRUE(oe.empty());
  ASSERT_EQ(entries_out.size(), 3u);

  oe.restoreRebuild(std::move(entries_out), std::move(stamps_out));
  EXPECT_EQ(stamps(oe), before_ts);
  EXPECT_EQ(uidWalk(oe), before) << "restoreRebuild must reconstruct the preserved UID inversion";
}

TEST(OrderedEntriesTest, ClearEmptiesEverything) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(20));
  oe.clear();
  EXPECT_TRUE(oe.empty());
  EXPECT_EQ(oe.size(), 0u);
  EXPECT_FALSE(oe.firstUid().valid());
  EXPECT_FALSE(oe.indexAtOrBefore(20).has_value());
  EXPECT_TRUE(oe.timestamps().empty());
}

// push() reports in-order vs out-of-order — the signal ObjectStore uses to decide
// whether to reset its warm latestAt cache.
TEST(OrderedEntriesTest, PushReportsInOrderVsOutOfOrder) {
  OrderedEntries oe;
  EXPECT_EQ(oe.push(makeEntry(10)), OrderedEntries::PushOrder::kInOrderAppend);
  EXPECT_EQ(oe.push(makeEntry(30)), OrderedEntries::PushOrder::kInOrderAppend);
  EXPECT_EQ(oe.push(makeEntry(30)), OrderedEntries::PushOrder::kInOrderAppend) << "equal-to-back stays in-order";
  EXPECT_EQ(oe.push(makeEntry(20)), OrderedEntries::PushOrder::kOutOfOrderInsert) << "ts below back is out-of-order";
}

// Equal timestamps inserted out-of-order keep arrival (UID) order at the value-type
// boundary (upper_bound places a new equal-ts entry after existing equals).
TEST(OrderedEntriesTest, EqualTimestampOutOfOrderInsertKeepsArrivalOrder) {
  OrderedEntries oe;
  SequentialUID a, b, c, d;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  ASSERT_EQ(oe.push(makeEntry(20, &c)), OrderedEntries::PushOrder::kOutOfOrderInsert);
  ASSERT_EQ(oe.push(makeEntry(20, &d)), OrderedEntries::PushOrder::kOutOfOrderInsert);  // equal to c, later arrival

  const auto window = oe.rangeByTime(15, 25);
  ASSERT_EQ(window.size(), 2u);
  EXPECT_EQ(window[0].uid, c) << "earlier arrival first among equal timestamps";
  EXPECT_EQ(window[1].uid, d);
  EXPECT_EQ(window[0].timestamp, 20);
  EXPECT_EQ(window[1].timestamp, 20);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c, d}));
}

// appendReuidFrom re-UIDs only the MOVED source entries; the destination's
// pre-existing UIDs must stay stable and resolvable (consumer cursors hold them).
TEST(OrderedEntriesTest, AppendReuidFromKeepsDestinationUidsResolvable) {
  OrderedEntries dst;
  SequentialUID d0, d1;
  dst.push(makeEntry(10, &d0));
  dst.push(makeEntry(20, &d1));

  OrderedEntries src;
  src.push(makeEntry(30));
  src.push(makeEntry(40));

  dst.appendReuidFrom(src);

  ASSERT_NE(dst.atUid(d0), nullptr) << "pre-existing destination UID must survive appendReuidFrom";
  EXPECT_EQ(dst.atUid(d0)->timestamp, 10);
  ASSERT_NE(dst.atUid(d1), nullptr);
  EXPECT_EQ(dst.atUid(d1)->timestamp, 20);
  EXPECT_EQ(dst.size(), 4u);
  EXPECT_TRUE(src.empty());
}

}  // namespace
}  // namespace PJ
