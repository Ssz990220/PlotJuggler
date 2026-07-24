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
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <utility>

#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/version_compare.hpp"
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

// Plugins bundled with an installed build (the "share" dir). Per the FHS
// bin/lib split (the binary installs to <prefix>/bin, arch-dependent code to
// <prefix>/lib), the bundled plugins live at <prefix>/lib/plotjuggler/plugins,
// resolved relative to the executable so the install stays relocatable — the
// same path works under /usr, /usr/local, or a mounted AppImage. It is a seed
// source only (see seedBundledPlugins), never a scanned load path. A dev build
// tree has no such directory, so seeding is a no-op there; developers point at
// their freshly built plugins with --plugin-dir instead.
QString bundledPluginsDir() {
  return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../lib/plotjuggler/plugins"));
}

// Copies one bundled payload (a per-id directory, or a lone DSO file) into the
// fresh staging dir stage_new. Returns the first failing step's error code
// ({} on success).
std::error_code stagePayload(
    const std::filesystem::path& src_entry, const std::filesystem::path& dso, const std::filesystem::path& stage_new) {
  std::error_code ec;
  std::filesystem::create_directories(stage_new, ec);
  if (ec) {
    return ec;
  }
  if (std::filesystem::is_directory(src_entry, ec)) {
    std::filesystem::copy(
        src_entry, stage_new,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
  } else if (!ec) {
    std::filesystem::copy_file(dso, stage_new / dso.filename(), std::filesystem::copy_options::overwrite_existing, ec);
  }
  return ec;
}

// Promotes stage_new into dst, moving any existing dst aside first. On a failed
// promote the old copy is renamed back; if even that restore fails,
// *aside_orphaned is set — the aside dir then holds the only surviving copy and
// the caller must preserve it. Returns the failing step's error code ({} on
// success).
std::error_code promoteStaged(
    const std::filesystem::path& stage_new, const std::filesystem::path& dst, const std::filesystem::path& aside,
    bool* aside_orphaned) {
  std::error_code ec;
  std::error_code exists_ec;
  const bool had_old = std::filesystem::exists(dst, exists_ec);
  if (had_old) {
    std::filesystem::rename(dst, aside, ec);
    if (ec) {
      return ec;
    }
  }
  std::filesystem::rename(stage_new, dst, ec);
  if (ec && had_old) {
    std::error_code restore_ec;
    std::filesystem::rename(aside, dst, restore_ec);
    *aside_orphaned = static_cast<bool>(restore_ec);
  }
  return ec;
}

// Where the bundled DSO lands inside the installed per-id copy: the payload's
// inner layout is preserved by the copy, so it is `rel` minus its leading
// (payload-entry) component; a flat payload keeps just the file name.
std::filesystem::path installedDsoPath(const std::filesystem::path& dst, const std::filesystem::path& rel) {
  // Purely lexical — lexically_relative, not std::filesystem::relative, which
  // would touch the filesystem. A flat payload (rel is just the file name)
  // yields "." and keeps the whole rel.
  const std::filesystem::path inner = rel.lexically_relative(*rel.begin());
  return dst / (inner == "." ? rel : inner);
}

// True when the installed copy of a bundled DSO is provably the same build:
// same size and same mtime (the seed stamps the installed DSO with the bundled
// one's mtime after every copy, so this holds in the steady state). A hit lets
// the seed skip dlopening the installed copy to re-read its manifest version;
// any doubt — missing file, differing signature, filesystem error — falls back
// to the manifest comparison.
bool sameDsoSignature(const std::filesystem::path& installed_dso, const std::filesystem::path& bundled_dso) {
  std::error_code ec;
  const auto installed_size = std::filesystem::file_size(installed_dso, ec);
  if (ec) {
    return false;
  }
  const auto bundled_size = std::filesystem::file_size(bundled_dso, ec);
  if (ec || installed_size != bundled_size) {
    return false;
  }
  const auto installed_time = std::filesystem::last_write_time(installed_dso, ec);
  if (ec) {
    return false;
  }
  const auto bundled_time = std::filesystem::last_write_time(bundled_dso, ec);
  return !ec && installed_time == bundled_time;
}

// Manifest version of the copy of `id` installed under dir, or nullopt when the
// folder is absent or holds no loadable plugin with that id (a gutted or
// half-written copy) — both mean "reseed".
std::optional<std::string> installedPluginVersion(const std::filesystem::path& dir, const std::string& id) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return std::nullopt;
  }
  const auto scan = scanPluginDsos(dir);
  if (!scan) {
    return std::nullopt;
  }
  for (const PluginDescriptor& descriptor : scan->plugins) {
    if (descriptor.id == id) {
      return descriptor.version;
    }
  }
  return std::nullopt;
}
}  // namespace

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, QObject* parent)
    : ExtensionCatalogService(std::move(extensions_dir), DiagnosticSink{}, parent) {}

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : ExtensionCatalogService(Paths{std::move(extensions_dir), {}, {}}, std::move(sink), parent) {}

