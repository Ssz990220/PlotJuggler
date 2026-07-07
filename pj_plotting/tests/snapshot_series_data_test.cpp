// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for SnapshotSeriesData against a datastore populated in-test with the
// motivating spline shape: predicted_trajectory[0..28], each element carrying
// positions[0..7] and time_from_start_s. Covers snapshot at several cursor times,
// a RAGGED case (a shorter message must not leak a previous message's elements),
// streaming-append refresh, and the index x-mode.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/SnapshotGroupResolver.h"
#include "pj_plotting/SnapshotSeriesData.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

constexpr Timestamp kNs = 1000000000;
constexpr int kElements = 29;
constexpr int kPositions = 8;

std::string posPath(int i, int j) {
  return "predicted_trajectory[" + std::to_string(i) + "].positions[" + std::to_string(j) + "]";
}
std::string stampPath(int i) {
  return "predicted_trajectory[" + std::to_string(i) + "].time_from_start_s";
}
// The value written for positions[j] of element i in message m — distinct per
// (m, i, j) so a stale vintage is detectable.
double posValue(int m, int i, int j) {
  return 1000.0 * m + 10.0 * i + j;
}
double stampValue(int i) {
  return 0.5 * i;
}

class SnapshotSeriesDataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto ds = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "spline"});
    ASSERT_TRUE(ds.has_value()) << ds.error();
    dataset_id_ = *ds;

    DataWriter writer = session_.dataEngine().createWriter();
    auto positions = makeArray("positions", makePrimitive("", PrimitiveType::kFloat64), kPositions);
    auto element = makeStruct("", {positions, makePrimitive("time_from_start_s", PrimitiveType::kFloat64)});
    auto traj = makeArray("predicted_trajectory", element, kElements);
    auto root = makeStruct("SplineInfo", {traj});
    auto schema = writer.registerSchema("spline", root);
    ASSERT_TRUE(schema.has_value()) << schema.error();

    TopicDescriptor desc;
    desc.name = "/wholebody/left_arm/debug/spline_info";
    desc.schema_id = *schema;
    desc.max_chunk_rows = 2;  // force multi-chunk so latestAt merges across chunks
    auto topic = writer.registerTopic(dataset_id_, desc);
    ASSERT_TRUE(topic.has_value()) << topic.error();
    topic_id_ = *topic;
    ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());

    // Resolve the real (field_path -> column index) map straight from the writer, so
    // the resolver runs against the datastore's actual layout, not an assumed one.
    for (int i = 0; i < kElements; ++i) {
      for (int j = 0; j < kPositions; ++j) {
        registerColumn(writer, posPath(i, j));
      }
      registerColumn(writer, stampPath(i));
    }

    writeMessage(writer, 0 * kNs, /*m=*/0, /*present=*/kElements);
    writeMessage(writer, 1 * kNs, /*m=*/1, /*present=*/kElements);
    writeMessage(writer, 2 * kNs, /*m=*/2, /*present=*/10);  // ragged: only 10 elements
    writeMessage(writer, 3 * kNs, /*m=*/3, /*present=*/kElements);
    ASSERT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  void registerColumn(DataWriter& writer, const std::string& path) {
    auto id = writer.resolveField(topic_id_, path);
    ASSERT_TRUE(id.has_value()) << id.error();
    col_of_[path] = static_cast<std::size_t>(*id);
    columns_.push_back(SnapshotColumn{static_cast<std::size_t>(*id), path});
  }

  void writeMessage(DataWriter& writer, Timestamp t, int m, int present) {
    ASSERT_TRUE(writer.beginRow(topic_id_, t).has_value());
    for (int i = 0; i < present; ++i) {
      for (int j = 0; j < kPositions; ++j) {
        writer.set(topic_id_, col_of_.at(posPath(i, j)), posValue(m, i, j));
      }
      writer.set(topic_id_, col_of_.at(stampPath(i)), stampValue(i));
    }
    // Elements [present, kElements) are left unset → auto-null → absent from the
    // snapshot (the ragged case). No explicit setNull needed.
    ASSERT_TRUE(writer.finishRow(topic_id_).has_value());
  }

  std::vector<SnapshotElement> resolveY(int j) const {
    return resolveSnapshotPattern(columns_, "predicted_trajectory[:].positions[" + std::to_string(j) + "]");
  }
  std::vector<SnapshotElement> resolveX() const {
    return resolveSnapshotPattern(columns_, "predicted_trajectory[:].time_from_start_s");
  }

  SessionManager session_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  std::map<std::string, std::size_t> col_of_;
  std::vector<SnapshotColumn> columns_;
};

