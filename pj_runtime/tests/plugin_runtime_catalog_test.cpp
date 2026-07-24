// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mock_data_source_vtable.h"
#include "pj_marketplace/version_compare.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "plugin_test_utils.h"

namespace PJ {
namespace {

using test::pluginFileName;

class PluginCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pj_catalog_test_" +
            std::to_string(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

TEST_F(PluginCatalogTest, RuntimeCatalogDedupsDuplicateIdFirstFolderWins) {
  // The same data-source DSO (manifest id "mock-data-source") placed in two
  // folders: setPluginDirs scans them in order and must load it exactly once,
  // from the first (higher-priority) folder.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_b / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "mock-data-source");
  // The winner must come from the first folder. Compare at the filesystem level
  // (std::filesystem::equivalent) so it holds regardless of how the stored path
  // is spelled — Windows back/forward slashes, drive-letter case, canonicalisation.
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a))
      << "winner " << winner << " is not in the first folder " << dir_a;
}

TEST_F(PluginCatalogTest, RuntimeCatalogSkipsDuplicateScanFolders) {
  // The same folder listed twice in setPluginDirs is scanned once: the second pass is
  // skipped, so no plugin is ever compared against itself — which would otherwise emit a
  // confusing "ignoring duplicate id … already loaded from <same path>" diagnostic.
  const std::filesystem::path dir_a = dir_ / "a";
  std::filesystem::create_directories(dir_a);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  catalog.setPluginDirs({{dir_a}, {dir_a}});  // same folder twice
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  for (const std::string& message : messages) {
    EXPECT_EQ(message.find("ignoring duplicate"), std::string::npos)
        << "a folder listed twice must not trigger a self-dedup diagnostic: " << message;
  }
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersHigherVersionOverFolderPriority) {
  // Same plugin id in two folders, but the LOWER-priority folder ships a newer
  // version (v2.0.0) than the higher-priority one (v1.0.0). Version wins over
  // folder priority, so the v2 DSO from the second folder is the one loaded.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));     // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "mock-data-source");
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the newer v2 in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogKeepsHigherPriorityWhenNewerVersionIsHigherPriority) {
  // Mirror of the above: the newer version now sits in the HIGHER-priority folder.
  // The winner is still v2.0.0, confirming the tie-break never demotes it.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v2.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_b / pluginFileName("ds"));     // v1.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a));
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersCompatibleOverHigherIncompatibleVersion) {
  // Higher-priority folder ships an incompatible v3.0.0 (needs PlotJuggler 5.0.0);
  // lower-priority folder ships a compatible v2.0.0. With a 4.0.0 host, the
  // compatible build wins even though it is both lower version and lower priority.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(
      PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));               // v3, min 5.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2, no min

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the compatible v2 in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogLoadsLoneIncompatiblePlugin) {
  // A single incompatible plugin (needs PlotJuggler 5.0.0) still loads on a 4.0.0
  // host: min_plotjuggler_version only breaks ties, it never excludes.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "3.0.0");
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersTheOnlyCompatibleAmongMixedCandidates) {
  // Three folders, same id, mixed compatibility: incompatible v3.0.0 (min 5.0.0) in
  // the highest-priority folder, compatible v2.0.0 in the middle, incompatible v1.5.0
  // (min 5.0.0) in the lowest. With a 4.0.0 host the compatible v2.0.0 wins — it is
  // neither the highest version nor the highest-priority folder.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  const std::filesystem::path dir_c = dir_ / "c";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::create_directories(dir_c);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v3, min 5
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));            // v2, compat
  std::filesystem::copy_file(
      PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_LOW_PLUGIN_PATH, dir_c / pluginFileName("ds"));  // v1.5, min 5

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a}, {dir_b}, {dir_c}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the only compatible candidate in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderOverridesHigherVersion) {
  // An authoritative (user-explicit) folder is a hard override: its v1.0.0 wins over
  // a higher v2.0.0 in a lower-priority managed folder. Without the authoritative
  // mark, version would pick v2.0.0 — this isolates the override.
  const std::filesystem::path dir_a = dir_ / "a";  // authoritative
  const std::filesystem::path dir_b = dir_ / "b";  // managed
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));     // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a, true}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a));
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderWinsEvenWhenIncompatible) {
  // The authoritative copy wins even if it is incompatible (min 5.0.0 > host 4.0.0)
  // and the managed alternative is compatible: an explicit choice overrides compat.
  const std::filesystem::path dir_a = dir_ / "a";  // authoritative, incompatible v3.0.0
  const std::filesystem::path dir_b = dir_ / "b";  // managed, compatible v2.0.0
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v3, min 5
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));            // v2, compat

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a, true}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "3.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a));
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderMayBeASymlink) {
  // The authoritative tier is a per-entry flag, so it must hold regardless of how
  // the folder is spelled — here the scan entry is a symlink to the real folder.
  const std::filesystem::path real_dir = dir_ / "real";  // symlink target, authoritative v1.0.0
  const std::filesystem::path link_dir = dir_ / "link";  // authoritative spelling (a symlink)
  const std::filesystem::path dir_b = dir_ / "b";        // managed, v2.0.0

  std::filesystem::create_directories(real_dir);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, real_dir / pluginFileName("ds"));  // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0
  std::error_code ec;
  std::filesystem::create_directory_symlink(real_dir, link_dir, ec);
  if (ec) {
    GTEST_SKIP() << "filesystem does not support directory symlinks: " << ec.message();
  }

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{link_dir, true}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0") << "authoritative override must hold for a symlinked scan entry";
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), real_dir));
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderSupersedesEarlierManagedIncumbent) {
  // The authoritative folder is scanned AFTER a managed one (it sits lower in the scan
  // list), so a managed incumbent is already recorded when the authoritative copy arrives.
  // The authoritative candidate must still supersede it — the override is not order
  // dependent. Complements the existing tests, which place the authoritative folder first.
  const std::filesystem::path dir_managed = dir_ / "managed";  // higher priority, managed, v2.0.0
  const std::filesystem::path dir_auth = dir_ / "auth";        // lower priority, authoritative, v1.0.0
  std::filesystem::create_directories(dir_managed);
  std::filesystem::create_directories(dir_auth);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_managed / pluginFileName("ds"));  // v2.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_auth / pluginFileName("ds"));        // v1.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_managed}, {dir_auth, true}});  // managed scanned first, authoritative second
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0")
      << "an authoritative folder must override an already-recorded managed incumbent";
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_auth));
}

