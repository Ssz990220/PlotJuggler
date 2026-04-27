#include "FileLoader.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "DialogPresenter.h"
#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_app_core/SessionManager.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcFileLoader, "pj.app.fileloader")

constexpr const char* kLastDirKey = "FileLoader/lastDir";
constexpr const char* kDefaultTimeDomainName = "default";
constexpr const char* kPluginConfigKeyPrefix = "PluginConfig/";

// Runtime host state for one file import. Only the bits CSV-style file sources
// actually exercise are wired up; parser-binding callbacks return false because
// self-parsing file sources (CSV, MCAP-via-bundled-parser) don't use them.
struct RuntimeHost {
  std::string last_error;
  std::atomic<bool> stop_requested{false};
};

bool failRuntime(RuntimeHost* state, PJ_error_t* out_error, const char* message) noexcept {
  state->last_error = message;
  PJ::sdk::fillError(out_error, 1, "pj.app.fileloader", state->last_error);
  return false;
}

void rhReportMessage(void* /*ctx*/, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
  const std::string_view text(message.data, message.size);
  switch (level) {
    case PJ_DATA_SOURCE_MESSAGE_ERROR:
      qCWarning(lcFileLoader) << "[plugin error]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    case PJ_DATA_SOURCE_MESSAGE_WARNING:
      qCWarning(lcFileLoader) << "[plugin warn]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    default:
      qCInfo(lcFileLoader) << "[plugin]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
  }
}

bool rhProgressStart(
    void* /*ctx*/, PJ_string_view_t /*label*/, uint64_t /*total*/, bool /*cancellable*/,
    PJ_error_t* /*out_error*/) noexcept {
  return true;  // Import progress is currently logged by plugin messages only.
}

bool rhProgressUpdate(void* ctx, uint64_t /*current*/) noexcept {
  return !static_cast<RuntimeHost*>(ctx)->stop_requested.load();
}

void rhProgressFinish(void* /*ctx*/) noexcept {}

bool rhIsStopRequested(void* ctx) noexcept {
  return static_cast<RuntimeHost*>(ctx)->stop_requested.load();
}

void rhNotifyState(void* /*ctx*/, PJ_data_source_state_t /*state*/) noexcept {}

void rhRequestStop(void* ctx, PJ_data_source_state_t /*terminal*/, PJ_string_view_t reason) noexcept {
  auto* state = static_cast<RuntimeHost*>(ctx);
  state->last_error.assign(reason.data, reason.size);
  state->stop_requested.store(true);
}

bool rhEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* /*request*/, PJ_parser_binding_handle_t* /*out*/,
    PJ_error_t* out_error) noexcept {
  return failRuntime(
      static_cast<RuntimeHost*>(ctx), out_error, "parser binding not supported (no MessageParser plugin wired in v1)");
}

bool rhPushRawMessage(
    void* ctx, PJ_parser_binding_handle_t /*handle*/, int64_t /*ts*/, PJ_bytes_view_t /*payload*/,
    PJ_error_t* out_error) noexcept {
  return failRuntime(static_cast<RuntimeHost*>(ctx), out_error, "push_raw_message called without parser binding");
}

int rhShowMessageBox(
    void* /*ctx*/, PJ_message_box_type_t /*type*/, PJ_string_view_t title, PJ_string_view_t message,
    int buttons) noexcept {
  // Headless default — log and pick a positive button. The host is single-
  // threaded for the import, but Qt modal dialogs from a plugin callback are
  // fragile, so v1 doesn't pop them. Plugins that need user input go through
  // their own dialog capability.
  qCInfo(lcFileLoader) << "[plugin msgbox]" << QString::fromUtf8(title.data, static_cast<int>(title.size)) << "—"
                       << QString::fromUtf8(message.data, static_cast<int>(message.size));
  if ((buttons & PJ_MSG_BTN_OK) != 0) {
    return PJ_MSG_BTN_OK;
  }
  if ((buttons & PJ_MSG_BTN_YES) != 0) {
    return PJ_MSG_BTN_YES;
  }
  if ((buttons & PJ_MSG_BTN_CONTINUE) != 0) {
    return PJ_MSG_BTN_CONTINUE;
  }
  return -1;
}

const char* rhListAvailableEncodings(void* /*ctx*/) noexcept {
  return nullptr;  // no parsers wired in v1
}

PJ_data_source_runtime_host_t makeRuntimeHost(RuntimeHost* state) {
  static const PJ_data_source_runtime_host_vtable_t vtable = {
      .protocol_version = 1,
      .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
      .report_message = rhReportMessage,
      .progress_start = rhProgressStart,
      .progress_update = rhProgressUpdate,
      .progress_finish = rhProgressFinish,
      .is_stop_requested = rhIsStopRequested,
      .notify_state = rhNotifyState,
      .request_stop = rhRequestStop,
      .ensure_parser_binding = rhEnsureParserBinding,
      .push_raw_message = rhPushRawMessage,
      .show_message_box = rhShowMessageBox,
      .list_available_encodings = rhListAvailableEncodings,
  };
  return PJ_data_source_runtime_host_t{.ctx = state, .vtable = &vtable};
}

QString normalizeExtension(const QString& path) {
  const QString suffix = QFileInfo(path).suffix();
  return suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix.toLower();
}

// Merge the file path into the (possibly empty) saved JSON config. Saved
// config carries the dialog state from the previous load (delimiter, time
// column, etc.) so the dialog opens pre-populated. If saved_config doesn't
// parse, treat it as empty rather than failing the import.
std::string buildLoadConfig(std::string_view saved_config, const QString& path) {
  QJsonObject obj;
  if (!saved_config.empty()) {
    const QByteArray bytes(saved_config.data(), static_cast<qsizetype>(saved_config.size()));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (doc.isObject()) {
      obj = doc.object();
    }
  }
  obj.insert(QStringLiteral("filepath"), path);
  const QByteArray out = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  return std::string(out.constData(), static_cast<std::size_t>(out.size()));
}

