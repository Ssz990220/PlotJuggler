#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
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
// QStandardPaths::AppDataLocation + "/extensions"). At construction it applies
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

  // Finds a MessageParser by encoding name.
  const LoadedMessageParser* findParserByEncoding(QStringView encoding) const;

  // Builds a QFileDialog-compatible filter string from all file-import sources.
  QString buildFileFilter() const;

 signals:
  // Emitted after reload() changes the loaded plugin set.
  void catalogChanged();

 private:
  // Emits one diagnostic through the optional app-level sink.
  void reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id = {}) const;

  QString extensions_dir_;
  DiagnosticSink sink_;

  std::unique_ptr<ExtensionManager> extension_manager_;
  std::unique_ptr<PluginRuntimeCatalog> plugin_catalog_;
};

}  // namespace PJ
