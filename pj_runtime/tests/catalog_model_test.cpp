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

TEST(CatalogModelTest, RestoreDatasetBringsBackClearedItems) {
  // Reproduces the layout-reload bug: a dataset hidden by clearAll
  // (which is what Clear All Curves calls) stays filtered out of every
  // future rebuildFromDatastore. restoreDataset() lifts the filter for
  // exactly one id so FileLoader can reuse an existing engine dataset
  // without un-hiding unrelated cleared datasets.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);

  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_b, "/gps/fix"), 0U);

  ASSERT_EQ(catalog.items().size(), 2U);

  catalog.clearAll();
  EXPECT_TRUE(catalog.items().empty());

  catalog.rebuildFromDatastore();
  EXPECT_TRUE(catalog.items().empty()) << "clearAll should mark both datasets as removed";

  catalog.restoreDataset(*dataset_a);
  const auto items_after_restore = catalog.items();
  ASSERT_EQ(items_after_restore.size(), 1U);
  EXPECT_EQ(items_after_restore[0].dataset_id, *dataset_a)
      << "restoreDataset must surface only the targeted dataset, not the other cleared one";
}

// --- setDatasetDisplayName (issue #98) --------------------------------------

TEST(CatalogModelTest, DisplayNameOverrideReplacesLabelButKeepsSourceName) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  ASSERT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("info.json"));

  catalog.setDatasetDisplayName(*dataset, QStringLiteral("pusht_v21"));

  EXPECT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("pusht_v21"));
  // The engine's source_name is left untouched so dataset-reuse matching
  // (FileLoader matches by source_name) keeps working.
  const PJ::DatasetInfo* info = session.dataEngine().getDataset(*dataset);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->source_name, std::string("info.json"));
}

TEST(CatalogModelTest, DisplayNameOverrideIsFlattened) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);

  catalog.setDatasetDisplayName(*dataset, QStringLiteral("lerobot/pusht"));

  // '/' is flattened to '_' (same normalization as a source_name label) so the
  // override stays a single tree-root node.
  EXPECT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("lerobot_pusht"));
}

TEST(CatalogModelTest, DisplayNameOverrideParticipatesInCollisionOrdinals) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto first = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.json"});
  ASSERT_TRUE(first.has_value());
  ASSERT_NE(addScalarTopic(session, *first, "/imu/accel/sample"), 0U);
  auto second = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.json"});
  ASSERT_TRUE(second.has_value());
  ASSERT_NE(addScalarTopic(session, *second, "/imu/accel/sample"), 0U);

  catalog.setDatasetDisplayName(*first, QStringLiteral("pusht"));
  catalog.setDatasetDisplayName(*second, QStringLiteral("pusht"));

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 2U);
  EXPECT_EQ(curves[0].dataset_name, QStringLiteral("pusht"));
  EXPECT_EQ(curves[1].dataset_name, QStringLiteral("pusht (2)"));
}

TEST(CatalogModelTest, DisplayNameOverrideSurvivesRebuild) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, QStringLiteral("pusht_v21"));

  // A second commit triggers rebuildFromDatastore via topicsCommitted.
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/gyro/sample"), 0U);

  for (const auto& curve : catalog.curves()) {
    EXPECT_EQ(curve.dataset_name, QStringLiteral("pusht_v21"));
  }
}

TEST(CatalogModelTest, DisplayNameOverrideSurvivesClearAndRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, QStringLiteral("pusht_v21"));

  catalog.clearAll();
  EXPECT_TRUE(catalog.items().empty());

  catalog.restoreDataset(*dataset);
  ASSERT_EQ(catalog.curves().size(), 1U);
  EXPECT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("pusht_v21"));
}

// Regression for the single-instance path: rebuildFromDatastore signals only
// add/remove keyed by curve identity (never a relabel), so the override must be
// in place BEFORE the topic is committed for its label to ride the itemAdded
// signal that incremental subscribers (the curve tree) consume. FileLoader
// relies on this by calling setDatasetDisplayName before start().
TEST(CatalogModelTest, DisplayNameOverrideSetBeforeCommitReachesItemAddedSignal) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  catalog.setDatasetDisplayName(*dataset, QStringLiteral("pusht_v21"));

  QString added_dataset_name;
  QObject::connect(&catalog, &PJ::CatalogModel::itemAdded, [&](const PJ::CatalogItem& item) {
    added_dataset_name = item.dataset_name;
  });

  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);

  EXPECT_EQ(added_dataset_name, QStringLiteral("pusht_v21"));
}

