#pragma once

#include <QObject>
#include <QString>

#include "pj_base/types.hpp"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class SessionManager;

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

  // Programmatic entry point. Returns true on successful ingest.
  // Emits fileLoaded() on success and fileLoadFailed() on failure.
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);

 signals:
  void fileLoaded(const QString& path);
  void fileLoadFailed(const QString& path, const QString& reason);

 private:
  // Lazy-creates a single shared "default" time domain on first import and
  // returns its id; subsequent calls return the cached id. Returns 0 if the
  // engine refuses creation (caller surfaces the error).
  TimeDomainId ensureDefaultTimeDomainId();

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  TimeDomainId default_time_domain_id_ = 0;
};

}  // namespace PJ
