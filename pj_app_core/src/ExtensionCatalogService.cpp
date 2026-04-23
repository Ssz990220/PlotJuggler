#include "pj_app_core/ExtensionCatalogService.h"

#include <QDir>
#include <QLatin1Char>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <algorithm>
#include <system_error>

#include <nlohmann/json.hpp>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcCatalog, "pj.app_core.extensions")

QString defaultExtensionsDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/extensions";
}

QString defaultPendingDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/.extension_staging";
}

// Resolve and canonicalize a plugin path so duplicate entries (symlinks,
// alternate casing on case-insensitive filesystems) collapse to one key.
QString canonicalPath(const std::filesystem::path& p) {
  std::error_code ec;
  auto canon = std::filesystem::weakly_canonical(p, ec);
  return QString::fromStdString((ec ? p : canon).string());
}

// last_write_time throws on race (file deleted between enumeration and query).
// Callers get a default-constructed time on failure; compare against current
// mtime would then always reload — acceptable, better than an aborted scan.
std::filesystem::file_time_type safeMtime(const std::filesystem::path& p) {
  std::error_code ec;
  auto t = std::filesystem::last_write_time(p, ec);
  if (ec) {
    qCWarning(lcCatalog) << "mtime read failed for" << QString::fromStdString(p.string())
                         << ':' << QString::fromStdString(ec.message());
    return {};
  }
  return t;
}

QString manifestName(const nlohmann::json& manifest, const std::filesystem::path& so_path) {
  return QString::fromStdString(manifest.value("name", so_path.stem().string()));
}

// Normalize a file extension so it starts with '.'. Accepts both "csv" and
// ".csv" from manifest authors and yields a glob-safe form.
QString normalizeExt(const std::string& raw) {
  QString s = QString::fromStdString(raw);
  if (!s.isEmpty() && !s.startsWith('.')) {
    s.prepend('.');
  }
  return s;
}

template <typename LoadedT>
bool dropMissing(std::vector<LoadedT>& vec, const std::vector<QString>& on_disk,
                 const char* family) {
  const auto before = vec.size();
  std::erase_if(vec, [&](const LoadedT& e) {
    const bool gone = std::find(on_disk.begin(), on_disk.end(), e.path) == on_disk.end();
    if (gone) {
      qCInfo(lcCatalog) << "Unloaded" << family << "(removed):" << e.path;
    }
    return gone;
  });
  return vec.size() != before;
}
}  // namespace

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, QObject* parent)
    : QObject(parent) {
  const bool use_defaults = extensions_dir.isEmpty();
  extensions_dir_ = use_defaults ? defaultExtensionsDir() : std::move(extensions_dir);
  const QString pending_dir =
      use_defaults ? defaultPendingDir() : extensions_dir_ + "/.pending";
  if (!QDir().mkpath(extensions_dir_)) {
    qCWarning(lcCatalog) << "Failed to create extensions directory" << extensions_dir_
                         << "— plugin loading will be a no-op until it exists.";
  }
  QDir().mkpath(pending_dir);

  download_manager_ = std::make_unique<DownloadManager>();
  extension_manager_ = std::make_unique<ExtensionManager>(download_manager_.get(),
                                                          extensions_dir_, pending_dir, this);
  extension_manager_->applyPendingInstalls();
  extension_manager_->applyPendingUninstalls();

  qCInfo(lcCatalog) << "Scanning" << extensions_dir_;
  scanOnce();
}

ExtensionCatalogService::~ExtensionCatalogService() = default;

bool ExtensionCatalogService::loadAndRegisterDataSource(const std::filesystem::path& so_path) {
  auto result = DataSourceLibrary::load(so_path.string());
  if (!result) {
    return false;
  }
  LoadedDataSource loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(so_path);
  loaded.loaded_mtime = safeMtime(so_path);

  auto handle = loaded.library.createHandle();
  loaded.capabilities = handle.capabilities();
  try {
    auto manifest = nlohmann::json::parse(handle.manifest());
    loaded.name = manifestName(manifest, so_path);
    if (manifest.contains("file_extensions") && manifest["file_extensions"].is_array()) {
      for (const auto& ext : manifest["file_extensions"]) {
        if (ext.is_string()) {
          loaded.file_extensions << normalizeExt(ext.get<std::string>());
        }
      }
    }
  } catch (const nlohmann::json::exception& e) {
    qCWarning(lcCatalog) << "Manifest parse failed for" << loaded.path << ':' << e.what();
    loaded.name = QString::fromStdString(so_path.stem().string());
  }
  qCInfo(lcCatalog) << "Loaded DataSource:" << loaded.name << "from" << loaded.path;
  data_sources_.push_back(std::move(loaded));
  return true;
}

