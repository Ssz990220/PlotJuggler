#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "pj_app_core/SessionManager.h"
#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plot_widgets/DatastoreCurveAdapter.h"

namespace PJ {
namespace {

constexpr Timestamp kNs = 1000000000;
constexpr Timestamp kDisplayOffset = 2 * kNs;

class DatastoreCurveAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto domain_or = session_.dataEngine().createTimeDomain("test");
    ASSERT_TRUE(domain_or.has_value()) << domain_or.error();
    time_domain_id_ = *domain_or;
    session_.dataEngine().setDisplayOffset(time_domain_id_, kDisplayOffset);

    auto dataset_or = session_.dataEngine().createDataset(
        DatasetDescriptor{.source_name = "test", .time_domain_id = time_domain_id_});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = *dataset_or;

    DataWriter writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema("sample", makePrimitive("value", PrimitiveType::kFloat64));
    ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

    TopicDescriptor descriptor;
    descriptor.name = "/series";
    descriptor.schema_id = *schema_or;
    descriptor.max_chunk_rows = 4;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
    topic_id_ = *topic_or;

    auto handle_or = writer.bindTopicWriter(topic_id_);
    ASSERT_TRUE(handle_or.has_value()) << handle_or.error();

    appendRows(writer, topic_id_, 0, 10);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());

    descriptor_ = CurveDescriptor{
        .name = "/series/value",
        .topic_id = topic_id_,
        .dataset_id = dataset_id_,
        .column_index = 0,
        .field_path = "value",
        .display_offset_ns = kDisplayOffset,
    };
    adapter_ = std::make_unique<DatastoreCurveAdapter>(&session_, descriptor_);
  }

  void appendMoreRows(int first, int last_exclusive) {
    DataWriter writer = session_.dataEngine().createWriter();
    auto handle_or = writer.bindTopicWriter(topic_id_);
    ASSERT_TRUE(handle_or.has_value()) << handle_or.error();
    appendRows(writer, topic_id_, first, last_exclusive);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  static void appendRows(DataWriter& writer, TopicId topic_id, int first, int last_exclusive) {
    for (int i = first; i < last_exclusive; ++i) {
      ASSERT_TRUE(writer.beginRow(topic_id, static_cast<Timestamp>(i) * kNs).has_value());
      if (i == 5) {
        writer.setNull(topic_id, 0);
      } else {
        writer.set(topic_id, 0, 10.0 + static_cast<double>(i));
      }
      ASSERT_TRUE(writer.finishRow(topic_id).has_value());
    }
  }

  SessionManager session_;
  TimeDomainId time_domain_id_ = 0;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  CurveDescriptor descriptor_;
  std::unique_ptr<DatastoreCurveAdapter> adapter_;
};

TEST_F(DatastoreCurveAdapterTest, DefaultAllRowsWorksBeforeRectOfInterest) {
  ASSERT_EQ(adapter_->size(), 10U);

  const QPointF first = adapter_->sample(0);
  EXPECT_DOUBLE_EQ(first.x(), -2.0);
  EXPECT_DOUBLE_EQ(first.y(), 10.0);

  const QPointF last = adapter_->sample(9);
  EXPECT_DOUBLE_EQ(last.x(), 7.0);
  EXPECT_DOUBLE_EQ(last.y(), 19.0);
}

TEST_F(DatastoreCurveAdapterTest, RectOfInterestNarrowsAndAddsBoundaryGuards) {
  adapter_->setRectOfInterest(QRectF(QPointF(3.0, -1.0), QPointF(4.0, 1.0)));

  ASSERT_EQ(adapter_->size(), 4U);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).x(), 2.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 14.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(3).x(), 5.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(3).y(), 17.0);
}

TEST_F(DatastoreCurveAdapterTest, SampleLookupWorksAcrossChunksAndNonSequentialAccess) {
  EXPECT_DOUBLE_EQ(adapter_->sample(3).y(), 13.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(4).y(), 14.0);

  EXPECT_DOUBLE_EQ(adapter_->sample(9).y(), 19.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 10.0);
}

TEST_F(DatastoreCurveAdapterTest, NullSamplesReturnNan) {
  const QPointF null_sample = adapter_->sample(5);
  EXPECT_DOUBLE_EQ(null_sample.x(), 3.0);
  EXPECT_TRUE(std::isnan(null_sample.y()));
}

TEST_F(DatastoreCurveAdapterTest, DisplayOffsetShiftsSamplesAndFullBounds) {
  adapter_->setRectOfInterest(QRectF(QPointF(3.0, -1.0), QPointF(4.0, 1.0)));

  const QRectF bounds = adapter_->boundingRect();
  EXPECT_DOUBLE_EQ(bounds.left(), -2.0);
  EXPECT_DOUBLE_EQ(bounds.right(), 7.0);
  EXPECT_DOUBLE_EQ(bounds.top(), 10.0);
  EXPECT_DOUBLE_EQ(bounds.bottom(), 19.0);
}

TEST_F(DatastoreCurveAdapterTest, BoundsCacheInvalidatesOnTopicCommittedAndDataCleared) {
  const QRectF before = adapter_->boundingRect();
  EXPECT_DOUBLE_EQ(before.right(), 7.0);

  appendMoreRows(10, 13);
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().right(), 7.0);

  adapter_->onTopicCommitted();
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().right(), 10.0);
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().bottom(), 22.0);

  ASSERT_NE(session_.dataEngine().getTopicStorage(topic_id_), nullptr);
  session_.dataEngine().getTopicStorage(topic_id_)->clearChunks();
  adapter_->onDataCleared();
  EXPECT_EQ(adapter_->size(), 0U);
  EXPECT_FALSE(adapter_->boundingRect().isValid());
}

TEST_F(DatastoreCurveAdapterTest, VisibleYRangeUsesStatsForFullChunksAndScansPartialChunks) {
  const std::optional<std::pair<double, double>> full_chunk = adapter_->visibleYRange(2.0, 5.0);
  ASSERT_TRUE(full_chunk.has_value());
  EXPECT_DOUBLE_EQ(full_chunk->first, 14.0);
  EXPECT_DOUBLE_EQ(full_chunk->second, 17.0);

  const std::optional<std::pair<double, double>> partial = adapter_->visibleYRange(3.0, 4.0);
  ASSERT_TRUE(partial.has_value());
  EXPECT_DOUBLE_EQ(partial->first, 16.0);
  EXPECT_DOUBLE_EQ(partial->second, 16.0);
}

TEST_F(DatastoreCurveAdapterTest, TopicCommitGrowsIndexedSize) {
  ASSERT_EQ(adapter_->size(), 10U);

  appendMoreRows(10, 13);
  adapter_->onTopicCommitted();

  EXPECT_EQ(adapter_->size(), 13U);
  EXPECT_DOUBLE_EQ(adapter_->sample(12).x(), 10.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(12).y(), 22.0);
}

}  // namespace
}  // namespace PJ