ExtensionCatalogService::ExtensionCatalogService(Paths paths, DiagnosticSink sink, QObject* parent)
    : QObject(parent), sink_(std::move(sink)) {
  default_mode_ = paths.install_dir.isEmpty();
  const bool default_marketplace = paths.marketplace_dir.isEmpty();
  marketplace_dir_ = default_marketplace ? defaultExtensionsDir() : std::move(paths.marketplace_dir);
  extensions_dir_ = default_mode_ ? marketplace_dir_ : std::move(paths.install_dir);
  bundled_dir_ = paths.bundled_dir.isEmpty() ? bundledPluginsDir() : std::move(paths.bundled_dir);
  // The staging dir must live with the managed dir it stages for; the platform
  // default is used only when the managed dir itself is the platform default.
  const QString pending_dir =
      (default_mode_ && default_marketplace) ? defaultPendingDir() : extensions_dir_ + "/.pending";

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

  // Sync bundled plugins into the marketplace dir BEFORE the scan — the bundled
  // dir is never scanned, so the seed is the only route by which shipped
  // plugins load. Runs in every mode, after the ExtensionManager applied
  // pending staged installs above, so a staged upgrade is promoted before the
  // seed's version comparison sees it.
  seedBundledPlugins();

  plugin_catalog_ = std::make_unique<PluginRuntimeCatalog>(std::filesystem::path{}, sink_, "ExtensionCatalogService");
  // Gauge each plugin's min_plotjuggler_version against the version the app
  // advertises. Only breaks ties between duplicate plugin ids (see
  // PluginRuntimeCatalog::setHostVersion).
  plugin_catalog_->setHostVersion(QCoreApplication::applicationVersion().toStdString());

  // Scan the ordered folder hierarchy (--plugin-dir override first, then custom
  // folders, then the marketplace dir; the catalog de-duplicates by plugin id —
  // authoritative entries override, then compatibility, then version, then
  // folder priority).
  std::vector<PluginDirEntry> scan_dirs = buildScanHierarchy(!default_mode_);
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
  if (marketplace_dir_ != extensions_dir_) {
    folders << marketplace_dir_;
  }
  return folders;
}

std::vector<PluginDirEntry> ExtensionCatalogService::buildScanHierarchy(bool extensions_dir_is_explicit) const {
  std::vector<PluginDirEntry> dirs;
  // A folder that doesn't exist on disk contributes no plugins, so skip it
  // rather than hand it to the catalog — scanning a missing directory reports a
  // kError per launch, which for an absent *optional* folder (e.g. a user-typed
  // custom folder already shown in red in the Preferences page) is noise that
  // masks real plugin-load errors.
  const auto add_if_exists = [&dirs](const QString& folder, bool authoritative) {
    if (!folder.isEmpty() && QDir(folder).exists()) {
      dirs.push_back({std::filesystem::path(folder.toStdString()), authoritative});
    }
  };
  // Priority order: the --plugin-dir override outranks everything, then the
  // custom Preferences folders, then the managed marketplace dir. The
  // user-explicit tiers are authoritative — a version-blind hard override in
  // the catalog's duplicate-id resolution — so an explicitly given folder wins
  // even when it holds an older build.
  if (extensions_dir_is_explicit) {
    add_if_exists(extensions_dir_, true);
  }
  for (const QString& folder : customPluginFolders()) {
    add_if_exists(folder, true);
  }
  add_if_exists(marketplace_dir_, false);
  return dirs;
}

