// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
using namespace Qt::StringLiterals;

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

  EXPECT_EQ(curves[0].name, u"dataset:1/topic:%1/column:0"_s.arg(first_topic));
  EXPECT_EQ(curves[0].dataset_name, u"drive.mcap"_s);
  EXPECT_EQ(curves[0].topic_name, u"/imu/accel/sample"_s);
  EXPECT_EQ(curves[0].field_name, u"value"_s);
  EXPECT_EQ(curves[0].dataset_id, *first_dataset);
  EXPECT_EQ(curves[0].topic_id, first_topic);

  EXPECT_EQ(curves[1].name, u"dataset:2/topic:%1/column:0"_s.arg(second_topic));
  EXPECT_EQ(curves[1].dataset_name, u"drive.mcap (2)"_s);
  EXPECT_EQ(curves[1].topic_name, u"/imu/accel/sample"_s);
  EXPECT_EQ(curves[1].field_name, u"value"_s);
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

  EXPECT_EQ(items[0].key, u"dataset:%1/object_topic:%2"_s.arg(*first_dataset).arg(first_object_topic->id));
  EXPECT_EQ(items[0].dataset_name, u"drive.mcap"_s);
  EXPECT_EQ(items[0].topic_name, u"/camera/image"_s);
  EXPECT_EQ(items[0].dataset_id, *first_dataset);
  EXPECT_EQ(PJ::asObjectTopic(items[0])->object_topic_id, *first_object_topic);

  EXPECT_EQ(items[1].key, u"dataset:%1/object_topic:%2"_s.arg(*second_dataset).arg(second_object_topic->id));
  EXPECT_EQ(items[1].dataset_name, u"drive.mcap (2)"_s);
  EXPECT_EQ(items[1].topic_name, u"/camera/image"_s);
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

// Regression for the remove-then-reload bug (PR #131): removeDataset tombstones a DatasetId in the
// catalog, and an in-place reload of the same file deliberately REUSES that id (replaceDataset keeps
// DatasetId/TopicId stable so curve keys survive). The tombstone therefore outlives the swap, so the
// reloaded data stays invisible unless the caller (FileLoader) lifts it with restoreDataset. This locks
// that contract: a real replaceDataset reusing a removed id does NOT self-un-hide; restoreDataset does.
TEST(CatalogModelTest, ReloadReusingRemovedDatasetIdStaysHiddenUntilRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  ASSERT_EQ(catalog.items().size(), 1U);

  // Remove: the engine keeps the data (append-only), the catalog tombstones the id and hides it.
  ASSERT_TRUE(catalog.removeDataset(*dataset));
  EXPECT_TRUE(catalog.items().empty());

  // Reload: stage a fresh engine carrying the same topic, then run the real in-place replace that
  // keeps the primary DatasetId stable (what FileLoader does on a same-file reload).
  PJ::DataEngine staged_engine;
  PJ::ObjectStore staged_store;
  auto staged = staged_engine.createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(staged.has_value()) << staged.error();
  {
    PJ::DataWriter writer = staged_engine.createWriter();
    auto handle = writer.registerScalarSeries(*staged, "/imu/accel/sample", PJ::NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value()) << handle.error();
    writer.appendScalar(*handle, 200, 2.0);
    staged_engine.commitChunks(writer.flushAll());
  }
  session.replaceDataset(staged_engine, staged_store, *staged, *dataset, {});
  catalog.rebuildFromDatastore();

  // The bug: the swap reused the tombstoned id, so the rebuild still filters the dataset out.
  EXPECT_TRUE(catalog.items().empty())
      << "an in-place reload reusing a removed DatasetId must stay hidden until restoreDataset";

  // The fix recipe FileLoader applies on the replace path: lift the tombstone for the reused id.
  catalog.restoreDataset(*dataset);
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U) << "restoreDataset must surface the reloaded dataset";
  EXPECT_EQ(items[0].dataset_id, *dataset);
}

// --- setDatasetDisplayName (issue #98) --------------------------------------

