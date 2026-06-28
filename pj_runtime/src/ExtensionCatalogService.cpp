// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ExtensionCatalogService.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QSettings>
#include <utility>

#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcCatalog, "pj.app_core.extensions")

QString defaultExtensionsDir() {
  return PlatformUtils::extensionsDir();
}

QString defaultPendingDir() {
  return PlatformUtils::pendingDir();
}

// User-managed extra plugin folders (Preferences page). QStringList.
constexpr auto kCustomPluginFoldersKey = "Preferences::plugin_folders";

QString executablePluginsDir() {
  return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
}
}  // namespace

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, QObject* parent)
    : ExtensionCatalogService(std::move(extensions_dir), DiagnosticSink{}, parent) {}

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : QObject(parent), sink_(std::move(sink)) {
  const bool use_defaults = extensions_dir.isEmpty();
  extensions_dir_ = use_defaults ? defaultExtensionsDir() : std::move(extensions_dir);
  const QString pending_dir = use_defaults ? defaultPendingDir() : extensions_dir_ + "/.pending";

  if (!QDir().mkpath(extensions_dir_)) {
    qCWarning(lcCatalog) << "Failed to create extensions directory" << extensions_dir_
                         << "- plugin loading will be a no-op until it exists.";
  }
  if (!QDir().mkpath(pending_dir)) {
    const QString message = QStringLiteral("Failed to create extension staging directory \"%1\"").arg(pending_dir);
    qCWarning(lcCatalog) << message;
    reportDiagnostic(DiagnosticLevel::kError, message);
  }

  extension_manager_ = std::make_unique<ExtensionManager>(nullptr, extensions_dir_, pending_dir, sink_, this);
  plugin_catalog_ = std::make_unique<PluginRuntimeCatalog>(std::filesystem::path{}, sink_, "ExtensionCatalogService");

  // Scan the full ordered folder hierarchy (custom folders win over the
  // built-ins; the catalog de-duplicates by plugin id, first folder wins).
  const std::vector<std::filesystem::path> scan_dirs = buildScanHierarchy();
  plugin_catalog_->setPluginDirs(scan_dirs);
  qCInfo(lcCatalog) << "Scanning" << static_cast<int>(scan_dirs.size()) << "plugin folder(s); install dir"
                    << extensions_dir_;
  plugin_catalog_->scanDirectory();
}

QStringList ExtensionCatalogService::customPluginFolders() const {
  QSettings settings;
  return settings.value(QLatin1String(kCustomPluginFoldersKey)).toStringList();
}

void ExtensionCatalogService::setCustomPluginFolders(const QStringList& folders) {
  QSettings settings;
  settings.setValue(QLatin1String(kCustomPluginFoldersKey), folders);
}

QStringList ExtensionCatalogService::builtinPluginFolders() const {
  QStringList folders;
  folders << extensions_dir_;
  const QString marketplace = defaultExtensionsDir();
  if (marketplace != extensions_dir_) {
    folders << marketplace;
  }
  folders << executablePluginsDir();
  return folders;
}

std::vector<std::filesystem::path> ExtensionCatalogService::buildScanHierarchy() const {
  std::vector<std::filesystem::path> dirs;
  // A folder that doesn't exist on disk contributes no plugins, so skip it
  // rather than hand it to the catalog — scanning a missing directory reports a
  // kError per launch, which for an absent *optional* folder (e.g. <exe>/plugins
  // in a dev layout, or a user-typed custom folder already shown in red in the
  // Preferences page) is noise that masks real plugin-load errors.
  const auto add_if_exists = [&dirs](const QString& folder) {
    if (!folder.isEmpty() && QDir(folder).exists()) {
      dirs.emplace_back(folder.toStdString());
    }
  };
  for (const QString& folder : customPluginFolders()) {
    add_if_exists(folder);
  }
  for (const QString& folder : builtinPluginFolders()) {
    add_if_exists(folder);
  }
  return dirs;
}

ExtensionCatalogService::~ExtensionCatalogService() = default;

void ExtensionCatalogService::reload() {
  if (plugin_catalog_->reload()) {
    emit catalogChanged();
  }
}

const std::vector<LoadedDataSource>& ExtensionCatalogService::dataSources() const {
  return plugin_catalog_->dataSources();
}

const std::vector<LoadedMessageParser>& ExtensionCatalogService::messageParsers() const {
  return plugin_catalog_->messageParsers();
}

const std::vector<LoadedToolbox>& ExtensionCatalogService::toolboxes() const {
  return plugin_catalog_->toolboxes();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::fileImportSources() const {
  const auto& catalog = *plugin_catalog_;
  return catalog.fileImportSources();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::streamSources() const {
  const auto& catalog = *plugin_catalog_;
  return catalog.streamSources();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::findSourcesForExtension(QStringView ext) const {
  const auto& catalog = *plugin_catalog_;
  return catalog.findSourcesForExtension(ext.toString().toStdString());
}

const LoadedMessageParser* ExtensionCatalogService::findParserByEncoding(QStringView encoding) const {
  const auto& catalog = *plugin_catalog_;
  return catalog.findParserByEncoding(encoding.toString().toStdString());
}

QString ExtensionCatalogService::buildFileFilter() const {
  return QString::fromStdString(plugin_catalog_->buildFileFilter());
}

void ExtensionCatalogService::reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id) const {
  if (!sink_) {
    return;
  }
  sink_(
      Diagnostic{
          level,
          "ExtensionCatalogService",
          id.toStdString(),
          message.toStdString(),
          std::chrono::system_clock::now(),
      });
}

}  // namespace PJ
