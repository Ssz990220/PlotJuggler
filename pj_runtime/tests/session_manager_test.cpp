// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <algorithm>
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

TEST(SessionManagerTimeTest, SetDisplayOffsetWritesDomainAndEmits) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  const auto domain = engine.createTimeDomain("src");
  ASSERT_TRUE(domain.has_value());
  const auto dataset = engine.createDataset(PJ::DatasetDescriptor{.source_name = "s.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());

  QSignalSpy spy(&session, qOverload<PJ::DatasetId>(&PJ::SessionManager::displayOffsetChanged));
  session.setDisplayOffset(*dataset, PJ::DisplayOffset{std::chrono::nanoseconds{500}});

  EXPECT_EQ(session.displayOffset(*dataset).value.count(), 500);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.front().front().value<PJ::DatasetId>(), *dataset);
}

TEST(SessionManagerTimeTest, SetDisplayOffsetIgnoresUnknownDatasetAndDefaultDomain) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  // Dataset bound to the default (id 0) domain: no-op + warning, no emit.
  const auto default_domain_dataset = engine.createDataset(PJ::DatasetDescriptor{.source_name = "plain.mcap"});
  ASSERT_TRUE(default_domain_dataset.has_value());

  QSignalSpy spy(&session, qOverload<PJ::DatasetId>(&PJ::SessionManager::displayOffsetChanged));
  session.setDisplayOffset(*default_domain_dataset, PJ::DisplayOffset{std::chrono::nanoseconds{500}});
  session.setDisplayOffset(9999, PJ::DisplayOffset{std::chrono::nanoseconds{500}});  // unknown dataset

  EXPECT_EQ(session.displayOffset(*default_domain_dataset).value.count(), 0);
  EXPECT_EQ(spy.count(), 0);
}

// Proves the mechanism the per-source TimeDomain loader change enables: each
// source gets its own domain, so an offset on one source cannot leak into
// another's displayOffset.
TEST(SessionManagerTimeTest, PerSourceDomainsIsolateOffsets) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  const auto td_a = engine.createTimeDomain("a");
  const auto td_b = engine.createTimeDomain("b");
  ASSERT_TRUE(td_a.has_value() && td_b.has_value());
  EXPECT_NE(*td_a, *td_b);  // distinct domains
  const auto ds_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "a", .time_domain_id = *td_a});
  const auto ds_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "b", .time_domain_id = *td_b});
  ASSERT_TRUE(ds_a.has_value() && ds_b.has_value());

  session.setDisplayOffset(static_cast<PJ::DatasetId>(*ds_a), PJ::DisplayOffset{std::chrono::nanoseconds{777}});
  EXPECT_EQ(session.displayOffset(static_cast<PJ::DatasetId>(*ds_a)).value.count(), 777);
  EXPECT_EQ(session.displayOffset(static_cast<PJ::DatasetId>(*ds_b)).value.count(), 0);  // unaffected
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

// Creates a dataset on its OWN TimeDomain (mirroring FileLoader's one-domain-per-
// source) and writes scalar samples. The per-source domain is required because
// "align starts" / Timeline offsets are written to the domain — a default (id 0)
// domain carries no shiftable offset.
PJ::DatasetId addDataset(
    PJ::SessionManager& session, std::string_view source, std::string_view topic,
    std::vector<PJ::Timestamp> timestamps) {
  auto domain = session.dataEngine().createTimeDomain(std::string(source));
  EXPECT_TRUE(domain.has_value()) << domain.error();
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = std::string(source), .time_domain_id = *domain});
  EXPECT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, topic, std::move(timestamps));
  return *dataset;
}

TEST(SessionManagerTimeOffsetTest, DefaultsOffWithZeroOffset) {
  PJ::SessionManager session;
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  EXPECT_FALSE(session.useTimeOffset());
  // Default: no auto-align. A data-bearing dataset shows at its natural absolute
  // time (offset 0) — NOT rebased to its own start. Regression guard for the bug
  // where "Use time offset" defaulted on and collapsed every dataset to zero.
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
}

