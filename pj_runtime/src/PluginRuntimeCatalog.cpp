// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PluginRuntimeCatalog.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/data_source_protocol.h"
#include "pj_base/toolbox_protocol.h"

namespace PJ {

namespace {

// Synthetic path scheme for statically registered plugins (registerStatic*);
// they have no backing file, so disk reconciliation must never evict them.
constexpr std::string_view kStaticPathPrefix = "static://";

bool isStaticPath(const std::string& path) {
  return path.starts_with(kStaticPathPrefix);
}

std::string canonicalPath(const std::filesystem::path& path) {
  std::error_code ec;
  auto canon = std::filesystem::weakly_canonical(path, ec);
  return (ec ? path : canon).string();
}

std::filesystem::file_time_type safeMtime(const std::filesystem::path& path) {
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(path, ec);
  return ec ? std::filesystem::file_time_type{} : mtime;
}

std::string normalizeExtension(std::string ext) {
  if (!ext.empty() && ext.front() != '.') {
    ext.insert(ext.begin(), '.');
  }
  return ext;
}

// ASCII case-insensitive equality, so ".CSV" in a manifest matches a ".csv"
// query. Deliberately not std::tolower: that is locale-sensitive (e.g. the
// Turkish-locale dotless i), and the contract promises a pure ASCII fold.
bool extensionEquals(std::string_view lhs, std::string_view rhs) {
  const auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
  return std::ranges::equal(lhs, rhs, [&](char a, char b) { return fold(a) == fold(b); });
}

// Probes a fresh plugin instance and returns its capability mask, or nullopt
// when the plugin's create() returned null — a broken build that must be
// rejected rather than handed a null context via capabilities().
template <typename LibraryT>
std::optional<uint64_t> probeCapabilities(const LibraryT& library) {
  const auto handle = library.createHandle();
  if (!handle.valid()) {
    return std::nullopt;
  }
  return handle.capabilities();
}

template <typename PluginT>
std::vector<PluginT*> mutablePtrs(std::vector<PluginT>& plugins, uint64_t capability) {
  std::vector<PluginT*> out;
  for (auto& plugin : plugins) {
    if ((plugin.capabilities & capability) != 0) {
      out.push_back(&plugin);
    }
  }
  return out;
}

template <typename PluginT>
std::vector<const PluginT*> constPtrs(const std::vector<PluginT>& plugins, uint64_t capability) {
  std::vector<const PluginT*> out;
  for (const auto& plugin : plugins) {
    if ((plugin.capabilities & capability) != 0) {
      out.push_back(&plugin);
    }
  }
  return out;
}

}  // namespace

namespace detail {

// Compares two dotted numeric version strings (e.g. "4.1.0" vs "4.0.2"). Only the
// leading numeric components matter: each component's digits are read until the
// first non-digit. A '.' continues to the next component; any other separator
// ('-', '+', …) ends the numeric part, so a pre-release/build suffix is ignored
// *in full* even when it contains dots ("1-rc2", "1-rc.2", "3+meta" all compare as
// "1"/"1"/"3"). A missing trailing component counts as 0 (so "4.1" == "4.1.0").
// Components are compared as unsigned decimals *without* converting to an integer
// type — leading zeros are stripped, then the longer digit run is the larger value,
// else they compare lexicographically. This is overflow-proof: an absurdly long
// component like "999999999999999999999.0.0" is handled correctly, not wrapped.
// Returns <0, 0, or >0 like strcmp.
int compareSemver(std::string_view lhs, std::string_view rhs) {
  // Consume the leading digit run of the current component; returns it with leading
  // zeros stripped ("" == numeric 0). Advance to the next component only across a '.'
  // that immediately follows the digits — any other separator begins a suffix the
  // comparison ignores, so the numeric part ends there. (Scanning for the next '.'
  // instead would wrongly step over a suffix like "-rc.2" and read its digits.)
  auto takeComponent = [](std::string_view& v) -> std::string_view {
    size_t len = 0;
    while (len < v.size() && v[len] >= '0' && v[len] <= '9') {
      ++len;
    }
    const std::string_view run = v.substr(0, len);
    v = (len < v.size() && v[len] == '.') ? v.substr(len + 1) : std::string_view{};
    size_t first_significant = 0;
    while (first_significant < run.size() && run[first_significant] == '0') {
      ++first_significant;
    }
    return run.substr(first_significant);
  };
  while (!lhs.empty() || !rhs.empty()) {
    const std::string_view l = takeComponent(lhs);
    const std::string_view r = takeComponent(rhs);
    if (l.size() != r.size()) {
      return l.size() < r.size() ? -1 : 1;
    }
    if (const int cmp = l.compare(r); cmp != 0) {
      return cmp < 0 ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace detail

PluginRuntimeCatalog::PluginRuntimeCatalog(
    std::filesystem::path plugin_dir, DiagnosticSink sink, std::string diagnostic_source)
    : sink_(std::move(sink)), diagnostic_source_(std::move(diagnostic_source)) {
  if (!plugin_dir.empty()) {
    plugin_dirs_.push_back({std::move(plugin_dir), false});
  }
}

void PluginRuntimeCatalog::setPluginDir(std::filesystem::path plugin_dir) {
  plugin_dirs_.clear();
  if (!plugin_dir.empty()) {
    plugin_dirs_.push_back({std::move(plugin_dir), false});
  }
}

void PluginRuntimeCatalog::setPluginDirs(std::vector<PluginDirEntry> plugin_dirs) {
  plugin_dirs_ = std::move(plugin_dirs);
}

void PluginRuntimeCatalog::setDiagnosticSink(DiagnosticSink sink) {
  sink_ = std::move(sink);
}

void PluginRuntimeCatalog::setHostVersion(std::string host_version) {
  host_version_ = std::move(host_version);
}

std::vector<PluginDescriptor> PluginRuntimeCatalog::collectDeduplicatedPlugins() const {
  // Winner selection when the same plugin id appears in more than one folder:
  //
  //   1. Authoritative entries (custom folders + --plugin-dir; see PluginDirEntry)
  //      are a HARD override. Once an id is claimed by an
  //      authoritative folder, the highest-priority authoritative copy wins outright
  //      — ignoring the version and compatibility of any lower-priority folder.
  //   2. Among the remaining (managed) folders — marketplace + bundled — the winner
  //      is by compatibility, then version, then folder priority: a compatible build
  //      beats an incompatible one (min_plotjuggler_version > host_version_) at any
  //      version; among equally compatible candidates the higher version wins; a tie
  //      keeps the higher-priority folder.
  //
  // Compatibility only breaks ties between managed candidates — it never excludes a
  // plugin (a lone incompatible plugin still loads; if every managed candidate is
  // incompatible the highest version wins). min_plotjuggler_version is an advisory
  // floor, not a hard gate (the ABI gate runs earlier in scanPluginDsos). An empty
  // host_version_ disables the compatibility check entirely.
  const auto compatible = [this](const PluginDescriptor& d) {
    return host_version_.empty() || d.min_plotjuggler_version.empty() ||
           detail::compareSemver(d.min_plotjuggler_version, host_version_) <= 0;
  };

  std::vector<PluginDescriptor> winners;
  std::unordered_map<std::string, size_t> winner_index;  // id -> position in `winners`
  std::unordered_set<std::string> authoritative_win;     // ids whose current winner is authoritative (locked)
  std::unordered_set<std::string> scanned_dirs;          // canonical paths already scanned (dedup the list itself)
  for (const PluginDirEntry& entry : plugin_dirs_) {
    const std::filesystem::path& dir = entry.dir;
    if (dir.empty()) {
      continue;
    }
    // Skip a folder already scanned in a higher-priority slot: without this, the
    // same folder listed twice compares every plugin against itself and emits a
    // confusing "ignoring duplicate id … already loaded from <same path>".
    if (!scanned_dirs.insert(canonicalPath(dir)).second) {
      continue;
    }
    const bool dir_authoritative = entry.authoritative;
    auto scan = scanPluginDsos(dir);
    if (!scan) {
      report(DiagnosticLevel::kError, {}, scan.error());
      continue;
    }
    reportScanDiagnostics(*scan);
    for (const PluginDescriptor& descriptor : scan->plugins) {
      const auto it = winner_index.find(descriptor.id);
      if (it == winner_index.end()) {
        winner_index.emplace(descriptor.id, winners.size());
        winners.push_back(descriptor);
        if (dir_authoritative) {
          authoritative_win.insert(descriptor.id);
        }
        continue;
      }
      PluginDescriptor& incumbent = winners[it->second];
      const std::string incumbent_path = incumbent.dso_path.string();

      // (1) A locked authoritative winner is a hard override — nothing displaces it.
      if (authoritative_win.count(descriptor.id) != 0) {
        // Note when the loser is itself authoritative (just lower priority), so the
        // diagnostic does not read as if a managed copy lost to an authoritative one.
        const std::string candidate_note =
            dir_authoritative ? "; candidate is also from an authoritative folder (lower priority)" : "";
        report(
            DiagnosticLevel::kInfo, descriptor.id,
            descriptor.dso_path.string() + ": ignoring duplicate plugin id \"" + descriptor.id + "\" v" +
                descriptor.version + " (authoritative v" + incumbent.version + " already loaded from " +
                incumbent_path + candidate_note + ")");
        continue;
      }
      // (1) An authoritative candidate overrides a managed incumbent, regardless of
      //     version or compatibility.
      if (dir_authoritative) {
        report(
            DiagnosticLevel::kInfo, descriptor.id,
            descriptor.dso_path.string() + ": plugin id \"" + descriptor.id + "\" v" + descriptor.version +
                " from an authoritative folder supersedes v" + incumbent.version + " from " + incumbent_path);
        incumbent = descriptor;
        authoritative_win.insert(descriptor.id);
        continue;
      }
      // (2) Both managed: compatibility, then version, then folder priority.
      const bool cand_ok = compatible(descriptor);
      const bool inc_ok = compatible(incumbent);
      const bool replace =
          (cand_ok != inc_ok) ? cand_ok : detail::compareSemver(descriptor.version, incumbent.version) > 0;
      if (replace) {
        const std::string reason = (cand_ok && !inc_ok)
                                       ? " supersedes incompatible v" + incumbent.version +
                                             " (needs PlotJuggler >= " + incumbent.min_plotjuggler_version + ")"
                                       : " supersedes v" + incumbent.version;
        report(
            DiagnosticLevel::kInfo, descriptor.id,
            descriptor.dso_path.string() + ": plugin id \"" + descriptor.id + "\" v" + descriptor.version + reason +
                " from " + incumbent_path);
        incumbent = descriptor;
      } else {
        const std::string reason =
            (!cand_ok && inc_ok)
                ? "ignoring incompatible plugin id \"" + descriptor.id + "\" v" + descriptor.version +
                      " (needs PlotJuggler >= " + descriptor.min_plotjuggler_version + "; compatible v" +
                      incumbent.version + " already loaded from " + incumbent_path + ")"
                : "ignoring duplicate plugin id \"" + descriptor.id + "\" v" + descriptor.version + " (v" +
                      incumbent.version + " already loaded from " + incumbent_path + ")";
        report(DiagnosticLevel::kInfo, descriptor.id, descriptor.dso_path.string() + ": " + reason);
      }
    }
  }
  // Statically registered plugins (registerStatic*) outrank every folder tier:
  // they are compiled into the host on purpose, so a scanned DSO with the same
  // id must not load alongside them.
  std::erase_if(winners, [&](const PluginDescriptor& descriptor) {
    if (!isStaticallyRegisteredId(descriptor.id)) {
      return false;
    }
    report(
        DiagnosticLevel::kInfo, descriptor.id,
        descriptor.dso_path.string() + ": ignoring duplicate plugin id \"" + descriptor.id +
            "\" (statically registered plugin takes precedence)");
    return true;
  });
  return winners;
}

void PluginRuntimeCatalog::scanDirectory() {
  // Statically registered plugins have no backing file; a rescan only rebuilds
  // the DSO-backed set.
  const auto drop_dso_backed = [](auto& vec) {
    std::erase_if(vec, [](const auto& plugin) { return !isStaticPath(plugin.path); });
  };
  drop_dso_backed(data_sources_);
  drop_dso_backed(message_parsers_);
  drop_dso_backed(toolbox_plugins_);

  for (const PluginDescriptor& descriptor : collectDeduplicatedPlugins()) {
    if (!loadAndRegister(descriptor)) {
      report(
          DiagnosticLevel::kError, descriptor.id,
          descriptor.dso_path.string() + ": failed to load " + std::string(toString(descriptor.family)) + " plugin");
    }
  }
}

bool PluginRuntimeCatalog::reload() {
  const std::vector<PluginDescriptor> plugins = collectDeduplicatedPlugins();

  std::vector<std::string> on_disk;
  on_disk.reserve(plugins.size());
  for (const PluginDescriptor& descriptor : plugins) {
    on_disk.push_back(canonicalPath(descriptor.dso_path));
  }

  bool changed = false;
  auto drop_missing = [&](auto& vec, std::string_view family) {
    const auto before = vec.size();
    std::erase_if(vec, [&](const auto& plugin) {
      if (isStaticPath(plugin.path)) {
        return false;  // no backing file to go missing
      }
      const bool gone = std::find(on_disk.begin(), on_disk.end(), plugin.path) == on_disk.end();
      if (gone) {
        report(DiagnosticLevel::kInfo, plugin.id, "Unloaded " + std::string(family) + ": " + plugin.path);
      }
      return gone;
    });
    changed = changed || vec.size() != before;
  };
  drop_missing(data_sources_, "DataSource");
  drop_missing(message_parsers_, "MessageParser");
  drop_missing(toolbox_plugins_, "Toolbox");

  for (const PluginDescriptor& descriptor : plugins) {
    const std::string path = canonicalPath(descriptor.dso_path);
    const auto disk_mtime = safeMtime(descriptor.dso_path);
    if (disk_mtime == std::filesystem::file_time_type{}) {
      report(DiagnosticLevel::kWarning, descriptor.id, descriptor.dso_path.string() + ": could not read mtime");
      continue;
    }

    const auto prior_mtime = loadedMtimeForPath(path);
    const bool already_loaded = prior_mtime != std::filesystem::file_time_type{};
    if (already_loaded && disk_mtime <= prior_mtime) {
      continue;
    }
    if (already_loaded) {
      evictByPath(path);
      changed = true;
    }
    if (loadAndRegister(descriptor)) {
      changed = true;
    } else {
      report(
          DiagnosticLevel::kError, descriptor.id,
          descriptor.dso_path.string() + ": failed to load " + std::string(toString(descriptor.family)) + " plugin");
    }
  }

  return changed;
}

namespace {

// Parse a plugin's embedded manifest JSON. Returns false on invalid JSON.
bool parseStaticManifest(const char* manifest_json, nlohmann::json& out) {
  try {
    out = nlohmann::json::parse(manifest_json ? manifest_json : "");
    return out.is_object();
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<std::string> readManifestStringArray(const nlohmann::json& j, const char* key) {
  std::vector<std::string> values;
  if (auto it = j.find(key); it != j.end() && it->is_array()) {
    for (const auto& v : *it) {
      if (v.is_string()) {
        values.push_back(v.get<std::string>());
      }
    }
  }
  return values;
}

// Shared body for the three registerStatic* methods: load the static vtable,
// parse the embedded manifest, fill the common Runtime*Plugin fields, and push.
// `claim_id` enforces the catalog-wide id policy (reject duplicate statics,
// supersede a DSO-backed entry); `fill` adds the family-specific fields
// (capabilities / extensions / encodings) and may veto the registration;
// `report` bridges to the catalog's private diagnostic sink.
template <typename LibraryT, typename RuntimeT, typename ReportFn, typename ClaimFn, typename FillFn>
bool registerStaticPlugin(
    const auto* vtable, std::vector<RuntimeT>& out, const char* family, const ReportFn& report, const ClaimFn& claim_id,
    const FillFn& fill) {
  auto result = LibraryT::loadStatic(vtable);
  if (!result) {
    report(DiagnosticLevel::kError, std::string{}, "static " + std::string(family) + ": " + result.error());
    return false;
  }
  nlohmann::json j;
  if (!parseStaticManifest(vtable->manifest_json, j)) {
    report(DiagnosticLevel::kError, std::string{}, "static " + std::string(family) + ": invalid manifest JSON");
    return false;
  }
  RuntimeT loaded;
  loaded.library = std::move(*result);
  // The typed reads throw on a well-formed manifest with mistyped fields
  // (e.g. "id":123); honor the false-plus-diagnostic contract instead.
  try {
    loaded.id = j.value("id", std::string{});
    loaded.name = j.value("name", std::string{});
    loaded.version = j.value("version", std::string{});
  } catch (const std::exception& e) {
    report(
        DiagnosticLevel::kError, std::string{},
        "static " + std::string(family) + ": invalid manifest field types: " + e.what());
    return false;
  }
  // A non-empty id is required: it is the only identity a static plugin has
  // (its synthetic path is derived from it), so an empty id would make two
  // static registrations indistinguishable to evictByPath/loadedMtimeForPath.
  if (loaded.id.empty()) {
    report(
        DiagnosticLevel::kError, std::string{},
        "static " + std::string(family) + ": manifest is missing a non-empty \"id\"");
    return false;
  }
  if (!claim_id(loaded.id)) {
    return false;
  }
  loaded.path = std::string(kStaticPathPrefix) + loaded.id;
  loaded.loaded_mtime = std::filesystem::file_time_type{};
  if (!fill(loaded, j)) {
    return false;
  }
  report(DiagnosticLevel::kInfo, loaded.id, "Registered static " + std::string(family) + " " + loaded.name);
  out.push_back(std::move(loaded));
  return true;
}

}  // namespace

bool PluginRuntimeCatalog::registerStaticDataSource(const PJ_data_source_vtable_t* vtable) {
  return registerStaticPlugin<DataSourceLibrary>(
      vtable, data_sources_, "DataSource",
      [this](DiagnosticLevel level, const std::string& id, std::string msg) { report(level, id, std::move(msg)); },
      [this](const std::string& id) { return claimStaticId(id, "DataSource"); },
      [this](RuntimeDataSourcePlugin& loaded, const nlohmann::json& j) {
        const auto capabilities = probeCapabilities(loaded.library);
        if (!capabilities) {
          report(
              DiagnosticLevel::kError, loaded.id,
              "static DataSource \"" + loaded.id + "\": plugin instance creation failed (create() returned null)");
          return false;
        }
        loaded.capabilities = *capabilities;
        for (auto& ext : readManifestStringArray(j, "file_extensions")) {
          loaded.file_extensions.push_back(normalizeExtension(std::move(ext)));
        }
        return true;
      });
}

bool PluginRuntimeCatalog::registerStaticMessageParser(const PJ_message_parser_vtable_t* vtable) {
  return registerStaticPlugin<MessageParserLibrary>(
      vtable, message_parsers_, "MessageParser",
      [this](DiagnosticLevel level, const std::string& id, std::string msg) { report(level, id, std::move(msg)); },
      [this](const std::string& id) { return claimStaticId(id, "MessageParser"); },
      [](RuntimeMessageParserPlugin& loaded, const nlohmann::json& j) {
        loaded.encodings = readManifestStringArray(j, "encoding");
        return true;
      });
}

bool PluginRuntimeCatalog::registerStaticToolbox(const PJ_toolbox_vtable_t* vtable) {
  return registerStaticPlugin<ToolboxLibrary>(
      vtable, toolbox_plugins_, "Toolbox",
      [this](DiagnosticLevel level, const std::string& id, std::string msg) { report(level, id, std::move(msg)); },
      [this](const std::string& id) { return claimStaticId(id, "Toolbox"); },
      [this](RuntimeToolboxPlugin& loaded, const nlohmann::json& /*j*/) {
        const auto capabilities = probeCapabilities(loaded.library);
        if (!capabilities) {
          report(
              DiagnosticLevel::kError, loaded.id,
              "static Toolbox \"" + loaded.id + "\": plugin instance creation failed (create() returned null)");
          return false;
        }
        loaded.capabilities = *capabilities;
        return true;
      });
}

bool PluginRuntimeCatalog::loadAndRegister(const PluginDescriptor& descriptor) {
  switch (descriptor.family) {
    case PluginFamily::kDataSource:
      return loadAndRegisterDataSource(descriptor);
    case PluginFamily::kMessageParser:
      return loadAndRegisterMessageParser(descriptor);
    case PluginFamily::kToolbox:
      return loadAndRegisterToolbox(descriptor);
    case PluginFamily::kDialog:
      report(
          DiagnosticLevel::kWarning, descriptor.id,
          descriptor.dso_path.string() + ": standalone dialog plugin \"" + descriptor.name +
              "\" discovered; dialogs are loaded through owning plugins");
      return false;
    case PluginFamily::kUnknown:
      break;
  }
  return false;
}

bool PluginRuntimeCatalog::loadAndRegisterDataSource(const PluginDescriptor& descriptor) {
  auto result = DataSourceLibrary::load(descriptor.dso_path.string());
  if (!result) {
    report(DiagnosticLevel::kError, descriptor.id, descriptor.dso_path.string() + ": " + result.error());
    return false;
  }

  RuntimeDataSourcePlugin loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(descriptor.dso_path);
  loaded.loaded_mtime = safeMtime(descriptor.dso_path);
  loaded.id = descriptor.id;
  loaded.name = descriptor.name;
  loaded.version = descriptor.version;

  const auto capabilities = probeCapabilities(loaded.library);
  if (!capabilities) {
    report(
        DiagnosticLevel::kError, descriptor.id,
        descriptor.dso_path.string() + ": plugin instance creation failed (create() returned null)");
    return false;
  }
  loaded.capabilities = *capabilities;

  // Fail-fast on plugins that lie about kCapabilityHasDialog: a misbuilt
  // plugin that advertises the bit but doesn't export the dialog vtable
  // would otherwise reach the host's dialog flow and silently degrade to
  // "no dialog", confusing the user. Block it here so the broken plugin
  // never enters the loaded set.
  if ((loaded.capabilities & PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG) != 0) {
    auto vt = loaded.library.resolveDialogVtable();
    if (!vt) {
      report(
          DiagnosticLevel::kError, descriptor.id,
          descriptor.dso_path.string() + ": advertises kCapabilityHasDialog but " + vt.error());
      return false;
    }
  }

  loaded.file_extensions.reserve(descriptor.file_extensions.size());
  for (const auto& ext : descriptor.file_extensions) {
    loaded.file_extensions.push_back(normalizeExtension(ext));
  }

  report(DiagnosticLevel::kInfo, loaded.id, "Loaded DataSource " + loaded.name + " from " + loaded.path);
  data_sources_.push_back(std::move(loaded));
  return true;
}

bool PluginRuntimeCatalog::loadAndRegisterMessageParser(const PluginDescriptor& descriptor) {
  auto result = MessageParserLibrary::load(descriptor.dso_path.string());
  if (!result) {
    report(DiagnosticLevel::kError, descriptor.id, descriptor.dso_path.string() + ": " + result.error());
    return false;
  }

  RuntimeMessageParserPlugin loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(descriptor.dso_path);
  loaded.loaded_mtime = safeMtime(descriptor.dso_path);
  loaded.id = descriptor.id;
  loaded.name = descriptor.name;
  loaded.version = descriptor.version;
  loaded.encodings.insert(loaded.encodings.end(), descriptor.encoding.begin(), descriptor.encoding.end());

  report(DiagnosticLevel::kInfo, loaded.id, "Loaded MessageParser " + loaded.name + " from " + loaded.path);
  message_parsers_.push_back(std::move(loaded));
  return true;
}

bool PluginRuntimeCatalog::loadAndRegisterToolbox(const PluginDescriptor& descriptor) {
  auto result = ToolboxLibrary::load(descriptor.dso_path.string());
  if (!result) {
    report(DiagnosticLevel::kError, descriptor.id, descriptor.dso_path.string() + ": " + result.error());
    return false;
  }

  RuntimeToolboxPlugin loaded;
  loaded.library = std::move(*result);
  loaded.path = canonicalPath(descriptor.dso_path);
  loaded.loaded_mtime = safeMtime(descriptor.dso_path);
  loaded.id = descriptor.id;
  loaded.name = descriptor.name;
  loaded.version = descriptor.version;

  const auto capabilities = probeCapabilities(loaded.library);
  if (!capabilities) {
    report(
        DiagnosticLevel::kError, descriptor.id,
        descriptor.dso_path.string() + ": plugin instance creation failed (create() returned null)");
    return false;
  }
  loaded.capabilities = *capabilities;

  // Same fail-fast contract as DataSource above: kToolboxCapabilityHasDialog
  // requires an exported dialog vtable.
  if ((loaded.capabilities & PJ_TOOLBOX_CAPABILITY_HAS_DIALOG) != 0) {
    auto vt = loaded.library.resolveDialogVtable();
    if (!vt) {
      report(
          DiagnosticLevel::kError, descriptor.id,
          descriptor.dso_path.string() + ": advertises kToolboxCapabilityHasDialog but " + vt.error());
      return false;
    }
  }

  report(DiagnosticLevel::kInfo, loaded.id, "Loaded Toolbox " + loaded.name + " from " + loaded.path);
  toolbox_plugins_.push_back(std::move(loaded));
  return true;
}

bool PluginRuntimeCatalog::isStaticallyRegisteredId(const std::string& id) const {
  const auto has_static = [&](const auto& vec) {
    return std::ranges::any_of(vec, [&](const auto& plugin) { return plugin.id == id && isStaticPath(plugin.path); });
  };
  return has_static(data_sources_) || has_static(message_parsers_) || has_static(toolbox_plugins_);
}

bool PluginRuntimeCatalog::claimStaticId(const std::string& id, const char* family) {
  if (isStaticallyRegisteredId(id)) {
    report(
        DiagnosticLevel::kError, id,
        "static " + std::string(family) + ": plugin id \"" + id + "\" is already statically registered");
    return false;
  }
  // A DSO-backed plugin with the same id yields to the static registration, the
  // same precedence scans apply (collectDeduplicatedPlugins skips descriptors
  // whose id is statically registered) — kept order-independent here.
  const auto evict_dso_backed = [&](auto& vec) {
    const auto before = vec.size();
    std::erase_if(vec, [&](const auto& plugin) { return plugin.id == id && !isStaticPath(plugin.path); });
    return vec.size() != before;
  };
  bool evicted = evict_dso_backed(data_sources_);
  evicted = evict_dso_backed(message_parsers_) || evicted;
  evicted = evict_dso_backed(toolbox_plugins_) || evicted;
  if (evicted) {
    report(
        DiagnosticLevel::kInfo, id,
        "static " + std::string(family) + ": superseding DSO-backed plugin id \"" + id + "\"");
  }
  return true;
}

bool PluginRuntimeCatalog::evictByPath(const std::string& path) {
  bool removed = false;
  auto erase_path = [&](auto& vec) {
    const auto before = vec.size();
    std::erase_if(vec, [&](const auto& plugin) { return plugin.path == path; });
    removed = removed || vec.size() != before;
  };
  erase_path(data_sources_);
  erase_path(message_parsers_);
  erase_path(toolbox_plugins_);
  return removed;
}

std::filesystem::file_time_type PluginRuntimeCatalog::loadedMtimeForPath(const std::string& path) const {
  auto find_mtime = [&](const auto& vec) {
    auto it = std::find_if(vec.begin(), vec.end(), [&](const auto& plugin) { return plugin.path == path; });
    return it == vec.end() ? std::filesystem::file_time_type{} : it->loaded_mtime;
  };
  if (auto mtime = find_mtime(data_sources_); mtime != std::filesystem::file_time_type{}) {
    return mtime;
  }
  if (auto mtime = find_mtime(message_parsers_); mtime != std::filesystem::file_time_type{}) {
    return mtime;
  }
  return find_mtime(toolbox_plugins_);
}

std::vector<RuntimeDataSourcePlugin*> PluginRuntimeCatalog::fileImportSources() {
  return mutablePtrs(data_sources_, PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT);
}

std::vector<const RuntimeDataSourcePlugin*> PluginRuntimeCatalog::fileImportSources() const {
  return constPtrs(data_sources_, PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT);
}

std::vector<RuntimeDataSourcePlugin*> PluginRuntimeCatalog::streamSources() {
  return mutablePtrs(data_sources_, PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM);
}

std::vector<const RuntimeDataSourcePlugin*> PluginRuntimeCatalog::streamSources() const {
  return constPtrs(data_sources_, PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM);
}

std::vector<RuntimeDataSourcePlugin*> PluginRuntimeCatalog::findSourcesForExtension(std::string_view ext) {
  const std::string query = normalizeExtension(std::string(ext));
  std::vector<RuntimeDataSourcePlugin*> out;
  for (auto& source : data_sources_) {
    if ((source.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) == 0) {
      continue;
    }
    if (std::ranges::any_of(
            source.file_extensions, [&](const std::string& candidate) { return extensionEquals(candidate, query); })) {
      out.push_back(&source);
    }
  }
  return out;
}

std::vector<const RuntimeDataSourcePlugin*> PluginRuntimeCatalog::findSourcesForExtension(std::string_view ext) const {
  const std::string query = normalizeExtension(std::string(ext));
  std::vector<const RuntimeDataSourcePlugin*> out;
  for (const auto& source : data_sources_) {
    if ((source.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) == 0) {
      continue;
    }
    if (std::ranges::any_of(
            source.file_extensions, [&](const std::string& candidate) { return extensionEquals(candidate, query); })) {
      out.push_back(&source);
    }
  }
  return out;
}

RuntimeMessageParserPlugin* PluginRuntimeCatalog::findParserByEncoding(std::string_view encoding) {
  for (auto& parser : message_parsers_) {
    if (std::find(parser.encodings.begin(), parser.encodings.end(), encoding) != parser.encodings.end()) {
      return &parser;
    }
  }
  return nullptr;
}

const RuntimeMessageParserPlugin* PluginRuntimeCatalog::findParserByEncoding(std::string_view encoding) const {
  for (const auto& parser : message_parsers_) {
    if (std::find(parser.encodings.begin(), parser.encodings.end(), encoding) != parser.encodings.end()) {
      return &parser;
    }
  }
  return nullptr;
}

std::string PluginRuntimeCatalog::buildFileFilter() const {
  std::string all_exts;
  std::string per_plugin;
  for (const auto& source : data_sources_) {
    if ((source.capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) == 0 || source.file_extensions.empty()) {
      continue;
    }

    if (!per_plugin.empty()) {
      per_plugin += ";;";
    }
    per_plugin += source.name + " (";
    for (size_t i = 0; i < source.file_extensions.size(); ++i) {
      if (i > 0) {
        per_plugin += " ";
      }
      per_plugin += "*" + source.file_extensions[i];
      if (!all_exts.empty()) {
        all_exts += " ";
      }
      all_exts += "*" + source.file_extensions[i];
    }
    per_plugin += ")";
  }

  std::string filter;
  if (!all_exts.empty()) {
    filter = "All supported files (" + all_exts + ")";
    if (!per_plugin.empty()) {
      filter += ";;" + per_plugin;
    }
  } else {
    filter = per_plugin;
  }
  if (!filter.empty()) {
    filter += ";;";
  }
  filter += "All files (*)";
  return filter;
}

std::string PluginRuntimeCatalog::listAvailableEncodings() const {
  std::vector<std::string> unique_encodings;
  for (const auto& parser : message_parsers_) {
    for (const auto& encoding : parser.encodings) {
      if (std::find(unique_encodings.begin(), unique_encodings.end(), encoding) == unique_encodings.end()) {
        unique_encodings.push_back(encoding);
      }
    }
  }

  return nlohmann::json(unique_encodings).dump();
}

void PluginRuntimeCatalog::report(DiagnosticLevel level, const std::string& id, std::string message) const {
  if (!sink_) {
    return;
  }
  sink_(Diagnostic{level, diagnostic_source_, id, std::move(message), std::chrono::system_clock::now()});
}

void PluginRuntimeCatalog::reportScanDiagnostics(const PluginScanResult& scan) const {
  for (const auto& diagnostic : scan.diagnostics) {
    report(DiagnosticLevel::kError, {}, diagnostic.path.string() + ": " + diagnostic.message);
  }
}

}  // namespace PJ