bool ExtensionCatalogService::loadAndRegisterMessageParser(const std::filesystem::path& so_path) {
  auto result = MessageParserLibrary::load(so_path.string());
  if (!result) {
    return false;
  }
  LoadedMessageParser loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(so_path);
  loaded.loaded_mtime = safeMtime(so_path);

  auto handle = loaded.library.createHandle();
  try {
    auto manifest = nlohmann::json::parse(handle.manifest());
    loaded.name = manifestName(manifest, so_path);
    if (manifest.contains("encoding") && manifest["encoding"].is_array()) {
      for (const auto& e : manifest["encoding"]) {
        if (e.is_string()) {
          loaded.encodings << QString::fromStdString(e.get<std::string>());
        }
      }
    }
  } catch (const nlohmann::json::exception& e) {
    qCWarning(lcCatalog) << "Manifest parse failed for" << loaded.path << ':' << e.what();
    loaded.name = QString::fromStdString(so_path.stem().string());
  }
  qCInfo(lcCatalog) << "Loaded MessageParser:" << loaded.name << "from" << loaded.path;
  message_parsers_.push_back(std::move(loaded));
  return true;
}

bool ExtensionCatalogService::loadAndRegisterToolbox(const std::filesystem::path& so_path) {
  auto result = ToolboxLibrary::load(so_path.string());
  if (!result) {
    return false;
  }
  LoadedToolbox loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(so_path);
  loaded.loaded_mtime = safeMtime(so_path);

  auto handle = loaded.library.createHandle();
  loaded.capabilities = handle.capabilities();
  try {
    auto manifest = nlohmann::json::parse(handle.manifest());
    loaded.name = manifestName(manifest, so_path);
  } catch (const nlohmann::json::exception& e) {
    qCWarning(lcCatalog) << "Manifest parse failed for" << loaded.path << ':' << e.what();
    loaded.name = QString::fromStdString(so_path.stem().string());
  }
  qCInfo(lcCatalog) << "Loaded Toolbox:" << loaded.name << "from" << loaded.path;
  toolboxes_.push_back(std::move(loaded));
  return true;
}

bool ExtensionCatalogService::tryLoadAny(const std::filesystem::path& so_path) {
  return loadAndRegisterDataSource(so_path) || loadAndRegisterMessageParser(so_path) ||
         loadAndRegisterToolbox(so_path);
}

bool ExtensionCatalogService::evictByPath(const QString& path) {
  auto match = [&](const auto& e) { return e.path == path; };
  bool removed = false;
  if (auto it = std::find_if(data_sources_.begin(), data_sources_.end(), match);
      it != data_sources_.end()) {
    data_sources_.erase(it);
    removed = true;
  }
  if (auto it = std::find_if(message_parsers_.begin(), message_parsers_.end(), match);
      it != message_parsers_.end()) {
    message_parsers_.erase(it);
    removed = true;
  }
  if (auto it = std::find_if(toolboxes_.begin(), toolboxes_.end(), match);
      it != toolboxes_.end()) {
    toolboxes_.erase(it);
    removed = true;
  }
  return removed;
}

void ExtensionCatalogService::scanOnce() {
  namespace fs = std::filesystem;
  const std::string dir = extensions_dir_.toStdString();
  if (!fs::is_directory(dir)) {
    return;
  }
  const std::string plugin_ext = PlatformUtils::pluginExtension();
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    const auto& entry = *it;
    if (!entry.is_regular_file() || entry.path().extension() != plugin_ext) {
      continue;
    }
    if (!tryLoadAny(entry.path())) {
      qCWarning(lcCatalog) << "Failed to load plugin:"
                           << QString::fromStdString(entry.path().string());
    }
  }
  if (ec) {
    qCWarning(lcCatalog) << "Scan aborted mid-directory:"
                         << QString::fromStdString(ec.message());
  }
}