// The Source Timeline edits the PER-SOURCE alignment offset (sourceDisplayOffset),
// which round-trips EXACTLY. The global "Use time offset" reference is layered on
// ONLY in displayOffset(): the two compose (alignment + global) instead of the old
// conflated model where a drag overwrote the global frame. So a drag while the
// frame is on no longer collapses onto the global term.
TEST(SessionManagerTimeOffsetTest, AlignmentOffsetIsSeparateFromGlobalFrame) {
  PJ::SessionManager session;
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});

  // Frame on: displayOffset subtracts the global earliest (5 s here); the per-source
  // alignment is untouched (still 0) so the Source Timeline bar stays put.
  session.setUseTimeOffset(true);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(session.globalTimeReference(), 5'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 5'000'000'000LL);

  // A Timeline drag writes the per-source alignment; it round-trips verbatim, and
  // displayOffset COMPOSES it with the global term (7e9 + 5e9), never overwrites.
  session.setDisplayOffset(a, PJ::DisplayOffset{std::chrono::nanoseconds{7'000'000'000LL}});
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 7'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 12'000'000'000LL);
}

// "Use time offset" subtracts ONE uniform global origin (the earliest sample across
// all datasets), so cross-dataset time gaps are PRESERVED — not a per-source rebase
// that collapses every dataset to zero. The per-source alignment offsets stay zero,
// so the Source Timeline's bars never move when the global frame is toggled.
TEST(SessionManagerTimeOffsetTest, UsesUniformGlobalOriginPreservingGaps) {
  PJ::SessionManager session;
  // Two datasets recorded 2 s apart.
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  const PJ::DatasetId b = addDataset(session, "b.mcap", "/b", {3'000'000'000LL, 4'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_TRUE(session.useTimeOffset());
  // Both datasets share the SAME global origin (b's 3 s, the global earliest); the
  // per-source alignment offsets are untouched (bars invariant to the toggle).
  EXPECT_EQ(session.globalTimeReference(), 3'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 3'000'000'000LL);
  EXPECT_EQ(session.displayOffset(b).value.count(), 3'000'000'000LL);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(session.sourceDisplayOffset(b).value.count(), 0);
  const auto range_a = session.datasetDisplayRange(a);
  const auto range_b = session.datasetDisplayRange(b);
  ASSERT_TRUE(range_a.has_value());
  ASSERT_TRUE(range_b.has_value());
  // b starts the axis at 0; a starts 2 s later — the real gap is PRESERVED (the old
  // per-source rebase would have put both at 0).
  EXPECT_DOUBLE_EQ(range_b->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range_a->min.value, 2.0);  // (5e9 - 3e9)/1e9
  EXPECT_DOUBLE_EQ(range_a->max.value, 6.0);  // (9e9 - 3e9)/1e9
  EXPECT_DOUBLE_EQ(range_b->max.value, 1.0);  // (4e9 - 3e9)/1e9

  session.setUseTimeOffset(false);
  EXPECT_FALSE(session.useTimeOffset());
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
  EXPECT_EQ(session.globalTimeReference(), 0);
  const auto range_a_off = session.datasetDisplayRange(a);
  ASSERT_TRUE(range_a_off.has_value());
  EXPECT_DOUBLE_EQ(range_a_off->min.value, 5.0);  // back to absolute epoch seconds
}

TEST(SessionManagerTimeOffsetTest, RetentionDoesNotMoveRelativeOffsetForward) {
  PJ::SessionManager session;
  auto dataset_or = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream"});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
  const PJ::DatasetId dataset = *dataset_or;

  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, "/stream/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  const PJ::ScalarSeriesHandle stream_series = *handle;

  const auto append_stream_samples = [&](std::vector<PJ::Timestamp> timestamps) {
    for (const PJ::Timestamp timestamp : timestamps) {
      writer.appendScalar(stream_series, timestamp, 1.0);
    }
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  };

  append_stream_samples({0, 5'000'000'000LL, 10'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0);

  const auto topics = session.dataEngine().listTopics(dataset);
  session.dataEngine().enforceRetention(5'000'000'000LL, dataset);
  session.notifyIngest(QVector<PJ::TopicId>(topics.begin(), topics.end()), /*live=*/true);

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0)
      << "streaming retention must not slide the relative-time origin forward";
  const auto retained_range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(retained_range.has_value());
  EXPECT_DOUBLE_EQ(retained_range->min.value, 5.0);
  EXPECT_DOUBLE_EQ(retained_range->max.value, 10.0);

  append_stream_samples({20'000'000'000LL});
  session.dataEngine().enforceRetention(5'000'000'000LL, dataset);
  session.notifyIngest(QVector<PJ::TopicId>(topics.begin(), topics.end()), /*live=*/true);

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0);
  const auto advanced_range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(advanced_range.has_value());
  EXPECT_DOUBLE_EQ(advanced_range->min.value, 20.0);
  EXPECT_DOUBLE_EQ(advanced_range->max.value, 20.0);
}

TEST(SessionManagerTimeOffsetTest, RefillInvalidatesPinnedRelativeOffset) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = addDataset(session, "reload", "/a", {5'000'000'000LL, 9'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(dataset).value.count(), 5'000'000'000LL);

  {
    PJ::RefillGuard guard = session.beginRefill(dataset);
    writeScalarSamples(session, dataset, "/a", {20'000'000'000LL, 25'000'000'000LL});
    guard.commit();
  }

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 20'000'000'000LL);
  const auto range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range->max.value, 5.0);
}

TEST(SessionManagerTimeOffsetTest, DisplayOffsetChangedEmittedOnFrameFlip) {
  PJ::SessionManager session;
  int changes = 0;
  QObject::connect(
      &session, qOverload<>(&PJ::SessionManager::displayOffsetChanged), &session, [&changes]() { ++changes; });

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

TEST(SessionManagerSignalTest, LiveNotifyEmitsEvenWithoutScalarTopicIds) {
  PJ::SessionManager session;
  int emissions = 0;
  bool saw_live = false;
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session, [&](const QVector<PJ::TopicId>& ids, bool live) {
        ++emissions;
        EXPECT_TRUE(ids.isEmpty());
        saw_live = live;
      });

  session.notifyIngest({}, /*live=*/false);
  EXPECT_EQ(emissions, 0);

  session.notifyIngest({}, /*live=*/true);
  EXPECT_EQ(emissions, 1);
  EXPECT_TRUE(saw_live);
}

// --- beginRefill / RefillGuard: in-place transactional reload prep ---
// (Migrated from the former SessionManagerClearRefillTest, which covered the deleted
// clearDatasetForRefill. The "writes back into the same topic id" case it also had is
// now covered by CommitKeepsRefilledDataAndFreesSnapshot below.)

TEST(SessionManagerRefillGuardTest, DetachEmitsAboutToBeReplacedBeforeEmptyIngest) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  writeScalarSamples(session, *ds, "/b", {150, 250});
  const auto topics_before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(topics_before.size(), 2u);
  ASSERT_TRUE(session.datasetDisplayRange(*ds).has_value());

  // Record the emission order of the two signals (direct, same-thread connections).
  std::vector<QString> order;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&order](PJ::DatasetId) {
    order.emplace_back(QStringLiteral("about"));
  });
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session, [&order](const QVector<PJ::TopicId>&, bool live) {
        order.emplace_back(live ? QStringLiteral("ingest_live") : QStringLiteral("ingest"));
      });

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // beginRefill's detach MUST emit datasetAboutToBeReplaced before the empty-state
    // (non-live) ingest notify — adapters drop cached TopicChunk* before the data moves.
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], QStringLiteral("about"));
    EXPECT_EQ(order[1], QStringLiteral("ingest"));

    // Dataset is now empty, but its topic ids stay registered (a refill reuses them).
    EXPECT_FALSE(session.datasetDisplayRange(*ds).has_value());
    EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before);
    guard.commit();  // keep the empty state (no refill here); avoid a rollback that would restore data
  }
}

