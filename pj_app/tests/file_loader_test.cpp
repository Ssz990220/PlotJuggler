// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end regression tests for FileLoader's same-file reload path: loading
// an already-loaded file must REPLACE the dataset's data in place (stable
// DatasetId/TopicIds, no re-append into the live engine, no wipe from a stale
// staged swap). Drives the real plugin pipeline headlessly: the SDK's
// mock_file_source_plugin (claims ".mock", writes topic "mock/file_data" with
// 3 rows at t=100/200/300) loaded through ExtensionCatalogService, with
// skip-dialog LoadHints standing in for the data-source dialog.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <memory>

#include "FileLoader.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

class FileLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());

    // Stage a copy of the plugin instead of pointing at the build dir: the SDK
    // builds deliberately-broken sibling test plugins next to this one, and
    // they would pollute the catalog with load-error diagnostics.
    const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    const QString plugin_dst = extensions_dir_.filePath(QFileInfo(plugin_src).fileName());
    ASSERT_TRUE(QFile::copy(plugin_src, plugin_dst)) << "could not stage " << plugin_src.toStdString();

    // AppSession bundles SessionManager/CatalogModel/ExtensionCatalogService
    // with the destruction order the plugin handles require (session-held
    // parser handles die before the plugin libraries unload).
    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(QStringLiteral(".mock")).empty())
        << "mock_file_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());

    // The mock source never reads the file; only the path/extension matter.
    mock_path_ = makeMockFile(QStringLiteral("sensors.mock"));
  }

  [[nodiscard]] PJ::SessionManager& session() {
    return app_session_->sessionManager();
  }
  [[nodiscard]] PJ::CatalogModel& catalog() {
    return app_session_->catalogModel();
  }

  [[nodiscard]] QString makeMockFile(const QString& name) {
    const QString path = data_dir_.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    return path;
  }

  [[nodiscard]] bool load(const QString& path) {
    return loadWithConfig(path, QStringLiteral("{}"));
  }

  [[nodiscard]] bool loadWithConfig(const QString& path, const QString& config) {
    return loader_->loadFile(path, nullptr, loadHints(config));
  }

  [[nodiscard]] PJ::LoadHints loadHints(const QString& config) {
    PJ::LoadHints hints;
    hints.expected_plugin_id = QStringLiteral("Mock File Source");
    hints.preset_config_json = config;
    hints.skip_dialog = true;
    return hints;
  }

  [[nodiscard]] bool load() {
    return load(mock_path_);
  }

  // The dataset whose engine-side source_name matches `basename`, or 0.
  [[nodiscard]] PJ::DatasetId datasetNamed(const std::string& basename) {
    for (const PJ::DatasetId id : session().createReader().listDatasets()) {
      const PJ::DatasetInfo* info = session().dataEngine().getDataset(id);
      if (info != nullptr && info->source_name == basename) {
        return id;
      }
    }
    return 0;
  }

  // Row count of the dataset's single topic, or -1 when the topic set is not
  // exactly {mock/file_data} (wiped or duplicated).
  [[nodiscard]] int64_t singleTopicRowCount(PJ::DatasetId dataset_id) {
    const PJ::DataReader reader = session().createReader();
    const auto topics = reader.listTopics(dataset_id);
    if (topics.size() != 1u) {
      return -1;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? static_cast<int64_t>(metadata->total_row_count) : -1;
  }

  [[nodiscard]] bool engineHasDataset(PJ::DatasetId dataset_id) {
    const auto ids = session().createReader().listDatasets();
    return std::find(ids.begin(), ids.end(), dataset_id) != ids.end();
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
  QString mock_path_;
};

TEST_F(FileLoaderTest, ReloadingSameFileReplacesDatasetInPlace) {
  ASSERT_TRUE(load());

  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3);
  EXPECT_EQ(catalog().items().size(), 1u);

  // Second load of the same file: replace in place — same DatasetId, same
  // topic, same row count. The pre-fix wiring ingested into the LIVE engine
  // (appending duplicate rows) and then swapped from the EMPTY staged engine
  // (retiring every topic); the row-count check catches both regressions.
  ASSERT_TRUE(load());

  EXPECT_EQ(session().createReader().listDatasets().size(), 1u) << "reload must not mint a second dataset";
  EXPECT_EQ(datasetNamed("sensors.mock"), dataset_id) << "reload must keep the DatasetId stable";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "reload wiped, duplicated, or re-appended the dataset's topics";
  EXPECT_EQ(catalog().items().size(), 1u) << "curve tree must survive a same-file reload";
}