TEST(CatalogModelTest, DisplayNameOverrideReplacesLabelButKeepsSourceName) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  ASSERT_EQ(catalog.curves().at(0).dataset_name, u"info.json"_s);

  catalog.setDatasetDisplayName(*dataset, u"pusht_v21"_s);

  EXPECT_EQ(catalog.curves().at(0).dataset_name, u"pusht_v21"_s);
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

  catalog.setDatasetDisplayName(*dataset, u"lerobot/pusht"_s);

  // '/' is flattened to '_' (same normalization as a source_name label) so the
  // override stays a single tree-root node.
  EXPECT_EQ(catalog.curves().at(0).dataset_name, u"lerobot_pusht"_s);
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

  catalog.setDatasetDisplayName(*first, u"pusht"_s);
  catalog.setDatasetDisplayName(*second, u"pusht"_s);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 2U);
  EXPECT_EQ(curves[0].dataset_name, u"pusht"_s);
  EXPECT_EQ(curves[1].dataset_name, u"pusht (2)"_s);
}

TEST(CatalogModelTest, DisplayNameOverrideSurvivesRebuild) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, u"pusht_v21"_s);

  // A second commit triggers rebuildFromDatastore via topicsCommitted.
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/gyro/sample"), 0U);

  for (const auto& curve : catalog.curves()) {
    EXPECT_EQ(curve.dataset_name, u"pusht_v21"_s);
  }
}

TEST(CatalogModelTest, DisplayNameOverrideSurvivesClearAndRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, u"pusht_v21"_s);

  catalog.clearAll();
  EXPECT_TRUE(catalog.items().empty());

  catalog.restoreDataset(*dataset);
  ASSERT_EQ(catalog.curves().size(), 1U);
  EXPECT_EQ(catalog.curves().at(0).dataset_name, u"pusht_v21"_s);
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
  catalog.setDatasetDisplayName(*dataset, u"pusht_v21"_s);

  QString added_dataset_name;
  QObject::connect(&catalog, &PJ::CatalogModel::itemAdded, [&](const PJ::CatalogItem& item) {
    added_dataset_name = item.dataset_name;
  });

  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);

  EXPECT_EQ(added_dataset_name, u"pusht_v21"_s);
}

TEST(CatalogModelTest, EmptyDisplayNameClearsOverride) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "info.json"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  catalog.setDatasetDisplayName(*dataset, u"pusht_v21"_s);
  ASSERT_EQ(catalog.curves().at(0).dataset_name, u"pusht_v21"_s);

  catalog.setDatasetDisplayName(*dataset, QString{});

  EXPECT_EQ(catalog.curves().at(0).dataset_name, u"info.json"_s);
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

TEST(CatalogModelTest, RemoveDatasetEmitsClearedWhenItEmptiesTheCatalog) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/sample"), 0U);
  ASSERT_NE(addScalarTopic(session, *dataset, "/gps/fix"), 0U);

  int cleared = 0;
  int items_removed_emissions = 0;
  QObject::connect(&catalog, &PJ::CatalogModel::cleared, [&cleared] { ++cleared; });
  QObject::connect(&catalog, &PJ::CatalogModel::itemsRemoved, [&items_removed_emissions](const QStringList&) {
    ++items_removed_emissions;
  });

  EXPECT_TRUE(catalog.removeDataset(*dataset));
  EXPECT_TRUE(catalog.items().empty());
  EXPECT_EQ(cleared, 1) << "emptying the catalog emits cleared() once (cheap view reset, avoids O(N^2))";
  EXPECT_EQ(items_removed_emissions, 0) << "no itemsRemoved churn when the whole catalog goes empty";
}

TEST(CatalogModelTest, RemoveDatasetEmitsOneBatchedItemsRemovedWhenOthersRemain) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/imu/accel/sample"), 0U);
  ASSERT_NE(addScalarTopic(session, *dataset_a, "/gps/fix"), 0U);
  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(session, *dataset_b, "/vehicle/speed"), 0U);

  int cleared = 0;
  int emissions = 0;
  int total_keys = 0;
  QObject::connect(&catalog, &PJ::CatalogModel::cleared, [&cleared] { ++cleared; });
  QObject::connect(&catalog, &PJ::CatalogModel::itemsRemoved, [&](const QStringList& keys) {
    ++emissions;
    total_keys += static_cast<int>(keys.size());
  });

  EXPECT_TRUE(catalog.removeDataset(*dataset_a));
  EXPECT_EQ(cleared, 0) << "dataset_b remains, so no cleared()";
  EXPECT_EQ(emissions, 1) << "one batched itemsRemoved, not one signal per key (no N replots downstream)";
  EXPECT_EQ(total_keys, 2) << "the batch carries both dropped keys";
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
  EXPECT_EQ(ds[0].second, u"a.mcap"_s);
  EXPECT_EQ(ds[1].first, *b);
  EXPECT_EQ(ds[1].second, u"b.mcap"_s);
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

  const auto da = catalog.descriptorForPath(*a, u"/vehicle/speed"_s, u"value"_s);
  const auto db = catalog.descriptorForPath(*b, u"/vehicle/speed"_s, u"value"_s);
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

  EXPECT_FALSE(catalog.descriptorForPath(*a, u"/no/such/topic"_s, u"value"_s).has_value());
  EXPECT_FALSE(catalog.descriptorForPath(*a, u"/vehicle/speed"_s, u"nope"_s).has_value());
}