TEST_F(PluginCatalogTest, RuntimeCatalogLoadsDistinctIdsFromMultipleFolders) {
  // Different plugins in different folders all load.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));
  std::filesystem::copy_file(PJ_MOCK_TOOLBOX_PLUGIN_PATH, dir_b / pluginFileName("tb"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  EXPECT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.toolboxes().size(), 1U);
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationSurvivesScanAndReload) {
  // A statically registered plugin has no backing file, so the disk
  // reconciliation in scanDirectory()/reload() must leave it loaded — its
  // synthetic "static://" path never appears among the scanned DSO paths.
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"static-mock","name":"Static Mock","version":"1.0.0"})");

  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  ASSERT_TRUE(catalog.registerStaticDataSource(&vt));
  ASSERT_EQ(catalog.dataSources().size(), 1U);

  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 2U) << "scanDirectory() must keep the static registration";

  EXPECT_FALSE(catalog.reload()) << "an unchanged disk state must not count as a change";
  ASSERT_EQ(catalog.dataSources().size(), 2U) << "reload() must not evict the static registration";
  EXPECT_TRUE(std::ranges::any_of(catalog.dataSources(), [](const RuntimeDataSourcePlugin& plugin) {
    return plugin.id == "static-mock";
  }));
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationShadowsDsoWithSameId) {
  // A statically registered id outranks every scan folder: the DSO sharing the
  // id ("mock-data-source") must be skipped by scanDirectory(), and a later
  // static registration of an id already loaded from a DSO must evict the DSO.
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"mock-data-source","name":"Static Mock","version":"0.1.0"})");

  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_ / pluginFileName("ds"));  // v2.0.0 DSO

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  ASSERT_TRUE(catalog.registerStaticDataSource(&vt));

  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 1U) << "the same-id DSO must be shadowed by the static registration";
  EXPECT_EQ(catalog.dataSources()[0].version, "0.1.0");

  // Order-independence: load the DSO first, then register the static copy.
  PluginRuntimeCatalog catalog_dso_first;
  catalog_dso_first.setPluginDir(dir_);
  catalog_dso_first.scanDirectory();
  ASSERT_EQ(catalog_dso_first.dataSources().size(), 1U);
  EXPECT_EQ(catalog_dso_first.dataSources()[0].version, "2.0.0");
  ASSERT_TRUE(catalog_dso_first.registerStaticDataSource(&vt));
  ASSERT_EQ(catalog_dso_first.dataSources().size(), 1U) << "the static registration must evict the same-id DSO";
  EXPECT_EQ(catalog_dso_first.dataSources()[0].version, "0.1.0");

  // A second static registration of the same id is a caller bug: rejected.
  EXPECT_FALSE(catalog_dso_first.registerStaticDataSource(&vt));
  EXPECT_EQ(catalog_dso_first.dataSources().size(), 1U);
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsMistypedManifest) {
  // A well-formed manifest with mistyped fields ("id":123) must fail with a
  // diagnostic, not escape as a JSON exception (the documented contract).
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":123,"name":"Bad Types","version":"1.0.0"})");

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_FALSE(messages.empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsNullCreate) {
  // A plugin whose create() returns null must be rejected by the validity
  // probe, never handed a null context via capabilities().
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"null-create","name":"Null Create","version":"1.0.0"})", pj_mock::detail::createNull);

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_FALSE(messages.empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRequiresManifestId) {
  // The manifest id is the only identity a static plugin has (its synthetic
  // path derives from it), so an id-less manifest is rejected with a diagnostic.
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(R"({"name":"No Id","version":"1.0.0"})");

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_FALSE(messages.empty());
}