// With several files loaded, reloading ONE of them must replace only that
// dataset. The staged ingest always allocates dataset id 1 in its throwaway
// engine, so any id confusion between staged and primary leaks the reload into
// whichever primary dataset shares the staged id (here: the first file).
TEST_F(FileLoaderTest, ReloadWithMultipleDatasetsLeavesOthersIntact) {
  const QString path_a = makeMockFile(QStringLiteral("a.mock"));
  const QString path_b = makeMockFile(QStringLiteral("b.mock"));
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));

  const PJ::DatasetId dataset_a = datasetNamed("a.mock");
  const PJ::DatasetId dataset_b = datasetNamed("b.mock");
  ASSERT_NE(dataset_a, 0u);
  ASSERT_NE(dataset_b, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_a), 3);
  ASSERT_EQ(singleTopicRowCount(dataset_b), 3);

  ASSERT_TRUE(load(path_b));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u) << "reload must not mint a new dataset";
  EXPECT_EQ(singleTopicRowCount(dataset_a), 3) << "reloading b must not touch dataset a";
  EXPECT_EQ(singleTopicRowCount(dataset_b), 3) << "dataset b must be replaced in place";
  EXPECT_EQ(catalog().items().size(), 2u) << "curve tree must keep both datasets' curves";
}

// Two files that share a basename but live in different directories (e.g.
// run1/log.mcap and run2/log.mcap — common for per-run robotics logs) must load
// as TWO distinct datasets. The same-source match keys on full-path identity,
// not basename, so the second file is not mistaken for a reload of the first
// (which would silently alias its data and lose the second file entirely).
TEST_F(FileLoaderTest, SameBasenameDifferentDirsLoadAsDistinctDatasets) {
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(QStringLiteral("runA")));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(QStringLiteral("runB")));
  const QString path_a = makeMockFile(QStringLiteral("runA/log.mock"));
  const QString path_b = makeMockFile(QStringLiteral("runB/log.mock"));
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "same-basename files in different dirs must be distinct datasets, not aliased";
  EXPECT_EQ(catalog().items().size(), 2u) << "both files' curves must appear in the tree";
}

// The layout-reload path (prefer_reuse) of two same-basename files in different
// dirs must likewise restore both — the multi-file layout feature this depends
// on. Pre-fix the second prefer_reuse load reused the first dataset and emitted
// no new one, collapsing the session to a single file.
TEST_F(FileLoaderTest, LayoutReloadOfSameBasenameDifferentDirsRestoresBoth) {
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(QStringLiteral("runA")));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(QStringLiteral("runB")));
  const QString path_a = makeMockFile(QStringLiteral("runA/log.mock"));
  const QString path_b = makeMockFile(QStringLiteral("runB/log.mock"));
  PJ::LoadHints hints;
  hints.expected_plugin_id = QStringLiteral("Mock File Source");
  hints.preset_config_json = QStringLiteral("{}");
  hints.skip_dialog = true;
  hints.prefer_reuse = true;  // mimic a layout replay
  ASSERT_TRUE(loader_->loadFile(path_a, nullptr, hints));
  ASSERT_TRUE(loader_->loadFile(path_b, nullptr, hints));

  EXPECT_EQ(session().createReader().listDatasets().size(), 2u)
      << "layout reload of two same-basename files must restore two datasets";
}

// FileLoader maps each loaded dataset back to the file it came from (so the
// shell can drop the right loaded-source entry when a dataset is removed).
// untrackDataset clears that association.
TEST_F(FileLoaderTest, SourcePathForDatasetTracksLoadedFileAndUntracks) {
  ASSERT_TRUE(load());  // loads mock_path_ (sensors.mock)
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  EXPECT_EQ(loader_->sourcePathForDataset(id), mock_path_);
  // An unknown id has no recorded path.
  EXPECT_TRUE(loader_->sourcePathForDataset(id + 1000).isEmpty());

  loader_->untrackDataset(id);
  EXPECT_TRUE(loader_->sourcePathForDataset(id).isEmpty()) << "untrackDataset must drop the association";
}

// Core of the resurrection fix (MainWindow::appendDataSourceElement's liveness
// filter): once a dataset is removed, the file it came from must drop out of the
// set of source paths still backing a live dataset — otherwise a saved layout
// would re-list and resurrect it on reload. Exercised with the real
// CatalogModel + FileLoader (the XML emission itself lives in MainWindow).
TEST_F(FileLoaderTest, RemovedDatasetDropsFromLiveSourcePaths) {
  const QString path_a = makeMockFile(QStringLiteral("a.mock"));
  const QString path_b = makeMockFile(QStringLiteral("b.mock"));
  ASSERT_TRUE(load(path_a));
  ASSERT_TRUE(load(path_b));
  const PJ::DatasetId id_a = datasetNamed("a.mock");
  ASSERT_NE(id_a, 0u);

  // The set of source paths still backing a live catalog dataset — exactly how
  // appendDataSourceElement decides which <fileInfo> entries to write.
  const auto live_paths = [&]() {
    QSet<QString> paths;
    for (const auto& [id, name] : catalog().datasets()) {
      (void)name;
      if (const QString p = loader_->sourcePathForDataset(id); !p.isEmpty()) {
        paths.insert(p);
      }
    }
    return paths;
  };

  EXPECT_TRUE(live_paths().contains(path_a));
  EXPECT_TRUE(live_paths().contains(path_b));

  // Remove dataset a the way MainWindow::onRemoveDatasetRequested does.
  session().evictDatasetObjects(id_a);
  catalog().removeDataset(id_a);
  loader_->untrackDataset(id_a);

  const QSet<QString> live = live_paths();
  EXPECT_FALSE(live.contains(path_a)) << "removed dataset's file must not be a live source (no resurrection)";
  EXPECT_TRUE(live.contains(path_b)) << "surviving dataset's file stays a live source";
}

