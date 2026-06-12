// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "urdf_package_resolver.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QTemporaryFile>
#include <string>

namespace pj::scene3d {
namespace {

#ifndef PJ_SCENE3D_FIXTURES_DIR
#error "PJ_SCENE3D_FIXTURES_DIR must be defined by the build"
#endif

std::string fixtures() {
  return std::string(PJ_SCENE3D_FIXTURES_DIR);
}

// A throwaway in-memory QSettings (Ini format, unique temp file per test).
class ScopedSettings {
 public:
  ScopedSettings() {
    tmp_.open();
    tmp_.close();
    settings_ = std::make_unique<QSettings>(tmp_.fileName(), QSettings::IniFormat);
  }
  QSettings* get() {
    return settings_.get();
  }

 private:
  QTemporaryFile tmp_;
  std::unique_ptr<QSettings> settings_;
};

// ---------------------------------------------------------------------------
// URI-scheme dispatch + the package:// guard
// ---------------------------------------------------------------------------

TEST(UrdfResolver, FileSchemeStrippedToAbsolutePath) {
  UrdfPackageResolver r;
  const ResolvedMesh m = r.resolveUri("file:///abs/path/mesh.stl", "/some/dir", false);
  EXPECT_TRUE(m.resolved);
  EXPECT_EQ(m.path, "/abs/path/mesh.stl");
  EXPECT_FALSE(m.is_url);
}

TEST(UrdfResolver, BarePathJoinsUrdfDirAndSkipsPackageSearch) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  // This bare path's first segment matches a package dir in the search root,
  // but the guard must keep it OUT of the package search.
  const ResolvedMesh m = r.resolveUri("demo_description/meshes/x.stl", "/urdf/dir", false);
  EXPECT_TRUE(m.resolved);
  EXPECT_EQ(m.path, "/urdf/dir/demo_description/meshes/x.stl");
  EXPECT_TRUE(r.unresolvedPackages().isEmpty());  // chain never ran
}

TEST(UrdfResolver, HttpBlockedForNonUrlSource) {
  UrdfPackageResolver r;
  const ResolvedMesh blocked = r.resolveUri("https://host/m.stl", "/dir", /*source_is_url=*/false);
  EXPECT_FALSE(blocked.resolved);
  const ResolvedMesh allowed = r.resolveUri("https://host/m.stl", "https://host/", /*source_is_url=*/true);
  EXPECT_TRUE(allowed.resolved);
  EXPECT_TRUE(allowed.is_url);
}

// ---------------------------------------------------------------------------
// Chain step 0 — MCAP attachment exact-name lookup
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step0_AttachmentExactNameHit) {
  UrdfPackageResolver r;
  QMap<QString, QByteArray> att;
  att.insert("package://demo_description/meshes/base.dae", QByteArray("DAE-BYTES"));
  r.setMcapAttachments(att);

  const ResolvedMesh m = r.resolveUri("package://demo_description/meshes/base.dae", "", false);
  ASSERT_TRUE(m.resolved);
  // Extracted to a temp file; the bytes must round-trip.
  QFile f(QString::fromStdString(m.path));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), QByteArray("DAE-BYTES"));
}

// ---------------------------------------------------------------------------
// Chain step 1 — remembered per-MCAP map (QSettings)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step1_RememberedPerMcapMapHit) {
  ScopedSettings s;
  UrdfPackageResolver r;
  r.setSettings(s.get());
  r.setMcapPath("/data/run42.mcap");
  // rememberPackageRoot writes both the per-MCAP map and the global roots.
  r.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));

  // A fresh resolver reading the same settings + mcap path resolves via step 1.
  UrdfPackageResolver r2;
  r2.setSettings(s.get());
  r2.setMcapPath("/data/run42.mcap");
  const std::string p = r2.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// ---------------------------------------------------------------------------
// Chain step 2 — ancestor heuristic (walk up urdf_dir for basename == pkg)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step2_AncestorHeuristicHit) {
  UrdfPackageResolver r;
  // urdf_dir is inside ws/demo_description/ ; walking up finds the pkg-named
  // ancestor and verifies ancestor/rel exists.
  const std::string urdf_dir = fixtures() + "/ws/demo_description";
  const std::string p = r.resolve("demo_description", "meshes/anc.stl", urdf_dir, false);
  EXPECT_EQ(p, fixtures() + "/ws/demo_description/meshes/anc.stl");
}

TEST(UrdfResolver, Step2_AncestorRejectsWhenRelMissing) {
  UrdfPackageResolver r;
  const std::string urdf_dir = fixtures() + "/ws/demo_description";
  // The ancestor basename matches, but the rel file does not exist there → no
  // false-positive accept; falls through (and, with no roots, is unresolved).
  const std::string p = r.resolve("demo_description", "meshes/DOES_NOT_EXIST.stl", urdf_dir, false);
  EXPECT_TRUE(p.empty());
}