// --- RefillGuard: the transactional in-place reload. Default outcome is ROLLBACK
//     (restore prior data); commit() keeps the refilled data. ---

TEST(SessionManagerRefillGuardTest, RollbackRestoresPriorScalarAndObjectData) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam", .metadata_json = "{}"});
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_topic, 250, std::vector<uint8_t>{1, 2, 3}).has_value());
  const auto scalar_topics_before = session.dataEngine().listTopics(*ds);
  const auto object_topics_before = session.objectStore().listTopics(*ds);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // In scope the dataset is detached (empty) but every id stays registered.
    EXPECT_FALSE(session.datasetDisplayRange(*ds).has_value());
    EXPECT_EQ(session.objectStore().entryCount(*object_topic), 0u);
    EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_topics_before);
    // guard dtor (no commit) rolls back here.
  }

  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "scalar data restored";
  EXPECT_EQ(session.objectStore().entryCount(*object_topic), 1u) << "object data restored";
  EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_topics_before) << "scalar ids stable";
  EXPECT_EQ(session.objectStore().listTopics(*ds), object_topics_before) << "object ids stable";
}

TEST(SessionManagerRefillGuardTest, CommitKeepsRefilledDataAndFreesSnapshot) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  const auto topics_before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(topics_before.size(), 1u);
  const PJ::TopicId topic = topics_before.front();

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    EXPECT_GT(guard.snapshotBytes(), 0u) << "prior data held aside";
    // Refill writes NEW samples into the SAME topic id.
    PJ::DataWriter writer = session.dataEngine().createWriter();
    const PJ::ScalarSeriesHandle handle{topic, 0};
    writer.appendScalar(handle, 1000, 1.0);
    writer.appendScalar(handle, 2000, 1.0);
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
    guard.commit();  // keep the refilled data
    EXPECT_EQ(guard.snapshotBytes(), 0u) << "commit frees the held-aside prior data";
  }

  EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before) << "id stable across commit";
  {
    // Only the refilled data remains (1000..2000) — commit froze the prior snapshot
    // rather than restoring it, and the refill did not double-count.
    auto lock = session.dataEngine().lockEngine();
    const PJ::TopicStorage* storage = session.dataEngine().getTopicStorage(topic);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->timeMin(), 1000);
    EXPECT_EQ(storage->timeMax(), 2000);
  }
}

