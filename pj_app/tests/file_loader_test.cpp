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
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
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
    PJ::LoadHints hints;
    hints.expected_plugin_id = QStringLiteral("Mock File Source");
    hints.preset_config_json = QStringLiteral("{}");
    hints.skip_dialog = true;
    return loader_->loadFile(path, nullptr, hints);
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