// ===========================================================================
// In-place reload (DataEngine/ObjectStore::replaceDatasetFrom) keeps curve/object
// keys stable — the runtime guarantee that plots and 2D docks keep bindings.
// ===========================================================================

TEST(CatalogModelReloadTest, ScalarReplaceKeepsCurveKeyAndEmitsNoRemoval) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto primary = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(primary.has_value());
  const PJ::TopicId topic = addScalarTopic(session, *primary, "/imu/accel/sample");
  ASSERT_NE(topic, 0U);
  catalog.rebuildFromDatastore();

  const auto before = catalog.curves();
  ASSERT_EQ(before.size(), 1U);
  const QString key_before = before[0].name;

  // Stage a same-source reload into a throwaway engine, then swap it in.
  PJ::DataEngine staged;
  auto staged_ds = staged.createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(staged_ds.has_value());
  {
    PJ::DataWriter writer = staged.createWriter();
    auto handle = writer.registerScalarSeries(*staged_ds, "/imu/accel/sample", PJ::NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value());
    writer.appendScalar(*handle, 200, 42.0);
    staged.commitChunks(writer.flushAll());
  }

  int removed_emissions = 0;
  bool cleared_emitted = false;
  QObject::connect(&catalog, &PJ::CatalogModel::itemsRemoved, &catalog, [&removed_emissions](const QStringList&) {
    ++removed_emissions;
  });
  QObject::connect(&catalog, &PJ::CatalogModel::cleared, &catalog, [&cleared_emitted]() { cleared_emitted = true; });

  ASSERT_TRUE(session.dataEngine().replaceDatasetFrom(staged, *staged_ds, *primary).has_value());
  catalog.rebuildFromDatastore();

  const auto after = catalog.curves();
  ASSERT_EQ(after.size(), 1U);
  EXPECT_EQ(after[0].name, key_before) << "curve key must be stable across reload";
  EXPECT_EQ(after[0].dataset_id, *primary);
  EXPECT_EQ(after[0].topic_id, topic);
  EXPECT_EQ(removed_emissions, 0) << "a surviving curve must not be reported removed";
  EXPECT_FALSE(cleared_emitted);
}

TEST(CatalogModelReloadTest, ObjectReplaceKeepsObjectTopicKey) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto primary = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(primary.has_value());
  auto obj =
      session.objectStore().registerTopic({.dataset_id = *primary, .topic_name = "/cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(obj.has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*obj, 100, std::vector<uint8_t>(4, 0)).has_value());
  catalog.rebuildFromDatastore();

  auto object_key = [](const std::vector<PJ::CatalogItem>& items) -> QString {
    for (const auto& item : items) {
      if (PJ::isObjectTopic(item)) {
        return item.key;
      }
    }
    return {};
  };
  const QString key_before = object_key(catalog.items());
  ASSERT_FALSE(key_before.isEmpty());

  PJ::ObjectStore staged;
  auto staged_obj = staged.registerTopic({.dataset_id = 9, .topic_name = "/cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(staged_obj.has_value());
  ASSERT_TRUE(staged.pushOwned(*staged_obj, 200, std::vector<uint8_t>(4, 1)).has_value());

  int removed_emissions = 0;
  QObject::connect(&catalog, &PJ::CatalogModel::itemsRemoved, &catalog, [&removed_emissions](const QStringList&) {
    ++removed_emissions;
  });

  ASSERT_TRUE(session.objectStore().replaceDatasetFrom(staged, 9, *primary).has_value());
  catalog.rebuildFromDatastore();

  EXPECT_EQ(object_key(catalog.items()), key_before) << "object topic key (ObjectTopicId) must be stable across reload";
  EXPECT_EQ(removed_emissions, 0);
}

// samplesIngested is now gated on a content fingerprint (rebuildIfChanged) so
// row-only ingest batches skip the full rebuild. The gate must NEVER skip a real
// structural change — a false skip would drop a newly-ingested topic from the
// catalog. Each new topic arrives via the gated commit path (addScalarTopic
// commits, which emits samplesIngested), so this pins the anti-false-skip
// property end-to-end.
TEST(CatalogModelTest, IngestGateSurfacesEveryNewTopic) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "gate.mcap"});
  ASSERT_TRUE(dataset.has_value());

  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/x"), 0U);
  EXPECT_EQ(catalog.items().size(), 1U) << "first topic must surface through the gate";

  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/y"), 0U);
  EXPECT_EQ(catalog.items().size(), 2U) << "gate must not skip a structural change (new topic)";

  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel/z"), 0U);
  EXPECT_EQ(catalog.items().size(), 3U) << "every new topic must keep surfacing";
}

