// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ExtensionCatalogService.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QMap>
#include <QSettings>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <utility>

#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
using namespace Qt::StringLiterals;

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

// Plugins bundled with an installed build. Per the FHS bin/lib split (the binary
// installs to <prefix>/bin, arch-dependent code to <prefix>/lib), the bundled
// plugins live at <prefix>/lib/plotjuggler/plugins, resolved relative to the
// executable so the install stays relocatable — the same path works under /usr,
// /usr/local, or a mounted AppImage. A dev build tree has no such directory, so
// it is simply skipped (buildScanHierarchy drops folders that don't exist);
// developers point at their freshly built plugins with --plugin-dir instead.
QString bundledPluginsDir() {
  return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../lib/plotjuggler/plugins"));
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
    const QString message = u"Failed to create extension staging directory \"%1\""_s.arg(pending_dir);
    qCWarning(lcCatalog) << message;
    reportDiagnostic(DiagnosticLevel::kError, message);
  }

  extension_manager_ = std::make_unique<ExtensionManager>(nullptr, extensions_dir_, pending_dir, sink_, this);

  // Seed bundled plugins into the marketplace dir (default mode only) BEFORE the
  // scan, so a plugin shipped in the installer becomes a normal marketplace-dir
  // install. Runs after the ExtensionManager applied pending staged installs
  // above, so a staged upgrade is promoted first and the seed leaves it intact.
  // With seeding, the bundled dir stops being a live scan tier (see the
  // include_bundled argument below), making the marketplace dir the single source
  // of truth so a seeded plugin never reads as "loaded but not installed".
  if (use_defaults) {
    seedBundledPlugins();
  }

  plugin_catalog_ = std::make_unique<PluginRuntimeCatalog>(std::filesystem::path{}, sink_, "ExtensionCatalogService");
  // Gauge each plugin's min_plotjuggler_version against the version the app
  // advertises. Only breaks ties between duplicate plugin ids (see
  // PluginRuntimeCatalog::setHostVersion).
  plugin_catalog_->setHostVersion(QCoreApplication::applicationVersion().toStdString());

  // Scan the full ordered folder hierarchy (custom folders have highest priority;
  // the catalog de-duplicates by plugin id — authoritative entries override, then
  // compatibility, then version, then folder priority).
  std::vector<PluginDirEntry> scan_dirs = buildScanHierarchy(!use_defaults, /*include_bundled=*/!use_defaults);
  const auto scan_dir_count = scan_dirs.size();
  plugin_catalog_->setPluginDirs(std::move(scan_dirs));

  qCInfo(lcCatalog) << "Scanning" << static_cast<int>(scan_dir_count) << "plugin folder(s); install dir"
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
  folders << bundledPluginsDir();
  return folders;
}

std::vector<PluginDirEntry> ExtensionCatalogService::buildScanHierarchy(
    bool extensions_dir_is_explicit, bool include_bundled) const {
  std::vector<PluginDirEntry> dirs;
  // A folder that doesn't exist on disk contributes no plugins, so skip it
  // rather than hand it to the catalog — scanning a missing directory reports a
  // kError per launch, which for an absent *optional* folder (e.g. the bundled
  // <prefix>/lib/plotjuggler/plugins path in a dev build tree, or a user-typed
  // custom folder already shown in red in the Preferences page) is noise that
  // masks real plugin-load errors.
  const auto add_if_exists = [&dirs](const QString& folder, bool authoritative) {
    if (!folder.isEmpty() && QDir(folder).exists()) {
      dirs.push_back({std::filesystem::path(folder.toStdString()), authoritative});
    }
  };
  // The user-explicit tiers are authoritative (a hard override in the dedup):
  // the custom folders and, when set, the --plugin-dir override (extensions_dir_
  // is the marketplace default only when no --plugin-dir was given). Marketplace
  // and bundled folders stay managed, so version/compatibility decide among them.
  for (const QString& folder : customPluginFolders()) {
    add_if_exists(folder, true);
  }
  const QString bundled = bundledPluginsDir();
  for (const QString& folder : builtinPluginFolders()) {
    // Once bundled plugins are seeded into the marketplace dir they load from
    // there; keeping the bundled dir as a live scan tier would surface a seeded
    // plugin from two folders and, worse, resurrect one the user uninstalled.
    if (!include_bundled && folder == bundled) {
      continue;
    }
    add_if_exists(folder, extensions_dir_is_explicit && folder == extensions_dir_);
  }
  return dirs;
}

