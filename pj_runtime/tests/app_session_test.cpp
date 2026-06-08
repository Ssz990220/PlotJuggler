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
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{150.0e-9});

  auto second_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "second.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  addScalarSamples(session, *second_dataset, "/imu/x", {120, 180});

  EXPECT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 200.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 150.0e-9);
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

}  // namespace