TEST(CatalogModelTest, EmptyDisplayNameClearsOverride) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, QStringLiteral("pusht_v21"));
  ASSERT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("pusht_v21"));

  catalog.setDatasetDisplayName(*dataset, QString{});

  EXPECT_EQ(catalog.curves().at(0).dataset_name, QStringLiteral("info.json"));
}

TEST(CatalogModelTest, RemoveDatasetHidesItemsScopedToTargetIdAndReturnsTrue) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);

  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_b, "/gps/fix"), 0U);

  ASSERT_EQ(catalog.items().size(), 2U);

  EXPECT_TRUE(catalog.removeDataset(*dataset_a));
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].dataset_id, *dataset_b) << "removeDataset must not touch unrelated datasets";
}

TEST(CatalogModelTest, RemoveDatasetTombstonePersistsAcrossRebuild) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);
  ASSERT_EQ(catalog.items().size(), 1U);

  EXPECT_TRUE(catalog.removeDataset(*dataset_a));
  EXPECT_TRUE(catalog.items().empty());

  catalog.rebuildFromDatastore();
  EXPECT_TRUE(catalog.items().empty())
      << "tombstone must outlive rebuildFromDatastore; otherwise the next commit resurrects the dataset";
}

TEST(CatalogModelTest, RemoveDatasetIsIdempotent) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);

  EXPECT_TRUE(catalog.removeDataset(*dataset_a));
  EXPECT_FALSE(catalog.removeDataset(*dataset_a)) << "second remove on the same id must be a no-op";
}

TEST(CatalogModelTest, RemoveDatasetEmitsClearedWhenEmptyingCatalog) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/gps/fix"), 0U);
  ASSERT_EQ(catalog.items().size(), 2U);

  int cleared_count = 0;
  int item_removed_count = 0;
  QObject::connect(&catalog, &PJ::CatalogModel::cleared, &catalog, [&]() { ++cleared_count; });
  QObject::connect(&catalog, &PJ::CatalogModel::itemRemoved, &catalog, [&](const QString&) { ++item_removed_count; });

  EXPECT_TRUE(catalog.removeDataset(*dataset_a));
  EXPECT_EQ(cleared_count, 1) << "must emit a single cleared() when the wipe empties the catalog";
  EXPECT_EQ(item_removed_count, 0)
      << "must not also emit per-item itemRemoved — that's the O(N^2) regression we're guarding against";
}

TEST(CatalogModelPathResolve, DatasetsEnumeratesLoadedDatasetsInLoadOrder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(a.has_value());
  ASSERT_NE(addScalarTopic(session, *a, "/imu/accel/sample"), 0U);
  auto b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(b.has_value());
  ASSERT_NE(addScalarTopic(session, *b, "/gps/fix"), 0U);

  const auto ds = catalog.datasets();
  ASSERT_EQ(ds.size(), 2U);
  EXPECT_EQ(ds[0].first, *a);
  EXPECT_EQ(ds[0].second, QStringLiteral("a.mcap"));
  EXPECT_EQ(ds[1].first, *b);
  EXPECT_EQ(ds[1].second, QStringLiteral("b.mcap"));
}

TEST(CatalogModelPathResolve, SameTopicFieldResolvesPerDatasetToDistinctKeys) {
  // The core generic-reuse guarantee: two similar recordings share an
  // identical topic+field path but live under different per-load keys.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "nissan_1.mcap"});
  ASSERT_TRUE(a.has_value());
  ASSERT_NE(addScalarTopic(session, *a, "/vehicle/speed"), 0U);
  auto b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "nissan_2.mcap"});
  ASSERT_TRUE(b.has_value());
  ASSERT_NE(addScalarTopic(session, *b, "/vehicle/speed"), 0U);

  const auto da = catalog.descriptorForPath(*a, QStringLiteral("/vehicle/speed"), QStringLiteral("value"));
  const auto db = catalog.descriptorForPath(*b, QStringLiteral("/vehicle/speed"), QStringLiteral("value"));
  ASSERT_TRUE(da.has_value());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(da->dataset_id, *a);
  EXPECT_EQ(db->dataset_id, *b);
  EXPECT_NE(da->name, db->name);  // distinct opaque keys, same stable path
}

TEST(CatalogModelPathResolve, ReturnsNulloptForAbsentTopicOrField) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(a.has_value());
  ASSERT_NE(addScalarTopic(session, *a, "/vehicle/speed"), 0U);

  EXPECT_FALSE(catalog.descriptorForPath(*a, QStringLiteral("/no/such/topic"), QStringLiteral("value")).has_value());
  EXPECT_FALSE(catalog.descriptorForPath(*a, QStringLiteral("/vehicle/speed"), QStringLiteral("nope")).has_value());
}

}  // namespace
