#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/toolbox_library.hpp"

namespace PJ {

class DownloadManager;
class ExtensionManager;

struct LoadedDataSource {
  DataSourceLibrary library;
  QString path;
  QString name;
  QStringList file_extensions;
  uint64_t capabilities = 0;
  std::filesystem::file_time_type loaded_mtime;
};

struct LoadedMessageParser {
  MessageParserLibrary library;
  QString path;
  QString name;
  QStringList encodings;
  std::filesystem::file_time_type loaded_mtime;
};

struct LoadedToolbox {
  ToolboxLibrary library;
  QString path;
  QString name;
  uint64_t capabilities = 0;
  std::filesystem::file_time_type loaded_mtime;
};

// Bundles the marketplace (ExtensionManager) with the plugin catalog (per-family
// loaders from pj_plugins) behind one Qt-friendly facade. `pj_app` never touches
// pj_marketplace or pj_plugins directly — it asks this service.
//
// The service owns an ExtensionManager rooted on `extensions_dir_` (by default
// QStandardPaths::AppDataLocation + "/extensions"). At construction it applies
// any pending Windows staging actions and then scans the directory. Call
// reload() after a marketplace install/uninstall to hot-load new plugins.
class ExtensionCatalogService : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<ExtensionCatalogService>;

  // If `extensions_dir` is empty, defaults to
  //   QStandardPaths::AppDataLocation + "/extensions"
  // (on Linux: ~/.local/share/PlotJuggler4/extensions/). Pass a custom path
  // for tests.
  explicit ExtensionCatalogService(QString extensions_dir = {}, QObject* parent = nullptr);
  ~ExtensionCatalogService() override;

  ExtensionCatalogService(const ExtensionCatalogService&) = delete;
  ExtensionCatalogService& operator=(const ExtensionCatalogService&) = delete;

  // Reference valid for the service's lifetime.
  ExtensionManager& extensionManager() const { return *extension_manager_; }

  QString extensionsDir() const { return extensions_dir_; }

  // Rescan the extensions directory, unload dropped plugins, reload
  // mtime-changed ones, load new ones. Emits catalogChanged() if the catalog
  // actually changed.
  void reload();

  const std::vector<LoadedDataSource>& dataSources() const { return data_sources_; }
  const std::vector<LoadedMessageParser>& messageParsers() const { return message_parsers_; }
  const std::vector<LoadedToolbox>& toolboxes() const { return toolboxes_; }

  std::vector<const LoadedDataSource*> fileImportSources() const;
  std::vector<const LoadedDataSource*> streamSources() const;
  std::vector<const LoadedDataSource*> findSourcesForExtension(QStringView ext) const;
  const LoadedMessageParser* findParserByEncoding(QStringView encoding) const;

  // Builds a QFileDialog-compatible filter string from all file-import sources.
  QString buildFileFilter() const;

 signals:
  void catalogChanged();

 private:
  bool loadAndRegisterDataSource(const std::filesystem::path& so_path);
  bool loadAndRegisterMessageParser(const std::filesystem::path& so_path);
  bool loadAndRegisterToolbox(const std::filesystem::path& so_path);
  bool tryLoadAny(const std::filesystem::path& so_path);
  // Removes any loaded entry whose path matches `path`. Returns true if an
  // entry was removed. Used by reload() to evict stale / family-transitioned
  // entries before re-loading.
  bool evictByPath(const QString& path);
  void scanOnce();

  QString extensions_dir_;

  // download_manager_ is declared before extension_manager_ so it outlives it;
  // ExtensionManager holds a raw DownloadManager* from here.
  std::unique_ptr<DownloadManager> download_manager_;
  std::unique_ptr<ExtensionManager> extension_manager_;

  std::vector<LoadedDataSource> data_sources_;
  std::vector<LoadedMessageParser> message_parsers_;
  std::vector<LoadedToolbox> toolboxes_;
};

}  // namespace PJ
