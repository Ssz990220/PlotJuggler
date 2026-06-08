// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QString>
#include <chrono>

#include "pj_runtime/SessionManager.h"

namespace {

TEST(SessionManagerSourceTest, LastLoadedSourceStartsEmpty) {
  PJ::SessionManager session;
  EXPECT_FALSE(session.lastLoadedSource().has_value());
}

TEST(SessionManagerSourceTest, RecordLoadedSourceStoresPathAndPrefix) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/run42.csv"), QStringLiteral("robot"));
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, QStringLiteral("/tmp/run42.csv"));
  EXPECT_EQ(src->prefix, QStringLiteral("robot"));
}

TEST(SessionManagerSourceTest, RecordLoadedSourceOverwritesPrevious) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QString());
  session.recordLoadedSource(QStringLiteral("/tmp/b.csv"), QStringLiteral("p"));
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, QStringLiteral("/tmp/b.csv"));
  EXPECT_EQ(src->prefix, QStringLiteral("p"));
}

TEST(SessionManagerSourceTest, ClearLoadedSourceResetsToEmpty) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QString());
  session.clearLoadedSource();
  EXPECT_FALSE(session.lastLoadedSource().has_value());
}

TEST(SessionManagerSourceTest, RecordLoadedSourceStoresPluginIdAndConfig) {
  PJ::SessionManager session;
  session.recordLoadedSource(
      QStringLiteral("/tmp/run42.mcap"), QStringLiteral(""), QStringLiteral("DataLoad MCAP"),
      QStringLiteral(R"({"topics":["/imu"]})"));
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, QStringLiteral("/tmp/run42.mcap"));
  EXPECT_EQ(src->prefix, QString());
  EXPECT_EQ(src->plugin_id, QStringLiteral("DataLoad MCAP"));
  EXPECT_EQ(src->plugin_config_json, QStringLiteral(R"({"topics":["/imu"]})"));
}

TEST(SessionManagerSourceTest, RecordLoadedSourceDefaultsPluginFieldsToEmpty) {
  PJ::SessionManager session;
  // Old 2-arg shape — plugin fields default-empty.
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QStringLiteral("p"));
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, QStringLiteral("/tmp/a.csv"));
  EXPECT_EQ(src->prefix, QStringLiteral("p"));
  EXPECT_TRUE(src->plugin_id.isEmpty());
  EXPECT_TRUE(src->plugin_config_json.isEmpty());
}

TEST(SessionManagerSourceTest, ClearLoadedSourceResetsPluginFieldsToo) {
  PJ::SessionManager session;
  session.recordLoadedSource(
      QStringLiteral("/tmp/a.mcap"), QString(), QStringLiteral("DataLoad MCAP"), QStringLiteral(R"({"x":1})"));
  session.clearLoadedSource();
  EXPECT_FALSE(session.lastLoadedSource().has_value());
}

TEST(SessionManagerObjectsTest, EvictDatasetObjectsRemovesOnlyThatDatasetsTopics) {
  PJ::SessionManager session;
  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_a,
                          .topic_name = "/camera/a",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());
  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_b,
                          .topic_name = "/camera/b",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());

  session.evictDatasetObjects(*dataset_a);

  EXPECT_TRUE(session.objectStore().listTopics(*dataset_a).empty()) << "dataset a's objects must be evicted";
  EXPECT_EQ(session.objectStore().listTopics(*dataset_b).size(), 1U) << "unrelated dataset b must be untouched";
}

TEST(SessionManagerObjectsTest, ClearAllObjectsEvictsEveryTopic) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset,
                          .topic_name = "/camera/image",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());
  ASSERT_FALSE(session.objectStore().listTopics().empty());

  session.clearAllObjects();

  EXPECT_TRUE(session.objectStore().listTopics().empty());
}

TEST(SessionManagerObjectsTest, EvictObjectTopicsRemovesOnlySpecifiedTopics) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  auto topic_keep = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/keep",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(topic_keep.has_value());
  auto topic_drop = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/drop",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(topic_drop.has_value());
  ASSERT_EQ(session.objectStore().listTopics(*dataset).size(), 2U);

  // Trashing one object-topic entry evicts only that topic, sibling topics in the
  // same dataset survive (drives onCatalogTrashRequested).
  session.evictObjectTopics({*topic_drop});

  const auto remaining = session.objectStore().listTopics(*dataset);
  ASSERT_EQ(remaining.size(), 1U) << "sibling object topic in the same dataset must survive";
  EXPECT_EQ(remaining.front().id, topic_keep->id) << "the surviving topic must be the one not evicted";
}

TEST(SessionManagerTimeTest, DisplayOffsetReadsLiveTimeDomainShift) {
  PJ::SessionManager session;
  auto domain = session.dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value());
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());

  // Offset set AFTER createDataset: it must be read live, since the dataset's
  // snapshot of the domain would still report zero.
  session.dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  EXPECT_EQ(session.displayOffset(*dataset).value, std::chrono::nanoseconds{2'000'000'000LL});
}

TEST(SessionManagerTimeTest, DisplayOffsetIsZeroForDefaultDomainAndUnknownDataset) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "plain.mcap"});
  ASSERT_TRUE(dataset.has_value());
  EXPECT_EQ(session.displayOffset(*dataset).value, std::chrono::nanoseconds{0});  // default (id 0) domain
  EXPECT_EQ(session.displayOffset(9999).value, std::chrono::nanoseconds{0});      // unknown dataset
}

}  // namespace
