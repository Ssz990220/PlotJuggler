// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QString>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "pj_datastore/writer.hpp"
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

TEST(SessionManagerSourceTest, RecordLoadedSourceAppendsDistinctPaths) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QString());
  session.recordLoadedSource(QStringLiteral("/tmp/b.csv"), QStringLiteral("p"));
  // Both distinct files are tracked, in load order.
  const auto& sources = session.loadedSources();
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].path, QStringLiteral("/tmp/a.csv"));
  EXPECT_EQ(sources[1].path, QStringLiteral("/tmp/b.csv"));
  // lastLoadedSource() is the most recent.
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, QStringLiteral("/tmp/b.csv"));
  EXPECT_EQ(src->prefix, QStringLiteral("p"));
}

TEST(SessionManagerSourceTest, RecordLoadedSourceReplacesSamePathInPlace) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QString());
  session.recordLoadedSource(QStringLiteral("/tmp/b.csv"), QStringLiteral("p"));
  // Re-recording an existing path (a reload) updates it in place, keeping its
  // position and not growing the list.
  session.recordLoadedSource(
      QStringLiteral("/tmp/a.csv"), QStringLiteral("robot"), QStringLiteral("CSV"), QStringLiteral(R"({"x":1})"));
  const auto& sources = session.loadedSources();
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].path, QStringLiteral("/tmp/a.csv"));
  EXPECT_EQ(sources[0].prefix, QStringLiteral("robot"));
  EXPECT_EQ(sources[0].plugin_id, QStringLiteral("CSV"));
  EXPECT_EQ(sources[0].plugin_config_json, QStringLiteral(R"({"x":1})"));
  EXPECT_EQ(sources[1].path, QStringLiteral("/tmp/b.csv"));
}

TEST(SessionManagerSourceTest, ClearLoadedSourceResetsToEmpty) {
  PJ::SessionManager session;
  session.recordLoadedSource(QStringLiteral("/tmp/a.csv"), QString());
  session.recordLoadedSource(QStringLiteral("/tmp/b.csv"), QString());
  session.clearLoadedSource();
  EXPECT_FALSE(session.lastLoadedSource().has_value());
  EXPECT_TRUE(session.loadedSources().empty());
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

// --- datasetDisplayRange: the offset-aware range primitive shared by the
// streaming-playback seed (so its origin matches AppSession's file-load seed) ---

void writeScalarSamples(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic,
    std::vector<PJ::Timestamp> timestamps) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic, PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  for (const PJ::Timestamp timestamp : timestamps) {
    writer.appendScalar(*handle, timestamp, 1.0);
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeUnionsScalarAndObjectBoundsInDisplaySeconds) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, "/imu/x", {100, 200});

  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_topic, 900, std::vector<uint8_t>{1}).has_value());

  // Min from the scalar topic (100 ns), max from the object topic (900 ns); no
  // offset, so display seconds == raw / 1e9.
  const auto range = session.datasetDisplayRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 100.0e-9);
  EXPECT_DOUBLE_EQ(range->max.value, 900.0e-9);
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeAppliesDatasetDisplayOffset) {
  PJ::SessionManager session;
  // A dataset on a time domain shifted by +2 s (display_time = raw - 2e9).
  auto domain = session.dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});

  // Display seconds = (raw - 2e9)/1e9 -> [3, 7], matching the file-load seed
  // (AppSessionTest.SeedPlaybackUsesDisplayRelativeSecondsForShiftedDataset) and
  // NOT the offset-blind absolute [5, 9] the old streaming path produced.
  const auto range = session.datasetDisplayRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 3.0);
  EXPECT_DOUBLE_EQ(range->max.value, 7.0);
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeIsNulloptForEmptyDataset) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  EXPECT_FALSE(session.datasetDisplayRange(*dataset).has_value());
}

// --- "Use time offset": the per-dataset relative-time frame ---

// Creates a dataset (on the implicit default domain — no domain offset needed,
// the time-offset shift is computed from data) and writes scalar samples.
PJ::DatasetId addDataset(
    PJ::SessionManager& session, std::string_view source, std::string_view topic,
    std::vector<PJ::Timestamp> timestamps) {
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = std::string(source)});
  EXPECT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, topic, std::move(timestamps));
  return *dataset;
}

TEST(SessionManagerTimeOffsetTest, DefaultsOffWithZeroOffset) {
  PJ::SessionManager session;
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  EXPECT_FALSE(session.useTimeOffset());
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
}

TEST(SessionManagerTimeOffsetTest, RebasesEachDatasetToItsOwnStart) {
  PJ::SessionManager session;
  // Two datasets recorded at different epochs.
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  const PJ::DatasetId b = addDataset(session, "b.mcap", "/b", {3'000'000'000LL, 4'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_TRUE(session.useTimeOffset());
  // EACH dataset re-bases to ITS OWN earliest sample (not a shared global min),
  // so both start at display second 0 — their starts align.
  EXPECT_EQ(session.displayOffset(a).value.count(), 5'000'000'000LL);
  EXPECT_EQ(session.displayOffset(b).value.count(), 3'000'000'000LL);
  const auto range_a = session.datasetDisplayRange(a);
  const auto range_b = session.datasetDisplayRange(b);
  ASSERT_TRUE(range_a.has_value());
  ASSERT_TRUE(range_b.has_value());
  EXPECT_DOUBLE_EQ(range_a->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range_b->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range_a->max.value, 4.0);  // (9e9 - 5e9)/1e9
  EXPECT_DOUBLE_EQ(range_b->max.value, 1.0);  // (4e9 - 3e9)/1e9

  session.setUseTimeOffset(false);
  EXPECT_FALSE(session.useTimeOffset());
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
  const auto range_a_off = session.datasetDisplayRange(a);
  ASSERT_TRUE(range_a_off.has_value());
  EXPECT_DOUBLE_EQ(range_a_off->min.value, 5.0);  // back to absolute epoch seconds
}

TEST(SessionManagerTimeOffsetTest, DisplayOffsetChangedEmittedOnFrameFlip) {
  PJ::SessionManager session;
  int changes = 0;
  QObject::connect(&session, &PJ::SessionManager::displayOffsetChanged, &session, [&changes]() { ++changes; });

  session.setUseTimeOffset(true);  // off -> on : one change
  EXPECT_EQ(changes, 1);
  session.setUseTimeOffset(true);  // already on : no emit
  EXPECT_EQ(changes, 1);
  session.setUseTimeOffset(false);  // on -> off : one change
  EXPECT_EQ(changes, 2);
}

TEST(SessionManagerTimeOffsetTest, EmptyDatasetHasZeroOffsetWhenEnabled) {
  PJ::SessionManager session;
  auto empty = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(empty.has_value()) << empty.error();
  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(*empty).value.count(), 0);
}

}  // namespace
