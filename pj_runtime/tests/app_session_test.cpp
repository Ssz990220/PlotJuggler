// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"

namespace {

void addScalarSamples(
    PJ::AppSession& session, PJ::DatasetId dataset_id, std::string_view topic, std::vector<PJ::Timestamp> timestamps) {
  PJ::DataWriter writer = session.sessionManager().dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic, PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle_or.has_value()) << handle_or.error();
  for (PJ::Timestamp timestamp : timestamps) {
    writer.appendScalar(*handle_or, timestamp, 1.0);
  }
  const auto changed_topics = session.sessionManager().commitChunks(writer.flushAll());
  ASSERT_FALSE(changed_topics.empty());
}

TEST(AppSessionTest, CustomExtensionDirectoryReachesMarketplaceManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  PJ::AppSession session(dir.path());

  EXPECT_EQ(session.extensionCatalog().extensionsDir(), dir.path());
  EXPECT_EQ(session.extensionCatalog().extensionManager().extensionsDir(), dir.path());
}

TEST(AppSessionTest, InvalidExtensionDirectoryReportsDiagnostic) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString file_path = dir.filePath("not-a-directory");
  QFile file(file_path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  std::vector<PJ::Diagnostic> diagnostics;
  PJ::AppSession session(
      file_path, [&diagnostics](const PJ::Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto is_error = [](const PJ::Diagnostic& diagnostic) {
    return diagnostic.level == PJ::DiagnosticLevel::kError;
  };
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), is_error));
}

TEST(AppSessionTest, SeedPlaybackUsesScalarAndObjectTimeBounds) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});

  auto object_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(
      session.sessionManager().objectStore().pushOwned(*object_topic, 900, std::vector<uint8_t>{1}).has_value());

  // Seeding reads the VISIBLE catalog (production rebuilds before seeding).
  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 900.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 100.0e-9);
}

TEST(AppSessionTest, SeedPlaybackUsesDisplayRelativeSecondsForShiftedDataset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // A dataset on a time domain shifted by +2 s (display_time = raw - 2e9).
  auto domain = session.sessionManager().dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.sessionManager().dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});

  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  // Display seconds = (raw - 2e9)/1e9 -> [3, 7], NOT the absolute [5, 9] the old
  // offset-blind seeding produced.
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 3.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 7.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 3.0);
}

TEST(AppSessionTest, SubsequentSeedPreservesCurrentTimeWhenNewRangeIsSubset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto first_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "first.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  addScalarSamples(session, *first_dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{150.0e-9});

  auto second_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "second.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  addScalarSamples(session, *second_dataset, "/imu/x", {120, 180});

  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 200.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 150.0e-9);
}

// The DataEngine keeps a removed dataset's scalars (append-only tombstone),
// but the playback timeline must track the VISIBLE catalog: once dataset A is
// removed, loading dataset B must yield B's range, not A∪B.
TEST(AppSessionTest, RemovedDatasetStopsContributingToPlaybackRange) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto removed_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "old.mcap"});
  ASSERT_TRUE(removed_dataset.has_value()) << removed_dataset.error();
  addScalarSamples(session, *removed_dataset, "/imu/x", {100, 10'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  ASSERT_TRUE(session.catalogModel().removeDataset(*removed_dataset));

  auto loaded_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "new.mcap"});
  ASSERT_TRUE(loaded_dataset.has_value()) << loaded_dataset.error();
  addScalarSamples(session, *loaded_dataset, "/imu/x", {1'000, 2'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 1'000.0e-9)
      << "removed dataset still stretches the timeline start";
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 2'000.0e-9)
      << "removed dataset still stretches the timeline end";
}

// Visibility is per-CURVE, not just per-dataset: trashing all of a topic's
// curves must drop that topic's bounds from the playback range even while
// sibling topics keep the dataset itself visible.
TEST(AppSessionTest, TrashedCurvesStopContributingToPlaybackRange) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});
  addScalarSamples(session, *dataset, "/gps/fix", {100, 1'000'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 1'000'000.0e-9);

  // Trash the long topic's curves (the dataset stays visible through /imu/x).
  std::vector<QString> trashed_keys;
  for (const PJ::CatalogItem& item : session.catalogModel().items()) {
    if (item.topic_name == QStringLiteral("/gps/fix")) {
      trashed_keys.push_back(item.key);
    }
  }
  ASSERT_FALSE(trashed_keys.empty());
  session.catalogModel().removeItems(trashed_keys);

  ASSERT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 200.0e-9)
      << "trashed topic's bounds must stop stretching the timeline";
}

// After a full catalog clear, the next seed behaves like a first load: range
// snaps to the new data only (no union with the cleared bounds) and the
// playhead snaps to the new start (no stale position carried over).
TEST(AppSessionTest, ClearAllThenSeedSnapsPlaybackToNewData) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto first_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "first.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  addScalarSamples(session, *first_dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{150.0e-9});

  session.catalogModel().clearAll();

  auto second_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "second.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  addScalarSamples(session, *second_dataset, "/imu/x", {10, 90});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 10.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 90.0e-9)
      << "cleared dataset's bounds must not survive into the new range";
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 10.0e-9)
      << "playhead must snap to the new data's start after a full clear";
}