TEST(SessionManagerRefillGuardTest, RollbackRetiresTopicsAddedByRefill) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  auto object_before = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam", .metadata_json = "{}"});
  ASSERT_TRUE(object_before.has_value()) << object_before.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_before, 150, std::vector<uint8_t>{1}).has_value());
  const auto scalar_before = session.dataEngine().listTopics(*ds);
  const auto object_before_ids = session.objectStore().listTopics(*ds);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The failed refill adds a NEW scalar topic AND a NEW object topic.
    writeScalarSamples(session, *ds, "/b", {500});
    auto added = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam2", .metadata_json = "{}"});
    ASSERT_TRUE(added.has_value()) << added.error();
    ASSERT_TRUE(session.objectStore().pushOwned(*added, 600, std::vector<uint8_t>{9}).has_value());
    // no commit -> rollback
  }

  EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_before) << "added scalar topic retired";
  EXPECT_EQ(session.objectStore().listTopics(*ds), object_before_ids) << "added object topic removed";
  EXPECT_EQ(session.objectStore().entryCount(*object_before), 1u) << "original object restored";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "original scalar restored";
}

TEST(SessionManagerRefillGuardTest, AboutToBeReplacedFiresOnConstructionAndRollback) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  int about_count = 0;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&about_count](PJ::DatasetId) {
    ++about_count;
  });
  {
    PJ::RefillGuard guard = session.beginRefill(*ds);  // fires once (detach)
    // no commit -> rollback fires once more (drop partial chunk pointers)
  }
  EXPECT_EQ(about_count, 2) << "fires on construction (detach) and on rollback";
}

