// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end tests for FileLoader. Single-instance loads run progressively on a
// worker thread (loadFile enqueues and returns; completion is async via
// fileLoaded/fileLoadFailed), and a same-file reload REPLACES the dataset's data
// in place (stable DatasetId/TopicIds, no duplicate topics, no re-append) via
// beginRefill's detach + the write-host's by-name topic reuse — NOT a staging
// swap. Drives the real plugin pipeline headlessly: the SDK's
// mock_file_source_plugin (claims ".mock", writes topic "mock/file_data" with
// 3 rows at t=100/200/300) loaded through ExtensionCatalogService, with
// skip-dialog LoadHints standing in for the data-source dialog.

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMutex>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <memory>
#include <vector>

#include "FileLoader.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/sdk/object_ingest_policy.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

#ifndef PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH
#error "PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH must be defined"
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
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".mock"_s).empty())
        << "mock_file_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());

    // The mock source never reads the file; only the path/extension matter.
    mock_path_ = makeMockFile(u"sensors.mock"_s);
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

  // Unified hints builder: skip the dialog, target the mock source, with an
  // optional preset config (e.g. {"fail_start":true} or a __pj_fanout list) and
  // the prefer_reuse flag a layout replay sets.
  [[nodiscard]] PJ::LoadHints loadHints(const QString& config = u"{}"_s, bool prefer_reuse = false) {
    PJ::LoadHints hints;
    hints.expected_plugin_id = u"Mock File Source"_s;
    hints.preset_config_json = config;
    hints.skip_dialog = true;
    hints.prefer_reuse = prefer_reuse;
    return hints;
  }

  // Back-compat alias used by the progressive-load tests.
  PJ::LoadHints skipDialogHints(bool prefer_reuse = false) {
    return loadHints(u"{}"_s, prefer_reuse);
  }

  // Enqueue a load and pump the event loop until it completes. Single-instance
  // loads run progressively on a worker thread, so completion is asynchronous
  // (fileLoaded/fileLoadFailed); fanout / layout-reuse / failure complete
  // synchronously inside loadFile (the signal fires before exec(), so done is
  // already set and we skip the loop).
  [[nodiscard]] bool loadAndWait(const QString& path, const PJ::LoadHints& hints) {
    QEventLoop loop;
    bool ok = false;
    bool done = false;
    const auto on_loaded = QObject::connect(
        loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
        [&](const QString&, const QString&, const QString&, const QString&) {
          ok = true;
          done = true;
          loop.quit();
        });
    const auto on_failed =
        QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
          ok = false;
          done = true;
          loop.quit();
        });
    loader_->loadFile(path, nullptr, hints);
    if (!done) {
      QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });  // safety: fail, don't hang CI
      loop.exec();
    }
    QObject::disconnect(on_loaded);
    QObject::disconnect(on_failed);
    return ok;
  }

  [[nodiscard]] bool load(const QString& path) {
    return loadAndWait(path, loadHints());
  }

  [[nodiscard]] bool loadWithConfig(const QString& path, const QString& config) {
    return loadAndWait(path, loadHints(config));
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
  const QString path_a = makeMockFile(u"a.mock"_s);
  const QString path_b = makeMockFile(u"b.mock"_s);
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
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runA"_s));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runB"_s));
  const QString path_a = makeMockFile(u"runA/log.mock"_s);
  const QString path_b = makeMockFile(u"runB/log.mock"_s);
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
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runA"_s));
  ASSERT_TRUE(QDir(data_dir_.path()).mkpath(u"runB"_s));
  const QString path_a = makeMockFile(u"runA/log.mock"_s);
  const QString path_b = makeMockFile(u"runB/log.mock"_s);
  ASSERT_TRUE(loadAndWait(path_a, skipDialogHints(/*prefer_reuse=*/true)));  // mimic a layout replay
  ASSERT_TRUE(loadAndWait(path_b, skipDialogHints(/*prefer_reuse=*/true)));

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
  const QString path_a = makeMockFile(u"a.mock"_s);
  const QString path_b = makeMockFile(u"b.mock"_s);
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

  // Remove dataset a the way MainWindow::removeDatasetData does.
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

  // Real delete the way MainWindow::removeDatasetData now does it: erase objects,
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
  hints.expected_plugin_id = u"Mock File Source"_s;
  hints.preset_config_json = u"{}"_s;
  hints.skip_dialog = true;
  hints.prefer_reuse = true;
  // The dataset was erased, so prefer_reuse finds nothing to reuse and falls through
  // to a FRESH single-instance load — which is async (worker). Wait for it.
  ASSERT_TRUE(loadAndWait(mock_path_, hints));

  const PJ::DatasetId reloaded = datasetNamed("sensors.mock");
  EXPECT_NE(reloaded, 0u) << "reload must re-create the dataset";
  EXPECT_NE(reloaded, id) << "a real delete + reload mints a FRESH id, not the erased one";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "the file must be re-ingested, not reattached empty";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after reload";
}