QString pluginConfigKey(const std::string& plugin_id) {
  return QString::fromLatin1(kPluginConfigKeyPrefix) + QString::fromStdString(plugin_id);
}

}  // namespace

FileLoader::FileLoader(
    SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent)
    : QObject(parent), session_(session), extensions_(extensions), catalog_(catalog) {}

FileLoader::~FileLoader() = default;

void FileLoader::openFromDialog(QWidget* dialog_parent) {
  QSettings settings;
  const QString last_dir = settings.value(kLastDirKey, QString()).toString();
  const QString filter = extensions_.buildFileFilter();

  const QString path = QFileDialog::getOpenFileName(dialog_parent, tr("Load data file"), last_dir, filter);
  if (path.isEmpty()) {
    return;
  }
  settings.setValue(kLastDirKey, QFileInfo(path).absolutePath());
  loadFile(path, dialog_parent);
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent) {
  // One unified failure path — log, optionally pop a dialog, emit signal.
  const auto fail = [&](const QString& reason) -> bool {
    qCWarning(lcFileLoader).noquote() << reason;
    if (dialog_parent != nullptr) {
      QMessageBox::warning(dialog_parent, tr("Load failed"), reason);
    }
    emit fileLoadFailed(path, reason);
    return false;
  };

  const QString ext = normalizeExtension(path);
  if (ext.isEmpty()) {
    return fail(tr("File has no extension; cannot pick a plugin."));
  }

  const std::vector<const LoadedDataSource*> matches = extensions_.findSourcesForExtension(ext);
  if (matches.empty()) {
    return fail(tr("No DataSource plugin handles %1 files. Install one from the Marketplace.").arg(ext));
  }

  // v1 picks the first match; M3+ can add a chooser when multiple plugins
  // claim the same extension.
  const LoadedDataSource* source = matches.front();
  const QString source_name = QString::fromStdString(source->name);

  DataSourceHandle handle = source->library.createHandle();
  if (!handle.valid()) {
    return fail(tr("Plugin '%1': createHandle failed.").arg(source_name));
  }

  // The v4 DataSource protocol resolves host services during bind(), so the
  // target dataset must exist before loadConfig() and start().
  DataEngine& engine = session_.dataEngine();
  const TimeDomainId td_id = ensureDefaultTimeDomainId();
  if (td_id == 0) {
    return fail(tr("Could not create the default time domain."));
  }

  const QString display_name = QFileInfo(path).fileName();
  auto dataset_or =
      engine.createDataset(DatasetDescriptor{.source_name = display_name.toStdString(), .time_domain_id = td_id});
  if (!dataset_or.has_value()) {
    return fail(tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error())));
  }

  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(*dataset_or)};
  DatastoreSourceWriteHost write_host(engine, source_handle);

  RuntimeHost runtime_state;
  ServiceRegistryBuilder registry;
  registry.registerService<sdk::SourceWriteHostService>(write_host.raw());
  registry.registerService<sdk::DataSourceRuntimeHostService>(makeRuntimeHost(&runtime_state));

  if (auto status = handle.bind(registry.view()); !status) {
    return fail(tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  // Pre-populate the dialog with last-used settings so users don't re-pick
  // delimiter/time column on every load. Key matches proto_app's scheme
  // (PluginConfig/<plugin name>) so layouts and saved configs read the same
  // setting across both apps.
  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source->name);
  const std::string saved_config = persisted_settings.value(config_key, QString()).toString().toStdString();

  std::string config = buildLoadConfig(saved_config, path);
  if (auto status = handle.loadConfig(config); !status) {
    return fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  // Show the plugin's configuration dialog when it advertises one. The
  // helper handles the capability check, vtable resolution, borrowed-handle
  // wiring, and parser-slot injection. We re-loadConfig on accept so the
  // source handle sees the dialog's choices before start() — DialogEngine
  // already wrote them back via the dialog vtable, but for plugins that
  // split dialog state from source state, the explicit reload keeps the
  // contract uniform.
  const auto dlg = dialog_presenter::showDataSourceDialog({
      .source = *source,
      .handle = handle,
      .catalog = extensions_,
      .parent = dialog_parent,
  });
  if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
    return false;
  }
  if (dlg.outcome == dialog_presenter::Outcome::kAccepted) {
    config = dlg.saved_config;
    if (auto status = handle.loadConfig(config); !status) {
      return fail(tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                      .arg(source_name, QString::fromStdString(status.error())));
    }
  }

  // Persist the resolved config before start() so the dialog choices stick
  // even if ingest fails afterwards (matches proto_app's onLoadFile, which
  // persists unconditionally after the import call).
  persisted_settings.setValue(config_key, QString::fromStdString(config));

  if (auto status = handle.start(); !status) {
    return fail(tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  write_host.flushPending();
  catalog_.rebuildFromDatastore();
  emit fileLoaded(path);
  return true;
}

TimeDomainId FileLoader::ensureDefaultTimeDomainId() {
  if (default_time_domain_id_ != 0) {
    return default_time_domain_id_;
  }
  auto domain_or = session_.dataEngine().createTimeDomain(kDefaultTimeDomainName);
  if (!domain_or.has_value()) {
    qCWarning(lcFileLoader) << "createTimeDomain failed:" << QString::fromStdString(domain_or.error());
    return 0;
  }
  default_time_domain_id_ = *domain_or;
  return default_time_domain_id_;
}

}  // namespace PJ
