#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <functional>
#include <unordered_map>

#include "pj_base/types.hpp"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class SessionManager;

// Hints supplied by callers that already know what plugin to use and what
// config to apply (e.g. layout-driven reload). When skip_dialog is true and
// the layout's preset_config_json applies cleanly to the matching plugin,
// FileLoader::loadFile bypasses the data-source dialog entirely. On any
// failure (id mismatch, loadConfig rejection), the dialog falls back open
// with the existing QSettings-based pre-fill.
struct LoadHints {
  QString expected_plugin_id;  // Empty -> no hint; FileLoader picks plugin by extension as usual.
  QString preset_config_json;  // Empty -> no hint; QSettings pre-fill is used.
  bool skip_dialog = false;    // Only honored when both fields above are non-empty AND the plugin id matches.
  // Layout replay reuses matching DatasetIds; normal load/reload replaces them.
  bool prefer_reuse = false;
};

// Drives the file-import path: pick a file, find the matching DataSource
// plugin via ExtensionCatalogService, ingest into the SessionManager's data
// engine, and refresh the curve catalog. Lives in pj_app because it talks to
// QFileDialog/QMessageBox and to pj_marketplace's plugin handles.
class FileLoader : public QObject {
  Q_OBJECT
 public:
  FileLoader(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent = nullptr);
  ~FileLoader() override;

  FileLoader(const FileLoader&) = delete;
  FileLoader& operator=(const FileLoader&) = delete;

  // Opens a file dialog filtered by every installed file-import plugin's
  // extensions. On accept, runs loadFile() with the chosen path. The chosen
  // directory is persisted in QSettings under "FileLoader/lastDir".
  void openFromDialog(QWidget* dialog_parent);

  // Resolves the "pick a file" interaction inside openFromDialog(). The shell
  // injects one that threads MainWindow's chrome metrics into PJ::FileDialog,
  // keeping FileLoader free of a MainWindow link (which also makes it testable
  // headlessly). Unset -> plain PJ::FileDialog::getOpenFileName, no metrics.
  using FilePicker =
      std::function<QString(QWidget* parent, const QString& caption, const QString& dir, const QString& filter)>;
  void setFilePicker(FilePicker picker) {
    file_picker_ = std::move(picker);
  }

  // Programmatic entry point. Returns true on successful ingest.
  // Emits fileLoaded() on success and fileLoadFailed() on failure.
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);
  bool loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints);

  // 3D TF ingest is triggered at load time through this service (owned by the
  // app shell, not the domain-neutral runtime). When unset, TF ingest is
  // skipped — non-3D builds simply never set it.
  void setTransformService(pj::scene3d::TransformService* service) {
    transform_service_ = service;
  }

  // Full filesystem path the given dataset was loaded from, or empty if this
  // loader did not create it (e.g. a streaming or test dataset, or an id it has
  // since forgotten). The shell uses this to translate a DatasetId back to the
  // SessionManager loaded-source entry when a dataset is removed.
  [[nodiscard]] QString sourcePathForDataset(DatasetId dataset_id) const;
  // Drop the dataset->path association after the dataset is removed, so the map
  // does not retain ids the engine no longer has. Safe to call for unknown ids.
  void untrackDataset(DatasetId dataset_id);

 signals:
  void fileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json);
  void fileLoadFailed(const QString& path, const QString& reason);

 private:
  // Lazy-creates a single shared "default" time domain on first import and
  // returns its id; subsequent calls return the cached id. Returns 0 if the
  // engine refuses creation (caller surfaces the error).
  TimeDomainId ensureDefaultTimeDomainId();

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  FilePicker file_picker_;
  TimeDomainId default_time_domain_id_ = 0;
  pj::scene3d::TransformService* transform_service_ = nullptr;
  // Full path each loaded dataset came from. The engine identifies datasets by
  // basename (DatasetInfo::source_name) only, so the same-source match below
  // consults this to keep two different files that share a basename distinct
  // instead of aliasing the second onto the first. DatasetIds are monotonic and
  // never recycled, so a stale entry for a removed id can never mis-resolve.
  std::unordered_map<DatasetId, QString> dataset_source_path_;
};

}  // namespace PJ
