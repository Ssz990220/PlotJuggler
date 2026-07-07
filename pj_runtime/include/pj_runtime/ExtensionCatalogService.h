#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/plugin_runtime_catalog.hpp"

namespace PJ {

class ExtensionManager;

using LoadedDataSource = RuntimeDataSourcePlugin;
using LoadedMessageParser = RuntimeMessageParserPlugin;
using LoadedToolbox = RuntimeToolboxPlugin;

// Bundles the marketplace (ExtensionManager) with the plugin catalog (per-family
// loaders from pj_plugins) behind one Qt-friendly facade. `pj_app` never touches
// pj_marketplace or pj_plugins directly — it asks this service.
//
// The service owns an ExtensionManager rooted on `extensions_dir_` (by default
// PlatformUtils::extensionsDir(), shared with pj_marketplace). At construction it applies
// any pending Windows staging actions and then scans the directory. Call
// reload() after a marketplace install/uninstall to hot-load new plugins.
class ExtensionCatalogService : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<ExtensionCatalogService>;

  // Creates a service using the default extension directory unless overridden.
  explicit ExtensionCatalogService(QString extensions_dir = {}, QObject* parent = nullptr);

  // Creates a service with an optional app-level diagnostic sink.
  ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  // Releases marketplace and loaded plugin resources.
  ~ExtensionCatalogService() override;

  // ExtensionCatalogService owns loaded plugin libraries and cannot be copied.
  ExtensionCatalogService(const ExtensionCatalogService&) = delete;

  // ExtensionCatalogService owns loaded plugin libraries and cannot be assigned.
  ExtensionCatalogService& operator=(const ExtensionCatalogService&) = delete;

  // Reference valid for the service's lifetime.
  ExtensionManager& extensionManager() const {
    return *extension_manager_;
  }

  // Returns the directory where extension DSOs are loaded from.
  QString extensionsDir() const {
    return extensions_dir_;
  }

  // Reconciles the loaded plugin catalog with files on disk.
  void reload();

  // Returns all loaded DataSource plugins.
  const std::vector<LoadedDataSource>& dataSources() const;

  // Returns all loaded MessageParser plugins.
  const std::vector<LoadedMessageParser>& messageParsers() const;

  // Returns all loaded Toolbox plugins.
  const std::vector<LoadedToolbox>& toolboxes() const;

  // Returns file-import capable DataSource plugins.
  std::vector<const LoadedDataSource*> fileImportSources() const;

  // Returns streaming-capable DataSource plugins.
  std::vector<const LoadedDataSource*> streamSources() const;

  // Finds file-import DataSources that handle ext.
  std::vector<const LoadedDataSource*> findSourcesForExtension(QStringView ext) const;

  // Finds a MessageParser by encoding name. Returns a raw pointer INTO the
  // catalog vector, valid only until the next reload(). GUI-THREAD ONLY — a
  // concurrent reload() (which reallocates the vector) would dangle it. Off-GUI
  // callers (a streaming source's poll thread) must use
  // createParserHandleForEncoding()/parserEncodings() instead, which resolve
  // under the catalog lock and never leak a raw catalog pointer.
  const LoadedMessageParser* findParserByEncoding(QStringView encoding) const;

  // [thread-safe] Resolve a parser by encoding and create an instance of it,
  // atomically under a shared catalog lock. The returned MessageParserHandle
  // carries its own DSO keepalive, so it stays valid even if a later reload()
  // drops the catalog entry. An invalid handle (`!valid()`) means no parser
  // handles that encoding. This is the ONLY safe way for a non-GUI thread to
  // obtain a parser while reload() may run on the GUI thread.
  [[nodiscard]] MessageParserHandle createParserHandleForEncoding(QStringView encoding) const;

  // [thread-safe] The set of encodings the loaded parsers accept, returned by
  // value (a snapshot copy) so a caller never holds a reference into the
  // catalog vector across a reload(). Sorted, de-duplicated.
  [[nodiscard]] std::vector<std::string> parserEncodings() const;

  // Builds a QFileDialog-compatible filter string from all file-import sources.
  QString buildFileFilter() const;

  // User-managed extra plugin folders, highest scan priority, persisted in
  // QSettings (Preferences::plugin_folders). Changes apply on next launch (no
  // hot reload), so the setter only writes the key — it does not re-scan.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);

  // Built-in plugin folders in scan-priority order: the install dir
  // (the --plugin-dir override or the marketplace location), the marketplace
  // location (only when the override made it distinct), then <exe>/plugins.
  // Read-only — shown to the user for reference.
  [[nodiscard]] QStringList builtinPluginFolders() const;

 signals:
  // Emitted after reload() changes the loaded plugin set.
  void catalogChanged();

 private:
  // Emits one diagnostic through the optional app-level sink.
  void reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id = {}) const;

  // Assembles the ordered scan list: custom folders first, then the built-in
  // folders. De-duplication by plugin id (first folder wins) is done in the
  // PluginRuntimeCatalog.
  [[nodiscard]] std::vector<std::filesystem::path> buildScanHierarchy() const;

  QString extensions_dir_;
  DiagnosticSink sink_;

  std::unique_ptr<ExtensionManager> extension_manager_;
  std::unique_ptr<PluginRuntimeCatalog> plugin_catalog_;

  // Guards plugin_catalog_'s vectors against the one genuine cross-thread
  // hazard: a streaming source's poll thread resolving a parser while the GUI
  // thread reloads the catalog (Marketplace install/uninstall). reload() takes
  // the exclusive lock; the thread-safe accessors take a shared lock. GUI-only
  // readers (findParserByEncoding, dataSources, buildFileFilter, …) do not
  // lock — they cannot race reload(), which is also GUI-thread-only.
  mutable std::shared_mutex catalog_mutex_;
};

}  // namespace PJ