TEST_F(FileLoaderTest, FanoutReloadErasesOldDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(u"fanout.mock"_s);
  ASSERT_TRUE(load(path));

  const PJ::DatasetId old_id = datasetNamed("fanout.mock");
  ASSERT_NE(old_id, 0u);
  ASSERT_EQ(singleTopicRowCount(old_id), 3);

  PJ::LoadHints fanout_hints =
      loadHints(uR"({"__pj_fanout":["{\"display_suffix\":\"left\"}","{\"display_suffix\":\"right\"}"]})"_s);
  ASSERT_TRUE(loader_->loadFile(path, nullptr, fanout_hints));

  EXPECT_FALSE(engineHasDataset(old_id)) << "fanout reload must erase the tombstoned old dataset from the engine";
  EXPECT_EQ(datasetNamed("fanout.mock"), 0u) << "prefer_reuse must not find the old basename after fanout reload";

  PJ::LoadHints reuse_hints = loadHints(u"{}"_s);
  reuse_hints.prefer_reuse = true;
  // Old dataset erased → prefer_reuse falls through to a fresh async load; wait for it.
  ASSERT_TRUE(loadAndWait(path, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("fanout.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after fanout reload must ingest a fresh dataset";
  EXPECT_NE(reloaded, old_id) << "the old fanout-replaced DatasetId must not be reused";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "fresh prefer_reuse load must ingest rows";
}

TEST_F(FileLoaderTest, FailedFirstLoadErasesAbandonedLiveDatasetBeforePreferReuseReload) {
  const QString path = makeMockFile(u"failed.mock"_s);

  ASSERT_FALSE(loadWithConfig(path, uR"({"fail_start":true})"_s));

  EXPECT_TRUE(session().createReader().listDatasets().empty())
      << "failed first load must erase the live-engine dataset shell";
  EXPECT_EQ(datasetNamed("failed.mock"), 0u) << "no failed-load shell may remain matchable by basename";

  PJ::LoadHints reuse_hints = loadHints(u"{}"_s);
  reuse_hints.prefer_reuse = true;
  // Old dataset erased → prefer_reuse falls through to a fresh async load; wait for it.
  ASSERT_TRUE(loadAndWait(path, reuse_hints));

  const PJ::DatasetId reloaded = datasetNamed("failed.mock");
  EXPECT_NE(reloaded, 0u) << "prefer_reuse after a failed first load must create a new dataset";
  EXPECT_EQ(singleTopicRowCount(reloaded), 3) << "prefer_reuse must ingest data, not reattach to a shell";
  EXPECT_EQ(catalog().items().size(), 1u) << "curves come back after the successful reload";
}

// Several files enqueued without waiting between them run sequentially on the
// worker; queueDrained fires once when the last completes, and all datasets land.
TEST_F(FileLoaderTest, QueueProcessesEnqueuedLoadsSequentially) {
  const QString a = makeMockFile(u"qa.mock"_s);
  const QString b = makeMockFile(u"qb.mock"_s);
  const QString c = makeMockFile(u"qc.mock"_s);

  int drained = 0;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, loader_.get(), [&drained]() { ++drained; });

  EXPECT_TRUE(loader_->loadFile(a, nullptr, skipDialogHints()));
  EXPECT_TRUE(loader_->loadFile(b, nullptr, skipDialogHints()));
  EXPECT_TRUE(loader_->loadFile(c, nullptr, skipDialogHints()));

  QEventLoop loop;
  QObject::connect(loader_.get(), &PJ::FileLoader::queueDrained, &loop, &QEventLoop::quit);
  if (loader_->isBusy()) {
    QTimer::singleShot(15000, &loop, [&loop]() { loop.quit(); });
    loop.exec();
  }

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(session().createReader().listDatasets().size(), 3u) << "all three queued files must load";
  EXPECT_EQ(drained, 1) << "queueDrained fires once when the queue empties";
}

// Tearing the loader down mid-load (the closeEvent path) must join the worker
// and drain the queue without crashing or hanging; the loader stays usable after.
TEST_F(FileLoaderTest, JoinForShutdownDuringLoadDoesNotCrash) {
  EXPECT_TRUE(loader_->loadFile(mock_path_, nullptr, skipDialogHints()));
  loader_->joinForShutdown();  // worker may still be running
  EXPECT_FALSE(loader_->isBusy());

  // The loader recovers: a fresh load after shutdown still completes.
  EXPECT_TRUE(load());
  EXPECT_NE(datasetNamed("sensors.mock"), 0u);
}

// A REPLACING reload is transactional: if the reload fails after the prior data was
// detached, the RefillGuard rolls back to that prior data instead of leaving the
// dataset empty. Before this guard, the up-front in-place clear destroyed the prior
// data with no recovery — the regression this fixes (codex #1 on PR #246).
// The success/commit path is covered by ReloadingSameFileReplacesDatasetInPlace
// above (it now routes through beginRefill + commit); commit-vs-rollback SEMANTICS
// are pinned deterministically at the SessionManager layer (the mock writes the same
// 3 rows every load, so the two are indistinguishable by row count here).
TEST_F(FileLoaderTest, ReplacingReloadStartFailureRestoresPriorData) {
  ASSERT_TRUE(load());  // loads sensors.mock (3 rows)
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  ASSERT_EQ(singleTopicRowCount(id), 3);

  // Reload the SAME file with a configured start() failure. beginRefill detaches the
  // prior data up front; start() then fails on the worker, so onWorkerFinished's
  // replacing start-fail arm must ROLL BACK to the prior data, not leave it empty.
  EXPECT_FALSE(loadWithConfig(mock_path_, uR"({"fail_start":true})"_s));

  EXPECT_EQ(datasetNamed("sensors.mock"), id) << "DatasetId stays stable across a failed reload";
  EXPECT_EQ(singleTopicRowCount(id), 3) << "prior data restored, NOT left empty";
  EXPECT_EQ(catalog().items().size(), 1u) << "curve tree restored after the failed reload";
}

// Tearing the loader down mid REPLACING-reload discards the in-flight reload and
// rolls back to the prior data (joinForShutdown captures was_replacing before
// ctx_.reset(), whose guard dtor performs the rollback). Robust to timing: holds
// whether the worker had not started, was mid-flight, or had just completed.
TEST_F(FileLoaderTest, JoinForShutdownDuringReplacingReloadRestoresPriorData) {
  ASSERT_TRUE(load());  // sensors.mock, 3 rows
  const PJ::DatasetId id = datasetNamed("sensors.mock");
  ASSERT_NE(id, 0u);
  ASSERT_EQ(singleTopicRowCount(id), 3);

  EXPECT_TRUE(loader_->loadFile(mock_path_, nullptr, skipDialogHints()));  // start the replacing reload
  loader_->joinForShutdown();                                              // shut down before it finalizes

  EXPECT_FALSE(loader_->isBusy());
  EXPECT_EQ(datasetNamed("sensors.mock"), id) << "DatasetId stable across shutdown-mid-reload";
  EXPECT_EQ(singleTopicRowCount(id), 3) << "prior data restored after the shutdown rollback (not empty)";

  // The loader recovers: a fresh load after shutdown still completes.
  EXPECT_TRUE(load());
  EXPECT_NE(datasetNamed("sensors.mock"), 0u);
}

// Images and depth images must ingest PURE-LAZY (like point clouds) so their raw
// bytes are re-fetched on read instead of pinned in RAM at ingest — retaining
// every frame of every image topic was the dominant peak-RSS cost on large
// robotics MCAPs. TF intentionally stays eager (tiny payload, useful scalars).
TEST(FileLoaderIngestPolicy, ImagesAndDepthImagesArePureLazyLikePointClouds) {
  using PJ::sdk::BuiltinObjectType;
  using PJ::sdk::ObjectIngestPolicy;

  PJ::sdk::ObjectIngestPolicyResolver resolver;
  PJ::FileLoader::applyDefaultIngestPolicies(resolver);

  EXPECT_EQ(resolver.resolve("src", "/cam/color", BuiltinObjectType::kImage), ObjectIngestPolicy::kPureLazy);
  EXPECT_EQ(resolver.resolve("src", "/cam/depth", BuiltinObjectType::kDepthImage), ObjectIngestPolicy::kPureLazy);
  // Parity with the point-cloud policy that already rendered lazily.
  EXPECT_EQ(resolver.resolve("src", "/lidar", BuiltinObjectType::kPointCloud), ObjectIngestPolicy::kPureLazy);
  // Occupancy grids and voxel grids can be large; pure-lazy like point clouds.
  EXPECT_EQ(resolver.resolve("src", "/map", BuiltinObjectType::kOccupancyGrid), ObjectIngestPolicy::kPureLazy);
  EXPECT_EQ(resolver.resolve("src", "/voxels", BuiltinObjectType::kVoxelGrid), ObjectIngestPolicy::kPureLazy);
  // TF is deliberately NOT pure-lazy.
  EXPECT_NE(resolver.resolve("src", "/tf", BuiltinObjectType::kFrameTransforms), ObjectIngestPolicy::kPureLazy);
}

// Captures Qt log messages for the lifetime of the instance. The cross-thread
// QObject::setParent warning fires on the import worker thread, so the sink is
// mutex-guarded. One instance at a time (tests run sequentially).
class QtMessageCapture {
 public:
  QtMessageCapture() {
    QMutexLocker lock(&mutex());
    sink() = &messages_;
    previous_ = qInstallMessageHandler(&QtMessageCapture::handle);
  }
  ~QtMessageCapture() {
    qInstallMessageHandler(previous_);
    QMutexLocker lock(&mutex());
    sink() = nullptr;
  }
  QtMessageCapture(const QtMessageCapture&) = delete;
  QtMessageCapture& operator=(const QtMessageCapture&) = delete;

  [[nodiscard]] bool sawText(const QString& needle) {
    QMutexLocker lock(&mutex());
    return std::any_of(messages_.begin(), messages_.end(), [&](const QString& m) { return m.contains(needle); });
  }

 private:
  static QMutex& mutex() {
    static QMutex m;
    return m;
  }
  static std::vector<QString>*& sink() {
    static std::vector<QString>* s = nullptr;
    return s;
  }
  static void handle(QtMsgType /*type*/, const QMessageLogContext& /*ctx*/, const QString& msg) {
    QMutexLocker lock(&mutex());
    if (sink() != nullptr) {
      sink()->push_back(msg);
    }
  }

  std::vector<QString> messages_;
  QtMessageHandler previous_ = nullptr;
};

// Fixture for the cross-thread message-box regression. Stages the test-only
// msgbox_mock_source plugin (calls runtimeHost().askContinue() from the import
// worker thread when configured) and drives a real FileLoader against it.
class MessageBoxMarshalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());
    const QString src = QString::fromUtf8(PJ_MSGBOX_MOCK_SOURCE_PLUGIN_PATH);
    const QString dst = extensions_dir_.filePath(QFileInfo(src).fileName());
    ASSERT_TRUE(QFile::copy(src, dst)) << "could not stage " << src.toStdString();

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".msgboxmock"_s).empty())
        << "msgbox_mock_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
};

