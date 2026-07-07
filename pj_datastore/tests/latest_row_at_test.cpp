// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for DataReader::latestRowAt — the single-row, multi-column snapshot read
// that backs snapshot ("current message") plots. The contract under test: every
// requested column is read from ONE row at-or-before the query time, so a set of
// columns can never mix values from different messages, and a null / absent cell
// reads as nullopt rather than falling back to an earlier row.

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

// Schema: struct msg { float64[2] positions, float64 stamp } — a minimal stand-in
// for the "array element with leaf fields" shape snapshot plots read. Columns
// flatten in declared order to: positions[0]=0, positions[1]=1, stamp=2.
class LatestRowAtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = engine_.createDataset(DatasetDescriptor{.source_name = "snap"});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = *dataset_or;

    DataWriter writer = engine_.createWriter();
    auto positions = makeArray("positions", makePrimitive("", PrimitiveType::kFloat64), 2u);
    auto root = makeStruct("msg", {positions, makePrimitive("stamp", PrimitiveType::kFloat64)});
    auto schema_or = writer.registerSchema("msg", root);
    ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

    TopicDescriptor descriptor;
    descriptor.name = "/msg";
    descriptor.schema_id = *schema_or;
    descriptor.max_chunk_rows = 2;  // force multiple chunks so latestAt merges across them
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
    topic_id_ = *topic_or;
    ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());

    // Row 0 @ t=10: positions=[1,2],   stamp=100
    // Row 1 @ t=20: positions=[3,null],stamp=200  (positions[1] null: a ragged element)
    // Row 2 @ t=30: positions=[5,6],   stamp=300
    writeRow(writer, 10, 1.0, 2.0, true, 100.0);
    writeRow(writer, 20, 3.0, 0.0, false, 200.0);
    writeRow(writer, 30, 5.0, 6.0, true, 300.0);
    ASSERT_FALSE(engine_.commitChunks(writer.flushAll()).empty());
  }

  void writeRow(DataWriter& writer, Timestamp t, double p0, double p1, bool p1_valid, double stamp) {
    ASSERT_TRUE(writer.beginRow(topic_id_, t).has_value());
    writer.set(topic_id_, 0, p0);
    if (p1_valid) {
      writer.set(topic_id_, 1, p1);
    } else {
      writer.setNull(topic_id_, 1);
    }
    writer.set(topic_id_, 2, stamp);
    ASSERT_TRUE(writer.finishRow(topic_id_).has_value());
  }

  DataEngine engine_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
};

TEST_F(LatestRowAtTest, ReadsAllColumnsFromOneRow) {
  DataReader reader = engine_.createReader();

  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 30}, {0, 1, 2});
  ASSERT_TRUE(snap_or.has_value()) << snap_or.error();
  ASSERT_TRUE(snap_or->has_value());
  const RowSnapshot& snap = **snap_or;
  EXPECT_EQ(snap.timestamp, 30);
  ASSERT_EQ(snap.values.size(), 3U);
  ASSERT_TRUE(snap.values[0].has_value());
  EXPECT_DOUBLE_EQ(*snap.values[0], 5.0);
  ASSERT_TRUE(snap.values[1].has_value());
  EXPECT_DOUBLE_EQ(*snap.values[1], 6.0);
  ASSERT_TRUE(snap.values[2].has_value());
  EXPECT_DOUBLE_EQ(*snap.values[2], 300.0);
}

TEST_F(LatestRowAtTest, ZeroOrderHoldPicksRowAtOrBeforeTime) {
  DataReader reader = engine_.createReader();

  // t=25 → row 1 (@20). Every column comes from THAT row, not a mix with row 2.
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 25}, {0, 2});
  ASSERT_TRUE(snap_or.has_value() && snap_or->has_value());
  EXPECT_EQ((*snap_or)->timestamp, 20);
  EXPECT_DOUBLE_EQ(*(*snap_or)->values[0], 3.0);
  EXPECT_DOUBLE_EQ(*(*snap_or)->values[1], 200.0);
}

TEST_F(LatestRowAtTest, NullCellReadsAsNulloptNeverFallsBackToEarlierRow) {
  DataReader reader = engine_.createReader();

  // Row 1 (@20) has positions[1] null even though row 0 (@10) has 2.0 there. The
  // snapshot for the row-at-20 must report nullopt for positions[1] — reading the
  // held-over 2.0 would mix a stale vintage into the "current message".
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 20}, {0, 1});
  ASSERT_TRUE(snap_or.has_value() && snap_or->has_value());
  EXPECT_EQ((*snap_or)->timestamp, 20);
  ASSERT_TRUE((*snap_or)->values[0].has_value());
  EXPECT_DOUBLE_EQ(*(*snap_or)->values[0], 3.0);
  EXPECT_FALSE((*snap_or)->values[1].has_value());
}

TEST_F(LatestRowAtTest, OutOfRangeColumnReadsAsNullopt) {
  DataReader reader = engine_.createReader();
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 30}, {0, 99});
  ASSERT_TRUE(snap_or.has_value() && snap_or->has_value());
  ASSERT_TRUE((*snap_or)->values[0].has_value());
  EXPECT_FALSE((*snap_or)->values[1].has_value());
}

TEST_F(LatestRowAtTest, NoRowAtOrBeforeTimeYieldsNulloptPayload) {
  DataReader reader = engine_.createReader();
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 5}, {0, 1, 2});
  ASSERT_TRUE(snap_or.has_value()) << snap_or.error();
  EXPECT_FALSE(snap_or->has_value());
}

TEST_F(LatestRowAtTest, UnknownTopicIsError) {
  DataReader reader = engine_.createReader();
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = 999999, .t = 30}, {0});
  EXPECT_FALSE(snap_or.has_value());
}

TEST_F(LatestRowAtTest, EmptyColumnListReturnsRowTimestampOnly) {
  DataReader reader = engine_.createReader();
  auto snap_or = reader.latestRowAt(QueryPoint{.topic_id = topic_id_, .t = 30}, {});
  ASSERT_TRUE(snap_or.has_value() && snap_or->has_value());
  EXPECT_EQ((*snap_or)->timestamp, 30);
  EXPECT_TRUE((*snap_or)->values.empty());
}

}  // namespace
}  // namespace PJ
