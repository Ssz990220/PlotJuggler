// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Bundled-plugin seeding: the bundled (share) dir is a seed source only —
// ExtensionCatalogService syncs it into the marketplace dir at construction and
// never scans it as a load path. Covers the seed rules (copy / version-aware
// refresh / never-downgrade / reseed-on-unreadable), the seed running in
// --plugin-dir mode, the default-mode-only core lock, and the override dir
// outranking a newer marketplace copy.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <memory>
#include <string>

#include "pj_marketplace/extension_manager.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "plugin_test_utils.h"

namespace PJ {
namespace {

// Thin QString adapter over the shared platform-suffix helper.
QString pluginFileName(const QString& stem) {
  return QString::fromStdString(test::pluginFileName(stem.toStdString()));
}

// Both mock DSOs carry the manifest id "mock-data-source": v1 reports version
// 1.0.0, v2 reports 2.0.0.
constexpr const char* kMockId = "mock-data-source";

class ExtensionCatalogSeedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(bundled_.isValid());
    ASSERT_TRUE(marketplace_.isValid());
    ASSERT_TRUE(override_.isValid());
  }

  // Copies a mock plugin DSO into dir under the given file name.
  static void placePlugin(const QString& dir, const char* source_path, const QString& file_name) {
    ASSERT_TRUE(QDir().mkpath(dir));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(source_path), dir + "/" + file_name));
  }

  // Service in default mode (no --plugin-dir): the marketplace dir is both the
  // managed install dir and the seed destination.
  [[nodiscard]] std::unique_ptr<ExtensionCatalogService> makeDefaultModeService() const {
    return std::make_unique<ExtensionCatalogService>(
        ExtensionCatalogService::Paths{{}, marketplace_.path(), bundled_.path()}, DiagnosticSink{}, nullptr);
  }

  // Service in --plugin-dir mode: override_ is the managed install dir.
  [[nodiscard]] std::unique_ptr<ExtensionCatalogService> makeOverrideModeService() const {
    return std::make_unique<ExtensionCatalogService>(
        ExtensionCatalogService::Paths{override_.path(), marketplace_.path(), bundled_.path()}, DiagnosticSink{},
        nullptr);
  }

  // The single loaded DataSource matching the mock id, or nullptr.
  static const LoadedDataSource* findMock(const ExtensionCatalogService& service) {
    for (const LoadedDataSource& plugin : service.dataSources()) {
      if (plugin.id == kMockId) {
        return &plugin;
      }
    }
    return nullptr;
  }

  QString seededDir() const {
    return marketplace_.path() + "/" + kMockId;
  }

  QTemporaryDir bundled_;
  QTemporaryDir marketplace_;
  QTemporaryDir override_;
};

TEST_F(ExtensionCatalogSeedTest, FirstLaunchSeedsBundledIntoMarketplaceDir) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeDefaultModeService();

  EXPECT_TRUE(QDir(seededDir()).exists()) << "bundled plugin must be copied under <marketplace>/<id>";
  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "seeded plugin must load from the marketplace dir";
  EXPECT_EQ(mock->version, "1.0.0");
  // Default mode: the bundled id -> version map reaches the ExtensionManager
  // (uninstall lock + downgrade-to-bundled).
  EXPECT_TRUE(service->extensionManager().isBundled(QString::fromUtf8(kMockId)));
  EXPECT_EQ(service->extensionManager().bundledVersion(QString::fromUtf8(kMockId)), "1.0.0");
}

TEST_F(ExtensionCatalogSeedTest, RefreshesSeededCopyWhenBundledIsNewer) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("old_v1"));     // installed 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // bundled 2.0.0

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "2.0.0") << "an older installed copy must be refreshed to the bundled version";
  // The refresh replaces the payload; the old version's files do not linger.
  EXPECT_FALSE(QFile::exists(seededDir() + "/" + pluginFileName("old_v1")));
  // The staging area (a sibling of the marketplace dir, never inside it) is
  // removed once the refresh lands.
  EXPECT_FALSE(QDir(marketplace_.path() + ".seed_stage").exists());
  EXPECT_FALSE(QDir(marketplace_.path() + "/.seed_stage").exists());
}

TEST_F(ExtensionCatalogSeedTest, NeverDowngradesAnInstallAheadOfBundled) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("user_v2"));  // installed 2.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));      // bundled 1.0.0

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "2.0.0") << "a marketplace update above the bundled version must survive the seed";
  EXPECT_TRUE(QFile::exists(seededDir() + "/" + pluginFileName("user_v2")));
}

TEST_F(ExtensionCatalogSeedTest, EqualVersionNeverRecopies) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("user_named"));  // installed 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));      // bundled 1.0.0

  const auto service = makeDefaultModeService();

  // Version-only comparison: an equal version is left byte-for-byte untouched
  // (shipping a change requires bumping the plugin version).
  EXPECT_TRUE(QFile::exists(seededDir() + "/" + pluginFileName("user_named")));
  EXPECT_FALSE(QFile::exists(seededDir() + "/" + pluginFileName("ds")));
}

TEST_F(ExtensionCatalogSeedTest, ReseedsWhenInstalledCopyIsUnreadable) {
  ASSERT_TRUE(QDir().mkpath(seededDir()));  // gutted: the id folder exists but holds no plugin
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "a gutted install must be treated as absent and reseeded";
  EXPECT_EQ(mock->version, "1.0.0");
}

TEST_F(ExtensionCatalogSeedTest, OverrideModeSeedsMarketplaceDirWithoutCoreLock) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeOverrideModeService();

  // The seed targets the marketplace dir in every mode — never the override dir.
  EXPECT_TRUE(QDir(seededDir()).exists());
  EXPECT_FALSE(QDir(override_.path() + "/" + kMockId).exists());
  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "bundled plugins load via the marketplace tier in --plugin-dir mode";
  // The core lock is scoped to default mode: the manager governs the override
  // dir here, where a same-id copy is user-owned and must stay uninstallable.
  EXPECT_FALSE(service->extensionManager().isBundled(QString::fromUtf8(kMockId)));
}

TEST_F(ExtensionCatalogSeedTest, OverrideDirOutranksNewerMarketplaceCopy) {
  placePlugin(override_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));    // override 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // seeds 2.0.0

  const auto service = makeOverrideModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "1.0.0")
      << "--plugin-dir is authoritative: its copy wins even against a newer marketplace version";
  int mock_count = 0;
  for (const LoadedDataSource& plugin : service->dataSources()) {
    mock_count += plugin.id == kMockId ? 1 : 0;
  }
  EXPECT_EQ(mock_count, 1);
}

}  // namespace
}  // namespace PJ