// A real "Remove Dataset" (no tombstone) truly erases the dataset from the engine, so a
// later load of the SAME source — even via the layout-replay prefer_reuse path — mints a
// FRESH dataset and re-ingests the data, instead of reattaching to an emptied/tombstoned
// shell (the bug: the 2nd load showed no progress bar and no data).
TEST_F(FileLoaderTest, RealDeleteThenPreferReuseReloadReIngestsFreshDataset) {
  ASSERT_TRUE(load());
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  ASSERT_EQ(singleTopicRowCount(id), 3);
  ASSERT_EQ(catalog().items().size(), 1u);

  // Real delete the way MainWindow::onRemoveDatasetRequested now does it: erase objects,
  // drop catalog items WITHOUT a tombstone, then erase the engine's scalar storage.
  session().evictDatasetObjects(id);
  catalog().removeDataset(id, /*tombstone=*/false);
  session().dataEngine().removeDataset(id);

  EXPECT_TRUE(session().createReader().listDatasets().empty()) << "dataset truly erased from the engine";
  EXPECT_TRUE(catalog().items().empty()) << "no catalog items survive a real delete";
  EXPECT_EQ(datasetNamed("sensors.mock"), 0u) << "no tombstoned shell left behind";

  // Reload the same source as a layout replay would (prefer_reuse). Pre-fix this reused
  // the emptied dataset (restoreDataset + return-false: no worker, no re-ingest); now the
  // erased dataset is gone from listDatasets, so the loader mints a fresh one and ingests.
  PJ::LoadHints hints;
  hints.expected_plugin_id = QStringLiteral("Mock File Source");
  hints.preset_config_json = QStringLiteral("{}");
  hints.skip_dialog = true;
  hints.prefer_reuse = true;
  ASSERT_TRUE(loader_->loadFile(mock_path_, nullptr, hints));

  const PJ::DatasetId reloaded = datasetNamed("sensors.mock");
  EXPECT_NE(reloaded, 0u) << "reload must re-create the dataset";
  EXPECT_NE(reloaded, id) << "a real delete + reload mints a FRESH id, not the erased one";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "the file must be re-ingested, not reattached empty";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after reload";
}

TEST_F(FileLoaderTest, FanoutReloadErasesOldDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(QStringLiteral("fanout.mock"));
  ASSERT_TRUE(load(path));

  const PJ::DatasetId old_id = datasetNamed("fanout.mock");
  ASSERT_NE(old_id, 0u);
  ASSERT_EQ(singleTopicRowCount(old_id), 3);

  PJ::LoadHints fanout_hints = loadHints(
      QStringLiteral(R"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"));
  ASSERT_TRUE(loader_->loadFile(path, nullptr, fanout_hints));

  EXPECT_FALSE(engineHasDataset(old_id)) << "fanout reload must erase the tombstoned old dataset from the engine";
  EXPECT_EQ(datasetNamed("fanout.mock"), 0u) << "prefer_reuse must not find the old basename after fanout reload";

  PJ::LoadHints reuse_hints = loadHints(QStringLiteral("{}"));
  reuse_hints.prefer_reuse = true;
  ASSERT_TRUE(loader_->loadFile(path, nullptr, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("fanout.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after fanout reload must ingest a fresh dataset";
  EXPECT_NE(reloaded, old_id) << "the old fanout-replaced DatasetId must not be reused";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "fresh prefer_reuse load must ingest rows";
}

TEST_F(FileLoaderTest, FailedFirstLoadErasesAbandonedLiveDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(QStringLiteral("failed.mock"));

  ASSERT_FALSE(loadWithConfig(path, QStringLiteral(R"({"fail_start":true})")));

  EXPECT_TRUE(session().createReader().listDatasets().empty())
      << "failed first load must erase the live-engine dataset shell";
  EXPECT_EQ(datasetNamed("failed.mock"), 0u) << "no failed-load shell may remain matchable by basename";

  PJ::LoadHints reuse_hints = loadHints(QStringLiteral("{}"));
  reuse_hints.prefer_reuse = true;
  ASSERT_TRUE(loader_->loadFile(path, nullptr, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("failed.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after a failed first load must create a new dataset";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "prefer_reuse must ingest data, not reattach to a shell";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after the successful reload";
}

}  // namespace

int main(int argc, char** argv) {
  // ProgressDialog (shown during ingest) needs a QApplication; run offscreen.
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  // Keep test QSettings out of the user's real PlotJuggler4.conf.
  QCoreApplication::setOrganizationName(QStringLiteral("PJ4Tests"));
  QCoreApplication::setApplicationName(QStringLiteral("file_loader_test"));
  static QTemporaryDir settings_dir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
  return RUN_ALL_TESTS();
}