void ExtensionCatalogService::reload() {
  namespace fs = std::filesystem;
  const std::string dir = extensions_dir_.toStdString();
  if (!fs::is_directory(dir)) {
    return;
  }
  const std::string plugin_ext = PlatformUtils::pluginExtension();

  std::vector<fs::path> on_disk_paths;
  std::vector<QString> on_disk_canonical;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    const auto& entry = *it;
    if (entry.is_regular_file() && entry.path().extension() == plugin_ext) {
      on_disk_paths.push_back(entry.path());
      on_disk_canonical.push_back(canonicalPath(entry.path()));
    }
  }
  if (ec) {
    qCWarning(lcCatalog) << "Reload scan aborted mid-directory:"
                         << QString::fromStdString(ec.message());
    return;
  }

  bool changed = false;
  changed |= dropMissing(data_sources_, on_disk_canonical, "DataSource");
  changed |= dropMissing(message_parsers_, on_disk_canonical, "MessageParser");
  changed |= dropMissing(toolboxes_, on_disk_canonical, "Toolbox");

  auto currentMtime = [](const auto& vec, const QString& path) -> std::filesystem::file_time_type {
    auto it = std::find_if(vec.begin(), vec.end(),
                           [&](const auto& e) { return e.path == path; });
    return it == vec.end() ? std::filesystem::file_time_type{} : it->loaded_mtime;
  };

  for (size_t i = 0; i < on_disk_paths.size(); ++i) {
    const auto& so_path = on_disk_paths[i];
    const QString& path_str = on_disk_canonical[i];
    const auto disk_mtime = safeMtime(so_path);

    // If stat failed (race with deletion / permission flap), preserve the
    // current state and move on. safeMtime already logged the failure; don't
    // mistake a default mtime for "file unchanged".
    if (disk_mtime == std::filesystem::file_time_type{}) {
      continue;
    }

    // Find any prior entry (in any family) and decide keep-or-reload.
    auto prior_mtime = currentMtime(data_sources_, path_str);
    if (prior_mtime == std::filesystem::file_time_type{}) {
      prior_mtime = currentMtime(message_parsers_, path_str);
    }
    if (prior_mtime == std::filesystem::file_time_type{}) {
      prior_mtime = currentMtime(toolboxes_, path_str);
    }
    const bool already_loaded = prior_mtime != std::filesystem::file_time_type{};

    if (already_loaded && disk_mtime <= prior_mtime) {
      continue;  // unchanged since last load
    }
    if (already_loaded) {
      qCInfo(lcCatalog) << "Reloading updated plugin:" << path_str;
      evictByPath(path_str);
      changed = true;
    }
    if (tryLoadAny(so_path)) {
      changed = true;
    } else {
      qCWarning(lcCatalog) << "Failed to load plugin:" << path_str;
    }
  }

  if (changed) {
    emit catalogChanged();
  }
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::fileImportSources() const {
  std::vector<const LoadedDataSource*> out;
  for (const auto& ds : data_sources_) {
    if (ds.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) {
      out.push_back(&ds);
    }
  }
  return out;
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::streamSources() const {
  std::vector<const LoadedDataSource*> out;
  for (const auto& ds : data_sources_) {
    if (ds.capabilities & PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM) {
      out.push_back(&ds);
    }
  }
  return out;
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::findSourcesForExtension(
    QStringView ext) const {
  std::vector<const LoadedDataSource*> out;
  const QString needle = ext.toString();
  for (const auto& ds : data_sources_) {
    if (!(ds.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT)) {
      continue;
    }
    if (ds.file_extensions.contains(needle)) {
      out.push_back(&ds);
    }
  }
  return out;
}

const LoadedMessageParser* ExtensionCatalogService::findParserByEncoding(QStringView encoding) const {
  const QString needle = encoding.toString();
  for (const auto& mp : message_parsers_) {
    if (mp.encodings.contains(needle)) {
      return &mp;
    }
  }
  return nullptr;
}

QString ExtensionCatalogService::buildFileFilter() const {
  QStringList all_exts;
  QStringList per_plugin;
  for (const auto& ds : data_sources_) {
    if (!(ds.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) ||
        ds.file_extensions.isEmpty()) {
      continue;
    }
    QStringList globs;
    for (const QString& ext : ds.file_extensions) {
      globs << "*" + ext;
      all_exts << "*" + ext;
    }
    per_plugin << QStringLiteral("%1 (%2)").arg(ds.name, globs.join(QLatin1Char(' ')));
  }

  QStringList parts;
  if (!all_exts.isEmpty()) {
    parts << QStringLiteral("All supported files (%1)").arg(all_exts.join(QLatin1Char(' ')));
  }
  parts.append(per_plugin);
  parts << QStringLiteral("All files (*)");
  return parts.join(QStringLiteral(";;"));
}

}  // namespace PJ
