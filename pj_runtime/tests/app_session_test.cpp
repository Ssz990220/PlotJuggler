// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <string_view>
#include <vector>

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

TEST(AppSessionTest, CurveColorRegistryIsOwnedBySessionManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // The registry is owned by SessionManager; AppSession's accessor delegates to
  // it. Plot widgets reach the same instance through their SessionManager
  // pointer, so it never has to be threaded through their constructors.
  EXPECT_EQ(&session.curveColorRegistry(), &session.sessionManager().curveColorRegistry());
}

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

}  // namespace