TEST(AppSessionTest, ClearingCatalogForgetsRememberedCurveColors) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Load data so the catalog is non-empty (clearAll() is a no-op, and emits
  // nothing, on an already-empty catalog).
  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();

  // A remembered curve color...
  session.curveColorRegistry().setColor(QStringLiteral("/imu/x"), QStringLiteral("#1f77b4"));
  ASSERT_TRUE(session.curveColorRegistry().color(QStringLiteral("/imu/x")).has_value());

  // ...is forgotten when the catalog is cleared (data replaced), via the
  // AppSession wiring of CatalogModel::cleared -> CurveColorRegistry::clear.
  session.catalogModel().clearAll();

  EXPECT_FALSE(session.curveColorRegistry().color(QStringLiteral("/imu/x")).has_value());
}

TEST(AppSessionTest, DatasetRawTimeRangeReturnsRawBounds) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // One dataset spanning raw [1000, 9000] ns across two scalar topics.
  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {1'000, 5'000});
  addScalarSamples(session, *dataset, "/gps/fix", {3'000, 9'000});
  session.catalogModel().rebuildFromDatastore();

  const auto range = session.datasetRawTimeRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 1'000);  // RAW (pre-offset) union min
  EXPECT_EQ(range->max, 9'000);  // RAW (pre-offset) union max

  EXPECT_FALSE(session.datasetRawTimeRange(99999).has_value());  // unknown dataset
}

// datasetRawTimeRange is RAW (pre-offset): a display offset on the dataset's
// domain must not shift the reported bounds (the caller applies the offset).
TEST(AppSessionTest, DatasetRawTimeRangeIgnoresDisplayOffset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto domain = session.sessionManager().dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.sessionManager().dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  const auto range = session.datasetRawTimeRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 5'000'000'000LL);  // raw, NOT display (raw - 2e9)
  EXPECT_EQ(range->max, 9'000'000'000LL);
}

TEST(AppSessionTest, CurveColorRegistryIsOwnedBySessionManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // The registry is owned by SessionManager; AppSession's accessor delegates to
  // it. Plot widgets reach the same instance through their SessionManager
  // pointer, so it never has to be threaded through their constructors.
  EXPECT_EQ(&session.curveColorRegistry(), &session.sessionManager().curveColorRegistry());
}

namespace {

// Create a dataset on its own time domain shifted by `display_offset_ns`
// (display_time = raw - offset) and write one scalar topic into it.
PJ::DatasetId makeShiftedDataset(
    PJ::AppSession& session, const std::string& name, PJ::Timestamp display_offset_ns, const std::string& topic,
    std::vector<PJ::Timestamp> timestamps) {
  auto domain = session.sessionManager().dataEngine().createTimeDomain(name + "_domain");
  EXPECT_TRUE(domain.has_value());
  session.sessionManager().dataEngine().setDisplayOffset(*domain, display_offset_ns);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = name, .time_domain_id = *domain});
  EXPECT_TRUE(dataset.has_value());
  addScalarSamples(session, *dataset, topic, std::move(timestamps));
  return *dataset;
}

std::size_t mergedSampleCount(PJ::AppSession& session, PJ::DatasetId dataset, const std::string& topic) {
  for (const PJ::TopicId tid : session.sessionManager().dataEngine().listTopics(dataset)) {
    const auto* st = session.sessionManager().dataEngine().getTopicStorage(tid);
    if (st != nullptr && st->descriptor().name == topic) {
      auto series = session.sessionManager().createReader().series(tid, 0);
      return series.has_value() ? series->size() : 0;
    }
  }
  return 0;
}

}  // namespace

// A bulk import (cloud fetch) FOCUSES playback: the range snaps to the new
// dataset's bounds even when an older dataset spans a much wider window — a
// 10s snippet must present a 10s timeline, not drown in the union.
TEST(AppSessionTest, FocusPlaybackSnapsRangeToTheGivenDatasets) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto old_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "old-wide.mcap"});
  ASSERT_TRUE(old_dataset.has_value()) << old_dataset.error();
  addScalarSamples(session, *old_dataset, "/imu/x", {1'000, 1'000'000'000});
  ASSERT_TRUE(session.seedPlaybackFromSession());

  auto snippet =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "snippet.mcap"});
  ASSERT_TRUE(snippet.has_value()) << snippet.error();
  addScalarSamples(session, *snippet, "/odom/x", {500'000, 600'000});

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*snippet}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 500'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 600'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 500'000.0e-9);
}