TEST(UrdfResolver, Step2_UrlAncestorHeuristic) {
  UrdfPackageResolver r;
  const std::string base = "https://example.com/repo/demo_description/urdf";
  const std::string p = r.resolve("demo_description", "meshes/base.dae", base, /*source_is_url=*/true);
  EXPECT_EQ(p, "https://example.com/repo/demo_description/meshes/base.dae");
}

// ---------------------------------------------------------------------------
// Chain step 3 — search roots (basename match, no package.xml)
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step3_SearchRootHit) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

TEST(UrdfResolver, Step3_AutoSeedFromEnv) {
  // Two entries joined with the native list separator (';' on Windows, where
  // absolute paths contain ':' and a colon split would shear the drive letter).
  const char sep = QDir::listSeparator().toLatin1();
  const std::string env_value = fixtures() + "/no_such_root" + sep + fixtures() + "/search_root";
  qputenv("ROS_PACKAGE_PATH", QByteArray::fromStdString(env_value));
  UrdfPackageResolver r;
  r.autoSeedSearchRoots("");  // no urdf dir; env supplies the roots
  qunsetenv("ROS_PACKAGE_PATH");
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// ---------------------------------------------------------------------------
// Chain step 4 — unresolved recorded for ask-once
// ---------------------------------------------------------------------------

TEST(UrdfResolver, Step4_UnresolvedRecorded) {
  UrdfPackageResolver r;  // no attachments, no settings, no roots
  const std::string p = r.resolve("ghost_pkg", "meshes/x.stl", "", false);
  EXPECT_TRUE(p.empty());
  ASSERT_EQ(r.unresolvedPackages().size(), 1);
  EXPECT_EQ(r.unresolvedPackages().first(), "ghost_pkg");
  // Idempotent: a second miss for the same pkg does not duplicate.
  r.resolve("ghost_pkg", "meshes/y.stl", "", false);
  EXPECT_EQ(r.unresolvedPackages().size(), 1);
}

// ---------------------------------------------------------------------------
// Stop-at-first-hit ordering: attachment beats search root.
// ---------------------------------------------------------------------------

TEST(UrdfResolver, AttachmentTakesPriorityOverSearchRoot) {
  UrdfPackageResolver r;
  r.addSearchRoot(QString::fromStdString(fixtures() + "/search_root"));
  QMap<QString, QByteArray> att;
  att.insert("package://demo_description/meshes/base.stl", QByteArray("ATTACHED"));
  r.setMcapAttachments(att);

  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  QFile f(QString::fromStdString(p));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), QByteArray("ATTACHED"));  // not the on-disk search-root file
}

// rememberPackageRoot persists globally + clears the unresolved entry.
TEST(UrdfResolver, RememberClearsUnresolvedAndPersists) {
  ScopedSettings s;
  UrdfPackageResolver r;
  r.setSettings(s.get());
  r.setMcapPath("/data/x.mcap");
  EXPECT_TRUE(r.resolve("demo_description", "meshes/base.stl", "", false).empty());
  EXPECT_TRUE(r.unresolvedPackages().contains("demo_description"));

  r.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));
  EXPECT_FALSE(r.unresolvedPackages().contains("demo_description"));
  // Now resolvable via the freshly-added global root.
  const std::string p = r.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

// rememberPackageRoot must enable cross-dataset (step-3) resolution: a FRESH
// resolver on a DIFFERENT mcap (so the per-MCAP map, step 1, misses) resolves
// via the persisted GLOBAL search root. Regression guard for the
// parent-vs-package-dir double-nesting bug (which made step 3 silently miss).
TEST(UrdfResolver, RememberEnablesCrossDatasetSearchRoot) {
  ScopedSettings s;
  UrdfPackageResolver r1;
  r1.setSettings(s.get());
  r1.setMcapPath("/data/first.mcap");
  r1.rememberPackageRoot("demo_description", QString::fromStdString(fixtures() + "/search_root/demo_description"));

  UrdfPackageResolver r2;
  r2.setSettings(s.get());
  r2.setMcapPath("/data/second.mcap");  // different mcap → step 1 cannot hit
  r2.autoSeedSearchRoots("");           // loads the persisted global root (the PARENT dir)
  const std::string p = r2.resolve("demo_description", "meshes/base.stl", "", false);
  EXPECT_EQ(p, fixtures() + "/search_root/demo_description/meshes/base.stl");
}

}  // namespace
}  // namespace pj::scene3d