// ─── compareSemver (pj_marketplace/version_compare.hpp — the shared version
//     ordering used by the catalog dedup, the seed, and the marketplace) ──────────

TEST(CompareSemver, OrdersByNumericComponents) {
  EXPECT_LT(compareSemver("4.0.2", "4.1.0"), 0);
  EXPECT_GT(compareSemver("5.0.0", "4.9.9"), 0);
  EXPECT_EQ(compareSemver("4.1.0", "4.1.0"), 0);
}

TEST(CompareSemver, TreatsMissingTrailingComponentsAsZero) {
  EXPECT_EQ(compareSemver("4.1", "4.1.0"), 0);
  EXPECT_EQ(compareSemver("4", "4.0.0"), 0);
  EXPECT_LT(compareSemver("4.0", "4.0.1"), 0);
}

TEST(CompareSemver, IgnoresLeadingZerosAndSuffixes) {
  EXPECT_EQ(compareSemver("4.01.0", "4.1.0"), 0);
  EXPECT_EQ(compareSemver("1.0.0-rc2", "1.0.0"), 0);  // pre-release suffix ignored
  EXPECT_LT(compareSemver("1.0.0", "2.0.0-beta"), 0);
}

TEST(CompareSemver, IgnoresDottedPreReleaseSuffix) {
  // The suffix is ignored in full even when it contains dots: the compare must not walk
  // past the '-' and read the suffix's own numeric parts (regression for a version that
  // stepped to the next '.' instead of stopping at the pre-release separator).
  EXPECT_EQ(compareSemver("1.0.0-rc.2", "1.0.0-rc.3"), 0);
  EXPECT_EQ(compareSemver("1.0-rc.2", "1.0.0"), 0);
  EXPECT_LT(compareSemver("1.0.0-rc.9", "1.0.1"), 0);
}

TEST(CompareSemver, DoesNotOverflowOnHugeComponents) {
  EXPECT_GT(compareSemver("999999999999999999999.0.0", "4.0.0"), 0);
  EXPECT_LT(compareSemver("4.0.0", "1000000000000000000000.0.0"), 0);
  EXPECT_EQ(compareSemver("100000000000000000000.0", "100000000000000000000.0"), 0);
}

}  // namespace
}  // namespace PJ
