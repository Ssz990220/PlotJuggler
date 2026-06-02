// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ExtensionCatalogService.h"

#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <utility>

#include "pj_marketplace/extension_manager.hpp"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcCatalog, "pj.app_core.extensions")

QString defaultExtensionsDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/extensions";
}

QString defaultPendingDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/.extension_staging";
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
  plugin_catalog_ =
      std::make_unique<PluginRuntimeCatalog>(extensions_dir_.toStdString(), sink_, "ExtensionCatalogService");

  qCInfo(lcCatalog) << "Scanning" << extensions_dir_;
  plugin_catalog_->scanDirectory();
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
