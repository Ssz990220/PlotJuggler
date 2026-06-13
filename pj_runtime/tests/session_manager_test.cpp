// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QString>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
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

// Trivial parser whose only job is to exist (vt_/ctx_ non-null) so the slot is
// reported valid by SessionManager. The concurrency canary never calls parse;
// it only races (re-)registration against per-tick binding reads.
class NoopParser : public PJ::MessageParserPluginBase {};

std::unique_ptr<PJ::MessageParserHandle> makeNoopHandle() {
  static constexpr const char* kManifest =
      R"({"id":"noop-parser","name":"Noop Parser","version":"1.0.0","encoding":["mock"]})";
  // One static vtable per CreateFn instantiation; the create fn allocates a fresh
  // NoopParser each call, so every handle owns a distinct instance.
  auto handle = std::make_unique<PJ::MessageParserHandle>(
      PJ::MessageParserPluginBase::vtableWithCreate([]() noexcept -> void* { return new NoopParser; }, kManifest));
  EXPECT_TRUE(handle->valid());
  return handle;
}

// Crash / TSan canary for the parser-slot race (H.12): the streaming worker
// re-registers fresh handles for the same ObjectTopicId in a tight loop while
// the GUI thread resolves the per-use binding and touches the parser through the
// keepalive. Deterministic (fixed iteration count, no sleeps) so it reproduces
// the unsynchronized-map UB and the replace-frees-live-parser hazard reliably
// under a sanitizer; a plain run just exercises the lock discipline.
TEST(SessionManagerParserRaceTest, ConcurrentRegisterAndBindIsSafe) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic{42};

  // Seed one parser so the reader sees a binding immediately.
  session.registerObjectTopicParser(topic, makeNoopHandle());

  constexpr int kIterations = 5000;
  std::atomic<bool> writer_done{false};

  std::thread writer([&] {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      // Each registration overwrites the slot, dropping the previous handle —
      // exactly the cross-thread replacement that used to free a parser out from
      // under a reader holding only a raw pointer.
      session.registerObjectTopicParser(topic, makeNoopHandle());
    }
    writer_done.store(true, std::memory_order_release);
  });

  // Reader loop: take a per-use snapshot, and when truthy, dereference the parser
  // through the binding's keepalive — the keepalive is what must keep a replaced
  // parser alive for the duration of this access.
  std::size_t valid_bindings = 0;
  std::size_t manifest_bytes = 0;  // sink so the manifest read is not optimized away
  while (!writer_done.load(std::memory_order_acquire)) {
    PJ::SessionManager::ParserBinding binding = session.parserBindingForObjectTopic(topic);
    if (binding) {
      // keepalive holds the handle (and DSO) mapped; reading manifest() exercises
      // the parser instance the snapshot named, even if the writer just replaced
      // the slot.
      const auto* handle = static_cast<const PJ::MessageParserHandle*>(binding.keepalive.get());
      manifest_bytes += handle->manifest().size();
      ++valid_bindings;
    }
  }
  writer.join();
  EXPECT_GT(manifest_bytes, 0U);  // keeps the keepalive-deref in the binary

  // Drain any in-flight replacement, then assert a final binding is valid.
  PJ::SessionManager::ParserBinding final_binding = session.parserBindingForObjectTopic(topic);
  EXPECT_TRUE(static_cast<bool>(final_binding)) << "topic must still have a valid parser after the race";
  EXPECT_GT(valid_bindings, 0U) << "reader never observed a valid binding";
}

}  // namespace