TEST_F(SnapshotSeriesDataTest, ColumnModeSnapshotAtSeveralCursorTimes) {
  SnapshotSeriesData series(
      &session_, topic_id_, dataset_id_, SnapshotSeriesData::XMode::kColumn, resolveX(), resolveY(3));

  // t=0.5s → message m=0.
  ASSERT_TRUE(series.refresh(0.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));
  EXPECT_DOUBLE_EQ(series.sample(0).x(), stampValue(0));
  EXPECT_DOUBLE_EQ(series.sample(0).y(), posValue(0, 0, 3));
  EXPECT_DOUBLE_EQ(series.sample(28).x(), stampValue(28));
  EXPECT_DOUBLE_EQ(series.sample(28).y(), posValue(0, 28, 3));

  // t=1.5s → message m=1 (zero-order hold picks the row at 1s).
  ASSERT_TRUE(series.refresh(1.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));
  EXPECT_DOUBLE_EQ(series.sample(5).y(), posValue(1, 5, 3));

  // Re-refresh at the same time reports no change (replot can be skipped).
  EXPECT_FALSE(series.refresh(1.5));
}

TEST_F(SnapshotSeriesDataTest, RaggedMessageDropsAbsentElementsNoVintageMixing) {
  SnapshotSeriesData series(
      &session_, topic_id_, dataset_id_, SnapshotSeriesData::XMode::kColumn, resolveX(), resolveY(3));

  // Land on the full message m=1 first (29 elements)...
  ASSERT_TRUE(series.refresh(1.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));

  // ...then move onto the ragged message m=2 (only 10 elements). The snapshot must
  // shrink to exactly those 10 — elements 10..28 are absent from THIS message and
  // must NOT retain m=1's values (no vintage mixing).
  ASSERT_TRUE(series.refresh(2.5));
  ASSERT_EQ(series.size(), 10U);
  for (std::size_t k = 0; k < 10; ++k) {
    EXPECT_DOUBLE_EQ(series.sample(k).x(), stampValue(static_cast<int>(k)));
    EXPECT_DOUBLE_EQ(series.sample(k).y(), posValue(2, static_cast<int>(k), 3));
  }
  // Nothing beyond element 9 leaked in from the earlier, longer message.
  EXPECT_TRUE(std::isnan(series.sample(10).y()));

  // Moving forward to the full message m=3 restores all 29.
  ASSERT_TRUE(series.refresh(3.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));
  EXPECT_DOUBLE_EQ(series.sample(28).y(), posValue(3, 28, 3));
}

TEST_F(SnapshotSeriesDataTest, IndexModeUsesElementIndexAsX) {
  SnapshotSeriesData series(
      &session_, topic_id_, dataset_id_, SnapshotSeriesData::XMode::kIndex, {}, resolveY(3));
  ASSERT_TRUE(series.refresh(0.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));
  for (std::size_t k = 0; k < static_cast<std::size_t>(kElements); ++k) {
    EXPECT_DOUBLE_EQ(series.sample(k).x(), static_cast<double>(k));
    EXPECT_DOUBLE_EQ(series.sample(k).y(), posValue(0, static_cast<int>(k), 3));
  }
}

TEST_F(SnapshotSeriesDataTest, StreamingAppendIsPickedUpByRefresh) {
  SnapshotSeriesData series(
      &session_, topic_id_, dataset_id_, SnapshotSeriesData::XMode::kColumn, resolveX(), resolveY(3));
  ASSERT_TRUE(series.refresh(3.5));  // latest so far is m=3
  EXPECT_DOUBLE_EQ(series.sample(0).y(), posValue(3, 0, 3));

  // A new message arrives on the topic (streaming). Refreshing at its time picks it
  // up — the same path samplesIngested drives in the widget.
  DataWriter writer = session_.dataEngine().createWriter();
  ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());
  writeMessage(writer, 4 * kNs, /*m=*/4, /*present=*/kElements);
  ASSERT_FALSE(session_.commitChunks(writer.flushAll()).empty());

  ASSERT_TRUE(series.refresh(4.5));
  ASSERT_EQ(series.size(), static_cast<std::size_t>(kElements));
  EXPECT_DOUBLE_EQ(series.sample(7).y(), posValue(4, 7, 3));
}

TEST_F(SnapshotSeriesDataTest, NoMessageBeforeTimeYieldsEmpty) {
  SnapshotSeriesData series(
      &session_, topic_id_, dataset_id_, SnapshotSeriesData::XMode::kColumn, resolveX(), resolveY(3));
  EXPECT_FALSE(series.refresh(-1.0));  // before the first message; nothing changes from empty
  EXPECT_EQ(series.size(), 0U);
}

}  // namespace
}  // namespace PJ