// scalarValueAt is the read seam behind the curve-list "Value" column: given a
// catalog key and a display-axis time, it returns the latest scalar sample at
// or before that time (zero-order hold) — or nullopt when the curve has no
// sample at/before that time, or the key is not a scalar curve.
TEST(CatalogModelTest, ScalarValueAtReturnsLatestSampleAtOrBeforeTime) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "values.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/imu/accel/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 1'000'000'000, 10.0);
  writer.appendScalar(*handle, 2'000'000'000, 20.0);
  writer.appendScalar(*handle, 3'000'000'000, 30.0);
  const auto committed = session.commitChunks(writer.flushAll());
  ASSERT_FALSE(committed.empty());

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);
  ASSERT_TRUE(PJ::isScalarField(items[0]));
  const QString key = items[0].key;

  // Convert raw stamps to the display-axis double the panel passes in, so the
  // assertions hold regardless of the dataset's display offset.
  const PJ::DisplayOffset offset = session.displayOffset(*dataset);
  auto displayOf = [&](PJ::Timestamp raw_ns) { return PJ::toAxisDouble(PJ::rawToDisplaySeconds(raw_ns, offset)); };

  // Between the 2nd and 3rd sample → holds the 2nd sample's value.
  auto mid = catalog.scalarValueAt(key, displayOf(2'500'000'000));
  ASSERT_TRUE(mid.has_value());
  EXPECT_DOUBLE_EQ(*mid, 20.0);

  // Exactly on the last sample → that sample.
  auto on_sample = catalog.scalarValueAt(key, displayOf(3'000'000'000));
  ASSERT_TRUE(on_sample.has_value());
  EXPECT_DOUBLE_EQ(*on_sample, 30.0);

  // After the last sample → still the last sample (zero-order hold forward).
  auto after = catalog.scalarValueAt(key, displayOf(9'000'000'000));
  ASSERT_TRUE(after.has_value());
  EXPECT_DOUBLE_EQ(*after, 30.0);

  // Before the first sample → no value.
  EXPECT_FALSE(catalog.scalarValueAt(key, displayOf(500'000'000)).has_value());

  // Unknown / non-scalar key → no value.
  EXPECT_FALSE(catalog.scalarValueAt(u"not-a-key"_s, displayOf(2'500'000'000)).has_value());
}

// String-typed fields surface in the catalog as scalar items (so the curve list
// shows them with their text value) but are excluded from plottable curves();
// stringValueAt reads the latest string at/before the cursor (zero-order hold).
TEST(CatalogModelTest, StringFieldIsCatalogedAsScalarReadableButNotPlottable) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "strings.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  // struct diag { float64 value; string frame_id } → col 0 numeric, col 1 string.
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto value = PJ::makePrimitive("value", PJ::PrimitiveType::kFloat64);
  auto frame_id = PJ::makePrimitive("frame_id", PJ::PrimitiveType::kString);
  auto schema = PJ::makeStruct("diag", {value, frame_id});
  auto schema_or = writer.registerSchema("diag", schema);
  ASSERT_TRUE(schema_or.has_value()) << schema_or.error();
  PJ::TopicDescriptor td;
  td.name = "/diagnostics";
  td.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(*dataset, td);
  ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
  const PJ::TopicId topic = *topic_or;

  const std::vector<std::string_view> frames = {"init", "running", "done"};
  for (std::size_t i = 0; i < frames.size(); ++i) {
    ASSERT_TRUE(writer.beginRow(topic, static_cast<PJ::Timestamp>((i + 1) * 1'000'000'000)).has_value());
    writer.set(topic, 0, static_cast<double>(i) * 10.0);
    writer.set(topic, 1, frames[i]);
    ASSERT_TRUE(writer.finishRow(topic).has_value());
  }
  const auto committed = session.commitChunks(writer.flushAll());
  ASSERT_FALSE(committed.empty());

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2U) << "both the numeric and the string field surface as catalog items";

  QString numeric_key;
  QString string_key;
  for (const auto& item : items) {
    const auto* scalar = PJ::asScalarField(item);
    ASSERT_NE(scalar, nullptr);
    (scalar->is_string ? string_key : numeric_key) = item.key;
  }
  ASSERT_FALSE(numeric_key.isEmpty());
  ASSERT_FALSE(string_key.isEmpty());

  // Only the numeric field is a plottable curve.
  EXPECT_EQ(catalog.curves().size(), 1U);
  EXPECT_TRUE(catalog.curveDescriptor(numeric_key).has_value());
  EXPECT_FALSE(catalog.curveDescriptor(string_key).has_value());

  // Key classification.
  EXPECT_TRUE(catalog.isScalarKey(numeric_key));
  EXPECT_TRUE(catalog.isScalarKey(string_key));
  EXPECT_FALSE(catalog.isStringKey(numeric_key));
  EXPECT_TRUE(catalog.isStringKey(string_key));

  const PJ::DisplayOffset offset = session.displayOffset(*dataset);
  auto displayOf = [&](PJ::Timestamp raw_ns) { return PJ::toAxisDouble(PJ::rawToDisplaySeconds(raw_ns, offset)); };

  // String zero-order hold: between the 2nd and 3rd sample → "running".
  auto mid = catalog.stringValueAt(string_key, displayOf(2'500'000'000));
  ASSERT_TRUE(mid.has_value());
  EXPECT_EQ(*mid, u"running"_s);
  // Before the first sample → no string.
  EXPECT_FALSE(catalog.stringValueAt(string_key, displayOf(500'000'000)).has_value());

  // Numeric and string reads stay on their own seams.
  auto numeric_value = catalog.scalarValueAt(numeric_key, displayOf(2'500'000'000));
  ASSERT_TRUE(numeric_value.has_value());
  EXPECT_DOUBLE_EQ(*numeric_value, 10.0);
  EXPECT_FALSE(catalog.scalarValueAt(string_key, displayOf(2'500'000'000)).has_value());
  EXPECT_FALSE(catalog.stringValueAt(numeric_key, displayOf(2'500'000'000)).has_value());
}

// --- Advertised (available-but-unsubscribed) placeholders (per-topic pause) ---

TEST(CatalogModelTest, AdvertisedTopicsAppearAsDataLessPlaceholders) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  catalog.setAdvertisedTopics(
      *dataset, {
                    {.topic_name = "/odom", .classification = PJ::sdk::BuiltinObjectType::kNone},
                    {.topic_name = "/camera/image", .classification = PJ::sdk::BuiltinObjectType::kImage},
                });

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2U);
  bool saw_scalar = false;
  bool saw_image = false;
  for (const auto& item : items) {
    EXPECT_TRUE(PJ::isAdvertisedTopic(item));
    EXPECT_FALSE(PJ::isScalarField(item));
    EXPECT_FALSE(PJ::isObjectTopic(item));
    // A data-less placeholder is not a plottable curve.
    EXPECT_FALSE(catalog.curveDescriptor(item.key).has_value());
    const auto* adv = PJ::asAdvertisedTopic(item);
    ASSERT_NE(adv, nullptr);
    if (item.topic_name == "/odom") {
      saw_scalar = true;
      EXPECT_EQ(adv->classification, PJ::sdk::BuiltinObjectType::kNone);
    } else if (item.topic_name == "/camera/image") {
      saw_image = true;
      EXPECT_EQ(adv->classification, PJ::sdk::BuiltinObjectType::kImage);
    }
  }
  EXPECT_TRUE(saw_scalar);
  EXPECT_TRUE(saw_image);
  EXPECT_TRUE(catalog.curves().empty());  // placeholders are never curves
}

TEST(CatalogModelTest, ScalarShapedPlaceholderIsScalarKeyButImageShapedIsNot) {
  // M3-UI: the curve-list Value column reads isScalarKey to choose "-" (scalar,
  // no sample yet) vs blank (non-scalar row) — a kNone-classified advertised
  // placeholder must count as scalar so it shows "-" like a real empty scalar
  // field, but an object-shaped placeholder (e.g. kImage) must not.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  catalog.setAdvertisedTopics(
      *dataset, {
                    {.topic_name = "/odom", .classification = PJ::sdk::BuiltinObjectType::kNone},
                    {.topic_name = "/camera/image", .classification = PJ::sdk::BuiltinObjectType::kImage},
                });

  QString scalar_key;
  QString image_key;
  for (const auto& item : catalog.items()) {
    (item.topic_name == "/odom" ? scalar_key : image_key) = item.key;
  }
  ASSERT_FALSE(scalar_key.isEmpty());
  ASSERT_FALSE(image_key.isEmpty());

  EXPECT_TRUE(catalog.isScalarKey(scalar_key));
  EXPECT_FALSE(catalog.isScalarKey(image_key));
  // No sample exists yet — scalarValueAt stays nullopt (the caller renders "-").
  EXPECT_FALSE(catalog.scalarValueAt(scalar_key, 0.0).has_value());
}

TEST(CatalogModelTest, RealDataSupersedesAdvertisedPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  catalog.setAdvertisedTopics(
      *dataset, {{.topic_name = "/imu/accel/sample", .classification = PJ::sdk::BuiltinObjectType::kNone}});
  ASSERT_EQ(catalog.items().size(), 1U);
  ASSERT_TRUE(PJ::isAdvertisedTopic(catalog.items()[0]));

  // Subscribe: real scalar data for the same topic name arrives.
  const PJ::TopicId topic = addScalarTopic(session, *dataset, "/imu/accel/sample");
  ASSERT_NE(topic, 0U);

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);
  EXPECT_TRUE(PJ::isScalarField(items[0]));  // placeholder replaced by the real field
  EXPECT_FALSE(PJ::isAdvertisedTopic(items[0]));
  EXPECT_EQ(items[0].topic_name, u"/imu/accel/sample"_s);
  EXPECT_TRUE(catalog.curveDescriptor(items[0].key).has_value());
}

TEST(CatalogModelTest, AdvertiseIsDeclarativeAndClearable) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  catalog.setAdvertisedTopics(
      *dataset, {
                    {.topic_name = "/a", .classification = PJ::sdk::BuiltinObjectType::kNone},
                    {.topic_name = "/b", .classification = PJ::sdk::BuiltinObjectType::kNone},
                });
  ASSERT_EQ(catalog.items().size(), 2U);

  // Declarative shrink: the new full set drops "/b".
  catalog.setAdvertisedTopics(*dataset, {{.topic_name = "/a", .classification = PJ::sdk::BuiltinObjectType::kNone}});
  ASSERT_EQ(catalog.items().size(), 1U);
  EXPECT_EQ(catalog.items()[0].topic_name, u"/a"_s);

  catalog.clearAdvertisedTopics(*dataset);
  EXPECT_TRUE(catalog.items().empty());
}

TEST(CatalogModelTest, ReAdvertisingIdenticalSetEmitsNothing) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  const std::vector<PJ::AdvertisedTopic> topics{
      {.topic_name = "/a", .classification = PJ::sdk::BuiltinObjectType::kNone}};
  catalog.setAdvertisedTopics(*dataset, topics);
  ASSERT_EQ(catalog.items().size(), 1U);

  QSignalSpy added_spy(&catalog, &PJ::CatalogModel::itemsAdded);
  QSignalSpy removed_spy(&catalog, &PJ::CatalogModel::itemsRemoved);
  catalog.setAdvertisedTopics(*dataset, topics);  // identical → idempotent, no churn
  EXPECT_EQ(added_spy.count(), 0);
  EXPECT_EQ(removed_spy.count(), 0);
}

TEST(CatalogModelTest, PerTopicPauseCapableDefaultsFalseAndRoundTrips) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  EXPECT_FALSE(catalog.isPerTopicPauseCapable(*dataset));  // unset → false (a file dataset, e.g.)
  catalog.setPerTopicPauseCapable(*dataset, true);
  EXPECT_TRUE(catalog.isPerTopicPauseCapable(*dataset));
  catalog.setPerTopicPauseCapable(*dataset, false);
  EXPECT_FALSE(catalog.isPerTopicPauseCapable(*dataset));
}

TEST(CatalogModelTest, PerTopicPauseCapableClearedOnDatasetRemoval) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "live"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  catalog.setAdvertisedTopics(*dataset, {{.topic_name = "/a", .classification = PJ::sdk::BuiltinObjectType::kNone}});
  catalog.setPerTopicPauseCapable(*dataset, true);
  ASSERT_TRUE(catalog.isPerTopicPauseCapable(*dataset));

  catalog.removeDataset(*dataset, /*tombstone=*/false);
  EXPECT_FALSE(catalog.isPerTopicPauseCapable(*dataset));
}

}  // namespace