TEST(SessionManagerRefillGuardTest, BeginRefillOnEmptyDatasetIsSafeNoop) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_NO_THROW({
    PJ::RefillGuard guard = session.beginRefill(*ds);
    EXPECT_EQ(guard.snapshotBytes(), 0u);
  });  // dtor rolls back an empty snapshot -> no-op
  EXPECT_NO_THROW({
    PJ::RefillGuard guard = session.beginRefill(*ds);
    guard.commit();
  });
}

TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsRetiresEmptyPriorScalarTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  writeScalarSamples(session, *ds, "/b", {150});
  writeScalarSamples(session, *ds, "/c", {300});
  const auto before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(before.size(), 3u);
  const PJ::TopicId topic_a = before[0];
  const PJ::TopicId topic_b = before[1];
  const PJ::TopicId topic_c = before[2];

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The reloaded file has only /a and /b; the refill writes back into those two,
    // never /c, so /c stays empty (vanished).
    PJ::DataWriter writer = session.dataEngine().createWriter();
    writer.appendScalar(PJ::ScalarSeriesHandle{topic_a, 0}, 1000, 1.0);
    writer.appendScalar(PJ::ScalarSeriesHandle{topic_b, 0}, 1500, 1.0);
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.dataEngine().listTopics(*ds);
  EXPECT_EQ(after.size(), 2u) << "the vanished topic /c is retired";
  EXPECT_NE(std::find(after.begin(), after.end(), topic_a), after.end()) << "/a kept (refilled)";
  EXPECT_NE(std::find(after.begin(), after.end(), topic_b), after.end()) << "/b kept (refilled)";
  EXPECT_EQ(std::find(after.begin(), after.end(), topic_c), after.end()) << "/c retired (vanished)";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value());
}

TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsRemovesEmptyPriorObjectTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  auto cam_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/a", .metadata_json = "{}"});
  auto cam_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/b", .metadata_json = "{}"});
  ASSERT_TRUE(cam_a.has_value()) << cam_a.error();
  ASSERT_TRUE(cam_b.has_value()) << cam_b.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*cam_a, 100, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*cam_b, 100, std::vector<uint8_t>{2}).has_value());
  ASSERT_EQ(session.objectStore().listTopics(*ds).size(), 2u);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The reloaded file has only /cam/a; the refill pushes to it, never /cam/b.
    ASSERT_TRUE(session.objectStore().pushOwned(*cam_a, 1000, std::vector<uint8_t>{3}).has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.objectStore().listTopics(*ds);
  ASSERT_EQ(after.size(), 1u) << "the vanished object topic /cam/b is removed";
  EXPECT_EQ(after.front(), *cam_a) << "/cam/a kept (refilled)";
}

TEST(SessionManagerRefillGuardTest, MovedGuardRollsBackExactlyOnce) {
  // The guard is move-only and lives in std::optional<RefillGuard> in FileLoader;
  // the move must neutralize the source (null session_ + mark committed) so the
  // moved-from dtor no-ops. A bug there would roll back TWICE.
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  const auto topics_before = session.dataEngine().listTopics(*ds);

  int about_count = 0;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&about_count](PJ::DatasetId) {
    ++about_count;
  });
  {
    PJ::RefillGuard original = session.beginRefill(*ds);  // detach fires datasetAboutToBeReplaced (1)
    PJ::RefillGuard moved = std::move(original);          // moved-from `original` must become inert
    // Scope exit destroys `moved` first (rollback -> fires (2)), then the moved-from
    // `original` (must no-op). A double rollback would push about_count to 3.
  }
  EXPECT_EQ(about_count, 2) << "exactly one rollback despite the move (moved-from guard is inert)";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "data restored once";
  EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before) << "ids stable";
}

}  // namespace
