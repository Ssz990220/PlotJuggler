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

#include "pj_app_core/CatalogModel.h"
#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_app_core/SessionManager.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcFileLoader, "pj.app.fileloader")

constexpr const char* kLastDirKey = "FileLoader/lastDir";
constexpr const char* kDefaultTimeDomainName = "default";

// Runtime host state for one file import. Only the bits CSV-style file sources
// actually exercise are wired up; parser-binding callbacks return false because
// self-parsing file sources (CSV, MCAP-via-bundled-parser) don't use them.
struct RuntimeHost {
  std::string last_error;
  std::atomic<bool> stop_requested{false};
};

const char* rhGetLastError(void* ctx) {
  auto* state = static_cast<RuntimeHost*>(ctx);
  return state->last_error.empty() ? nullptr : state->last_error.c_str();
}

void rhReportMessage(void* /*ctx*/, PJ_data_source_message_level_t level, PJ_string_view_t message) {
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

bool rhProgressStart(void* /*ctx*/, PJ_string_view_t /*label*/, uint64_t /*total*/, bool /*cancellable*/) {
  return false;  // host doesn't show progress for v1
}

bool rhProgressUpdate(void* ctx, uint64_t /*current*/) {
  return !static_cast<RuntimeHost*>(ctx)->stop_requested.load();
}

void rhProgressFinish(void* /*ctx*/) {}

bool rhIsStopRequested(void* ctx) {
  return static_cast<RuntimeHost*>(ctx)->stop_requested.load();
}

void rhNotifyState(void* /*ctx*/, PJ_data_source_state_t /*state*/) {}

void rhRequestStop(void* ctx, PJ_data_source_state_t /*terminal*/, PJ_string_view_t reason) {
  static_cast<RuntimeHost*>(ctx)->last_error.assign(reason.data, reason.size);
}

bool rhEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* /*request*/, PJ_parser_binding_handle_t* /*out*/) {
  static_cast<RuntimeHost*>(ctx)->last_error = "parser binding not supported (no MessageParser plugin wired in v1)";
  return false;
}

bool rhPushRawMessage(void* ctx, PJ_parser_binding_handle_t /*handle*/, int64_t /*ts*/, PJ_bytes_view_t /*payload*/) {
  static_cast<RuntimeHost*>(ctx)->last_error = "push_raw_message called without parser binding";
  return false;
}

int rhShowMessageBox(
    void* /*ctx*/, PJ_message_box_type_t /*type*/, PJ_string_view_t title, PJ_string_view_t message, int buttons) {
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

const char* rhListAvailableEncodings(void* /*ctx*/) {
  return nullptr;  // no parsers wired in v1
}

PJ_data_source_runtime_host_t makeRuntimeHost(RuntimeHost* state) {
  static const PJ_data_source_runtime_host_vtable_t vtable = {
      .protocol_version = PJ_DATA_SOURCE_PROTOCOL_VERSION,
      .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
      .get_last_error = rhGetLastError,
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

// Build a JSON config payload with the file path. Uses Qt's JSON serializer so
// paths with embedded quotes or backslashes (Windows portability) are escaped
// correctly — naive string concatenation would break loadConfig parsing.
std::string buildLoadConfig(const QString& path) {
  const QJsonObject obj{{QStringLiteral("filepath"), path}};
  const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
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

  // Stand up plugin scaffolding BEFORE creating the dataset. The most common
  // failure is loadConfig() rejecting a bad/missing path; if we created the
  // dataset first and then loadConfig failed, the engine would accumulate a
  // stale empty dataset (pj_datastore has no removeDataset API).
  DataSourceHandle handle = source->library.createHandle();
  if (!handle.valid()) {
    return fail(tr("Plugin '%1': createHandle failed.").arg(source->name));
  }

  RuntimeHost runtime_state;
  if (!handle.bindRuntimeHost(makeRuntimeHost(&runtime_state))) {
    return fail(
        tr("Plugin '%1': bindRuntimeHost failed: %2").arg(source->name, QString::fromStdString(handle.lastError())));
  }

  if (!handle.loadConfig(buildLoadConfig(path))) {
    return fail(tr("Plugin '%1': loadConfig failed: %2").arg(source->name, QString::fromStdString(handle.lastError())));
  }

  // Past this point, errors will leave a dataset behind (no removal API in
  // pj_datastore). Acceptable for v1: bind/start failures are rare and the
  // file path was already validated by loadConfig.
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
  if (!handle.bindWriteHost(write_host.raw())) {
    return fail(
        tr("Plugin '%1': bindWriteHost failed: %2").arg(source->name, QString::fromStdString(handle.lastError())));
  }

  if (!handle.start()) {
    return fail(tr("Plugin '%1': start failed: %2").arg(source->name, QString::fromStdString(handle.lastError())));
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
