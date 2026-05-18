#include <gtest/gtest.h>

#include <QString>
#include <string>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }

  writer.appendScalar(*handle_or, 100, 1.0);
  const auto committed_topics = session.commitChunks(writer.flushAll());
  EXPECT_FALSE(committed_topics.empty());
  return handle_or->topic_id;
}

TEST(CatalogModelTest, KeepsDuplicateDatasetTopicsVisibleUnderDatasetRoot) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto first_dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  const PJ::TopicId first_topic = addScalarTopic(session, *first_dataset, "/imu/accel/sample");
  ASSERT_NE(first_topic, 0U);

  auto second_dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  const PJ::TopicId second_topic = addScalarTopic(session, *second_dataset, "/imu/accel/sample");
  ASSERT_NE(second_topic, 0U);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 2U);
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2U);
  EXPECT_TRUE(PJ::isScalarField(items[0]));
  EXPECT_TRUE(PJ::isScalarField(items[1]));
  EXPECT_FALSE(PJ::isObjectTopic(items[0]));
  EXPECT_FALSE(PJ::isObjectTopic(items[1]));

  EXPECT_EQ(curves[0].name, QStringLiteral("dataset:1/topic:%1/column:0").arg(first_topic));
  EXPECT_EQ(curves[0].dataset_name, QStringLiteral("drive.mcap"));
  EXPECT_EQ(curves[0].topic_name, QStringLiteral("/imu/accel/sample"));
  EXPECT_EQ(curves[0].field_name, QStringLiteral("value"));
  EXPECT_EQ(curves[0].dataset_id, *first_dataset);
  EXPECT_EQ(curves[0].topic_id, first_topic);

  EXPECT_EQ(curves[1].name, QStringLiteral("dataset:2/topic:%1/column:0").arg(second_topic));
  EXPECT_EQ(curves[1].dataset_name, QStringLiteral("drive.mcap (2)"));
  EXPECT_EQ(curves[1].topic_name, QStringLiteral("/imu/accel/sample"));
  EXPECT_EQ(curves[1].field_name, QStringLiteral("value"));
  EXPECT_EQ(curves[1].dataset_id, *second_dataset);
  EXPECT_EQ(curves[1].topic_id, second_topic);

  const auto first_descriptor = catalog.curveDescriptor(curves[0].name);
  ASSERT_TRUE(first_descriptor.has_value());
  EXPECT_EQ(first_descriptor->topic_id, first_topic);

  const auto second_descriptor = catalog.curveDescriptor(curves[1].name);
  ASSERT_TRUE(second_descriptor.has_value());
  EXPECT_EQ(second_descriptor->topic_id, second_topic);
}

TEST(CatalogModelTest, KeepsDuplicateDatasetObjectTopicsVisibleUnderDatasetRoot) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto first_dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  auto first_object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *first_dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(first_object_topic.has_value()) << first_object_topic.error();

  auto second_dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  auto second_object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *second_dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(second_object_topic.has_value()) << second_object_topic.error();

  catalog.rebuildFromDatastore();

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2U);
  EXPECT_TRUE(catalog.curves().empty());
  ASSERT_TRUE(PJ::isObjectTopic(items[0]));
  ASSERT_TRUE(PJ::isObjectTopic(items[1]));
  EXPECT_EQ(PJ::asObjectTopic(items[0])->object_type, PJ::sdk::BuiltinObjectType::kImage);
  EXPECT_EQ(PJ::asObjectTopic(items[1])->object_type, PJ::sdk::BuiltinObjectType::kImage);

  EXPECT_EQ(items[0].key, QStringLiteral("dataset:%1/object_topic:%2").arg(*first_dataset).arg(first_object_topic->id));
  EXPECT_EQ(items[0].dataset_name, QStringLiteral("drive.mcap"));
  EXPECT_EQ(items[0].topic_name, QStringLiteral("/camera/image"));
  EXPECT_EQ(items[0].dataset_id, *first_dataset);
  EXPECT_EQ(PJ::asObjectTopic(items[0])->object_topic_id, *first_object_topic);

  EXPECT_EQ(
      items[1].key, QStringLiteral("dataset:%1/object_topic:%2").arg(*second_dataset).arg(second_object_topic->id));
  EXPECT_EQ(items[1].dataset_name, QStringLiteral("drive.mcap (2)"));
  EXPECT_EQ(items[1].topic_name, QStringLiteral("/camera/image"));
  EXPECT_EQ(items[1].dataset_id, *second_dataset);
  EXPECT_EQ(PJ::asObjectTopic(items[1])->object_topic_id, *second_object_topic);

  const auto first_descriptor = catalog.itemDescriptor(items[0].key);
  ASSERT_TRUE(first_descriptor.has_value());
  ASSERT_TRUE(PJ::isObjectTopic(*first_descriptor));
  EXPECT_EQ(PJ::asObjectTopic(*first_descriptor)->object_topic_id, *first_object_topic);
  EXPECT_FALSE(catalog.curveDescriptor(items[0].key).has_value());

  const auto second_descriptor = catalog.itemDescriptor(items[1].key);
  ASSERT_TRUE(second_descriptor.has_value());
  ASSERT_TRUE(PJ::isObjectTopic(*second_descriptor));
  EXPECT_EQ(PJ::asObjectTopic(*second_descriptor)->object_topic_id, *second_object_topic);
  EXPECT_FALSE(catalog.curveDescriptor(items[1].key).has_value());
}

TEST(CatalogModelTest, RemovedObjectTopicStaysHiddenAcrossRebuild) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();

  catalog.rebuildFromDatastore();
  const auto initial_items = catalog.items();
  ASSERT_EQ(initial_items.size(), 1U);
  ASSERT_TRUE(PJ::isObjectTopic(initial_items[0]));

  catalog.removeItems({initial_items[0].key});
  EXPECT_TRUE(catalog.items().empty());

  catalog.rebuildFromDatastore();
  EXPECT_TRUE(catalog.items().empty());
}

}  // namespace