// Latched/static objects (tf_static, stale markers) carry payload-embedded
// stamps far OUTSIDE an import's window: scalar series alone bound the focused
// range; objects define it only when the import has no scalar data at all.
TEST(AppSessionTest, FocusPlaybackPrefersScalarBoundsOverObjectStamps) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "fetch.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/odom/x", {500'000, 600'000});

  // A tf_static-shaped object entry stamped LONG before the window.
  auto object_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/tf_static",
          .metadata_json = R"({"builtin_object_type":"kFrameTransforms"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.sessionManager().objectStore().pushOwned(*object_topic, 7, std::vector<uint8_t>{1}).has_value());

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*dataset}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 500'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 600'000.0e-9);

  // Objects still seed when the import carries ONLY objects (a 3D-only fetch).
  auto objects_only =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud-3d.mcap"});
  ASSERT_TRUE(objects_only.has_value()) << objects_only.error();
  auto cloud_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *objects_only,
          .topic_name = "/points",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(cloud_topic.has_value()) << cloud_topic.error();
  ASSERT_TRUE(
      session.sessionManager().objectStore().pushOwned(*cloud_topic, 42'000, std::vector<uint8_t>{1}).has_value());

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*objects_only}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 42'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 42'000.0e-9);
}

TEST(AppSessionMergeTest, CollapsesSelectionIntoMergedAnchor) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // A at display [0, 1e9]; B dragged to display [2e9, 3e9] (raw 5e9..6e9, offset 3e9).
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 3'000'000'000LL, "/s", {5'000'000'000LL, 6'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  session.mergeDatasets({a, b});
  session.catalogModel().rebuildFromDatastore();

  // Only the merged anchor remains, relabelled, spanning the arranged union.
  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_EQ(datasets.front().first, a);
  EXPECT_TRUE(datasets.front().second.endsWith("_merged")) << datasets.front().second.toStdString();
  const auto range = session.datasetRawTimeRange(a);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 0);                // anchor's absolute start
  EXPECT_EQ(range->max, 3'000'000'000LL);  // B folded in at display [2e9,3e9] -> raw 2e9..3e9
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 4U);
}

TEST(AppSessionMergeTest, AnchorIsLeftmostInDisplayNotRaw) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Same raw [0,1e9], but B is dragged 5 s LEFT (offset 5e9 -> display [-5e9,-4e9]),
  // so B is the leftmost-in-display anchor even though A shares its raw start.
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 5'000'000'000LL, "/s", {0, 1'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  session.mergeDatasets({a, b});
  session.catalogModel().rebuildFromDatastore();

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_EQ(datasets.front().first, b);  // B is the anchor (leftmost in display)
  const auto range = session.datasetRawTimeRange(b);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 0);  // B's absolute start (A folded in after, shifted +5e9)
  EXPECT_EQ(mergedSampleCount(session, b, "/s"), 4U);
}

TEST(AppSessionMergeTest, SingleSelectionIsNoOp) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  session.mergeDatasets({a});  // < 2 datasets -> nothing happens
  session.catalogModel().rebuildFromDatastore();

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_FALSE(datasets.front().second.endsWith("_merged"));
}

TEST(AppSessionMergeTest, DataLessDatasetInSelectionIsIgnoredForAnchor) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});  // display [0,1]
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});  // [2,3]
  // C is registered but carries NO samples -> no time-bearing data (exercises the
  // datasetRawTimeRange-returns-nullopt continue branch in the anchor pick).
  auto c_domain = session.sessionManager().dataEngine().createTimeDomain("C_domain");
  ASSERT_TRUE(c_domain.has_value());
  auto c = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "C", .time_domain_id = *c_domain});
  ASSERT_TRUE(c.has_value());
  session.catalogModel().rebuildFromDatastore();

  const PJ::DatasetId anchor = session.mergeDatasets({a, *c, b});
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(anchor, a);                                // A is leftmost-in-display; data-less C can never anchor
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 4U);  // A's 2 + B's 2; C adds nothing
}

TEST(AppSessionMergeTest, RejectedMergeLeavesCatalogConsistentWithStore) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  // A duplicated source makes the engine REJECT the merge (nothing mutated in the
  // store). The catalog must be left intact rather than removing B — otherwise B
  // would vanish from the UI while its data still lives in the store.
  const PJ::DatasetId anchor = session.mergeDatasets({a, b, b});
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(anchor, 0U);  // no-op
  const auto datasets = session.catalogModel().datasets();
  EXPECT_EQ(datasets.size(), 2U);  // both datasets still present
  for (const auto& [id, label] : datasets) {
    EXPECT_FALSE(label.endsWith("_merged")) << label.toStdString();
  }
  EXPECT_TRUE(session.datasetRawTimeRange(b).has_value());  // B's data still live
}

TEST(AppSessionTest, RecomputeRangeMovesRangeButNeverCurrentTime) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Seed a dataset (display [0,10]) and park the playhead at an interior 4.0 s.
  const PJ::DatasetId ds = makeShiftedDataset(session, "A", 0, "/s", {0, 10'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{4.0});
  ASSERT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 4.0);

  // Shift the offset so the union range moves to display [-2,8] (still around 4.0).
  // recomputeRange is the live-drag path: it must move the RANGE and return the
  // new min, but NEVER re-snap currentTime (the property the whole design guards).
  session.sessionManager().setDisplayOffset(ds, PJ::DisplayOffset{PJ::Duration{2'000'000'000LL}});
  const auto new_min = session.recomputeRange();
  ASSERT_TRUE(new_min.has_value());
  EXPECT_DOUBLE_EQ(new_min->value, -2.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, -2.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 8.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 4.0);  // unchanged: no snap
}

}  // namespace