void ExtensionCatalogService::seedBundledPlugins() {
  // Refresh staging area: a SIBLING of the marketplace dir, so the promote
  // rename never crosses a filesystem, yet an interrupted seed can never leak
  // a stale payload into the plugin scan (which walks the marketplace dir
  // recursively). Leftovers from a crashed launch are cleared before anything
  // else — even when the bundled dir is absent this run.
  const std::filesystem::path market_root(marketplace_dir_.toStdString());
  const std::filesystem::path stage_root((marketplace_dir_ + u".seed_stage"_s).toStdString());
  std::error_code ec;
  std::filesystem::remove_all(stage_root, ec);

  if (!QDir(bundled_dir_).exists()) {
    return;  // dev build tree, or an install with no bundled plugins — nothing to seed
  }

  const auto scan = scanPluginDsos(std::filesystem::path(bundled_dir_.toStdString()));
  if (!scan) {
    reportDiagnostic(
        DiagnosticLevel::kWarning,
        u"Could not scan bundled plugins at \"%1\": %2"_s.arg(bundled_dir_, QString::fromStdString(scan.error())));
    return;
  }

  if (!QDir().mkpath(marketplace_dir_)) {
    reportDiagnostic(
        DiagnosticLevel::kWarning,
        u"Cannot create the extensions directory \"%1\" — bundled plugins are unavailable this session"_s.arg(
            marketplace_dir_));
    return;
  }

  // The payload sources resolve against the canonical bundled root (computed
  // once — it is loop-invariant).
  ec.clear();
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(std::filesystem::path(bundled_dir_.toStdString()), ec);
  if (ec) {
    reportDiagnostic(
        DiagnosticLevel::kWarning, u"Cannot resolve the bundled plugin directory \"%1\": %2"_s.arg(
                                       bundled_dir_, QString::fromStdString(ec.message())));
    return;
  }

  // Every bundled id -> version, whether or not it gets copied this run.
  QMap<QString, QString> bundled_versions;
  // Set when a failed promote could not restore the old copy: the stage dir
  // then holds the only surviving payload and must not be deleted below.
  bool stage_has_orphans = false;

  for (const PluginDescriptor& descriptor : scan->plugins) {
    const QString id = QString::fromStdString(descriptor.id);
    if (id.isEmpty()) {
      continue;
    }
    bundled_versions.insert(id, QString::fromStdString(descriptor.version));

    // The payload is the top-level entry under the bundled dir that contains the
    // DSO — a per-id subdirectory (e.g. csv-loader/, or ros2-topic-subscriber/
    // with its dist/<distro>/ inners) or, for a flat layout, the .so file itself.
    const std::filesystem::path dst = market_root / descriptor.id;
    std::error_code resolve_ec;
    const std::filesystem::path dso = std::filesystem::weakly_canonical(descriptor.dso_path, resolve_ec);
    const std::filesystem::path rel = std::filesystem::relative(dso, root, resolve_ec);
    if (resolve_ec || rel.empty()) {
      reportDiagnostic(
          DiagnosticLevel::kWarning, u"Skipped bundled plugin \"%1\": cannot resolve its path"_s.arg(id), id);
      continue;
    }
    const std::filesystem::path src = root / *rel.begin();
    const std::filesystem::path installed_dso = installedDsoPath(dst, rel);

    // Steady-state fast path: an installed DSO matching the bundled one by
    // size and (seed-stamped) mtime is the same build — skip without paying
    // the installed-side dlopen.
    if (sameDsoSignature(installed_dso, dso)) {
      continue;
    }

    // The marketplace dir stays at least as new as the bundled set: absent or
    // unreadable -> copy; older -> refresh; same-or-newer -> untouched (a
    // marketplace update above the bundled version is never downgraded, and an
    // equal version never refreshes — ship a change by bumping the version).
    const std::optional<std::string> installed = installedPluginVersion(dst, descriptor.id);
    if (installed && compareSemver(descriptor.version, *installed) <= 0) {
      continue;
    }

    // Stage the new payload, then swap it into place. Any failure keeps (or
    // restores) the old copy; the next launch retries.
    const std::filesystem::path stage_new = stage_root / descriptor.id;
    const std::filesystem::path aside = stage_root / (descriptor.id + ".old");
    bool aside_orphaned = false;
    std::error_code step_ec = stagePayload(src, dso, stage_new);
    if (!step_ec) {
      step_ec = promoteStaged(stage_new, dst, aside, &aside_orphaned);
    }
    if (aside_orphaned) {
      // Worst case: the promote failed AND the old copy could not be put back.
      // Keep it in the stage dir (inert — never scanned) and say exactly
      // where, so the user can recover it before the next launch's cleanup.
      stage_has_orphans = true;
      reportDiagnostic(
          DiagnosticLevel::kError,
          u"Refreshing plugin \"%1\" failed and the previous copy could not be restored; it is preserved at "
          u"\"%2\" until the next launch"_s.arg(id, QString::fromStdString(aside.string())),
          id);
    }
    if (step_ec) {
      std::error_code cleanup_ec;
      std::filesystem::remove_all(stage_new, cleanup_ec);
      reportDiagnostic(
          DiagnosticLevel::kWarning,
          u"Failed to seed bundled plugin \"%1\" into \"%2\": %3"_s.arg(
              id, QString::fromStdString(dst.string()), QString::fromStdString(step_ec.message())),
          id);
      continue;
    }

    // Stamp the installed DSO with the bundled one's mtime (best effort) so
    // the next launch's signature fast path can skip the manifest read.
    std::error_code stamp_ec;
    const auto bundled_mtime = std::filesystem::last_write_time(dso, stamp_ec);
    if (!stamp_ec) {
      std::filesystem::last_write_time(installed_dso, bundled_mtime, stamp_ec);
    }

    if (installed) {
      const QString message = u"Refreshed bundled plugin \"%1\" %2 -> %3"_s.arg(
          id, QString::fromStdString(*installed), QString::fromStdString(descriptor.version));
      qCInfo(lcCatalog) << message;
      reportDiagnostic(DiagnosticLevel::kInfo, message, id);
    } else {
      qCInfo(lcCatalog) << "Seeded bundled plugin" << id << "into" << QString::fromStdString(dst.string());
    }
  }

  if (!stage_has_orphans) {
    std::error_code stage_cleanup_ec;
    std::filesystem::remove_all(stage_root, stage_cleanup_ec);
  }

  // The bundled map powers the marketplace "core plugin" policy: uninstall lock
  // (isBundled) + downgrade-to-bundled. Scoped to default mode — in a
  // --plugin-dir session the ExtensionManager governs the override dir, where a
  // same-id copy is user-owned and must stay uninstallable.
  if (default_mode_) {
    extension_manager_->setBundledVersions(bundled_versions);
  }
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