// REGRESSION (CSV-load segfault, cross-thread QMessageBox): a DataSource plugin
// may call the runtime host's message box from the import worker thread (the
// SDK contract tags show_message_box [main-thread] and promises the host
// marshals it). Pre-fix, FileLoader's setMessageBoxHandler built the QMessageBox
// directly on the worker → "QObject::setParent: ... different thread" + a
// paint-engine segfault. Here we pass a real GUI-thread dialog_parent (the
// handler is only installed when non-null), trigger a worker-thread askContinue,
// auto-click Continue from the GUI thread, and assert no cross-thread warning
// was emitted and the load completed.
TEST_F(MessageBoxMarshalTest, WorkerThreadMessageBoxIsMarshaledToGuiThread) {
  QtMessageCapture capture;
  QWidget parent;  // a real GUI-thread-owned parent for the marshaled message box

  const QString path = data_dir_.filePath(u"trigger.msgboxmock"_s);
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
  }

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Msgbox Mock Source"_s;
  hints.preset_config_json = uR"({"ask_msgbox":true})"_s;
  hints.skip_dialog = true;

  // The marshaled QMessageBox::exec() spins a nested event loop on the GUI
  // thread; this timer fires inside it, finds the modal, and clicks its Continue
  // (AcceptRole) button so askContinue returns true and the load proceeds.
  QTimer dismiss;
  dismiss.setInterval(20);
  QObject::connect(&dismiss, &QTimer::timeout, [&]() {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr) {
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* candidate = qobject_cast<QMessageBox*>(w); candidate != nullptr && candidate->isVisible()) {
          box = candidate;
          break;
        }
      }
    }
    if (box == nullptr) {
      return;
    }
    for (QAbstractButton* button : box->buttons()) {
      if (box->buttonRole(button) == QMessageBox::AcceptRole) {
        button->click();
        return;
      }
    }
  });
  dismiss.start();

  QEventLoop loop;
  bool ok = false;
  bool done = false;
  const auto on_loaded = QObject::connect(
      loader_.get(), &PJ::FileLoader::fileLoaded, &loop,
      [&](const QString&, const QString&, const QString&, const QString&) {
        ok = true;
        done = true;
        loop.quit();
      });
  const auto on_failed =
      QObject::connect(loader_.get(), &PJ::FileLoader::fileLoadFailed, &loop, [&](const QString&, const QString&) {
        ok = false;
        done = true;
        loop.quit();
      });
  loader_->loadFile(path, &parent, hints);
  if (!done) {
    QTimer::singleShot(10000, &loop, [&loop]() { loop.quit(); });  // safety: fail, don't hang CI
    loop.exec();
  }
  QObject::disconnect(on_loaded);
  QObject::disconnect(on_failed);
  dismiss.stop();

  EXPECT_FALSE(capture.sawText(u"Cannot set parent"_s))
      << "host built the QMessageBox off the GUI thread (QObject::setParent cross-thread warning)";
  EXPECT_FALSE(capture.sawText(u"different thread"_s))
      << "a cross-thread Qt warning was emitted during the worker-thread message box";
  EXPECT_TRUE(ok) << "the marshaled askContinue must return Continue and complete the load";
}

}  // namespace

int main(int argc, char** argv) {
  // ProgressDialog (shown during ingest) needs a QApplication; run offscreen.
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  // Keep test QSettings out of the user's real PlotJuggler4.conf.
  QCoreApplication::setOrganizationName(u"PJ4Tests"_s);
  QCoreApplication::setApplicationName(u"file_loader_test"_s);
  static QTemporaryDir settings_dir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
  return RUN_ALL_TESTS();
}