void ExtensionCatalogService::seedBundledPlugins() {
  const QString bundled = bundledPluginsDir();
  if (!QDir(bundled).exists()) {
    return;  // dev build tree, or an install with no bundled plugins — nothing to seed
  }

  const auto scan = scanPluginDsos(std::filesystem::path(bundled.toStdString()));
  if (!scan) {
    reportDiagnostic(
        DiagnosticLevel::kWarning,
        u"Could not scan bundled plugins at \"%1\": %2"_s.arg(bundled, QString::fromStdString(scan.error())));
    return;
  }

  const std::filesystem::path bundled_root(bundled.toStdString());

  // Every bundled id -> version, whether or not it gets copied this run. Handed to
  // the ExtensionManager so it can lock uninstall of "core" plugins by id and
  // offer "downgrade to bundled" when the installed version is above the bundled
  // one — no per-folder marker.
  QMap<QString, QString> bundled_versions;

  for (const PluginDescriptor& descriptor : scan->plugins) {
    const QString id = QString::fromStdString(descriptor.id);
    if (id.isEmpty()) {
      continue;
    }
    bundled_versions.insert(id, QString::fromStdString(descriptor.version));
    const QString dest_dir = extensions_dir_ + "/" + id;

    // Already present in the marketplace dir (a prior seed, or a user install):
    // leave it untouched. The folder's existence is the record — no re-copy.
    if (QDir(dest_dir).exists()) {
      continue;
    }

    // First sight of this id: copy its bundled payload into the marketplace dir.
    // The source is the top-level entry under the bundled dir that contains the
    // DSO — a per-id subdirectory (e.g. csv-loader/, or ros2-topic-subscriber/
    // with its dist/<distro>/ inners) or, for a flat layout, the .so file itself.
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(bundled_root, ec);
    const std::filesystem::path dso = std::filesystem::weakly_canonical(descriptor.dso_path, ec);
    const std::filesystem::path rel = std::filesystem::relative(dso, root, ec);
    if (ec || rel.empty()) {
      reportDiagnostic(
          DiagnosticLevel::kWarning, u"Skipped bundled plugin \"%1\": cannot resolve its path"_s.arg(id), id);
      continue;
    }
    const std::filesystem::path src = root / *rel.begin();
    const std::filesystem::path dst(dest_dir.toStdString());

    if (std::filesystem::is_directory(src, ec)) {
      std::filesystem::create_directories(dst, ec);
      std::filesystem::copy(
          src, dst, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    } else {
      std::filesystem::create_directories(dst, ec);
      std::filesystem::copy_file(dso, dst / dso.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    }

    if (ec) {
      // Best-effort: drop the half-written dir and retry next launch. In default
      // mode bundled is not a scan source, so a failed seed just means this
      // plugin is absent this session.
      std::error_code cleanup_ec;
      std::filesystem::remove_all(dst, cleanup_ec);
      reportDiagnostic(
          DiagnosticLevel::kWarning,
          u"Failed to seed bundled plugin \"%1\" into \"%2\": %3"_s.arg(
              id, dest_dir, QString::fromStdString(ec.message())),
          id);
      continue;
    }

    qCInfo(lcCatalog) << "Seeded bundled plugin" << id << "into" << dest_dir;
  }

  // Hand the full bundled id -> version map to the ExtensionManager so it locks
  // uninstall of these "core" plugins (isBundled) and can offer downgrade-to-bundled.
  extension_manager_->setBundledVersions(bundled_versions);
}

ExtensionCatalogService::~ExtensionCatalogService() = default;

void ExtensionCatalogService::reload() {
  bool changed = false;
  {
    // Exclusive: reload() clears/reallocates the catalog's parser vector. Any
    // poll thread resolving a parser through the shared-lock accessors is
    // fenced out for the duration.
    const std::unique_lock lock(catalog_mutex_);
    changed = plugin_catalog_->reload();
  }
  if (changed) {
    emit catalogChanged();
  }
}

MessageParserHandle ExtensionCatalogService::createParserHandleForEncoding(QStringView encoding) const {
  const std::shared_lock lock(catalog_mutex_);
  const LoadedMessageParser* parser = plugin_catalog_->findParserByEncoding(encoding.toString().toStdString());
  if (parser == nullptr) {
    return MessageParserHandle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  }
  // createHandle() copies the library's DSO keepalive into the returned handle,
  // so the handle stays valid after the lock is released even if a later
  // reload() drops this catalog entry.
  return parser->library.createHandle();
}

std::vector<std::string> ExtensionCatalogService::parserEncodings() const {
  const std::shared_lock lock(catalog_mutex_);
  std::vector<std::string> encodings;
  for (const auto& parser : plugin_catalog_->messageParsers()) {
    for (const auto& encoding : parser.encodings) {
      encodings.push_back(encoding);
    }
  }
  std::sort(encodings.begin(), encodings.end());
  encodings.erase(std::unique(encodings.begin(), encodings.end()), encodings.end());
  return encodings;
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
