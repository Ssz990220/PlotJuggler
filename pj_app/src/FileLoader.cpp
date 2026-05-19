#include "FileLoader.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProgressBar>
#include <QProgressDialog>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <string>
#include <string_view>

#include "DialogPresenter.h"
#include "MainWindow.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/MessageBox.h"

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcFileLoader, "pj.app.fileloader")

constexpr const char* kLastDirKey = "FileLoader/lastDir";
constexpr const char* kDefaultTimeDomainName = "default";
constexpr const char* kPluginConfigKeyPrefix = "PluginConfig/";

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

  // PJ::FileDialog wraps a non-native QFileDialog in our frameless
  // chrome — see pj_widgets/FileDialog.h. The native GTK dialog also
  // crashes on this app's libpng ABI skew (see the --exclude-libs,ALL
  // note in pj_app/CMakeLists.txt), so we avoid it both for look and
  // for stability. Passing the MainWindow as the metrics source primes
  // the toolbar icon size and keeps it in step via chromeMetricsChanged.
  auto* metrics_source = dialog_parent != nullptr ? qobject_cast<MainWindow*>(dialog_parent->window()) : nullptr;
  const QString path = FileDialog::getOpenFileName(dialog_parent, tr("Load Data"), last_dir, filter, metrics_source);
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
      MessageBox::warning(dialog_parent, tr("Load failed"), reason);
    }
    emit fileLoadFailed(path, reason);
    return false;
  };

  const QString ext = normalizeExtension(path);
  if (ext.isEmpty()) {
    return fail(tr("File has no extension; cannot pick a plugin."));
  }

  const auto matches = extensions_.findSourcesForExtension(ext);
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

  const auto dataset_id = static_cast<DatasetId>(*dataset_or);
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(*dataset_or)};
  DataSourceRuntimeHost ingest_session(
      engine, extensions_, dataset_id, source_handle, session_.objectStore(), source->id,
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_.registerObjectTopicParser(id, std::move(parser));
      });
  ingest_session.policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);
  ingest_session.policyResolver().setForType(
      PJ::sdk::BuiltinObjectType::kPointCloud, PJ::sdk::ObjectIngestPolicy::kPureLazy);

  ServiceRegistryBuilder registry;
  ingest_session.registerServices(registry);

  if (auto status = handle.bind(registry.view()); !status) {
    return fail(tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  // Pre-populate the dialog with last-used settings so users don't re-pick
  // delimiter/time column on every load.
  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source->name);
  const std::string saved_config = persisted_settings.value(config_key, QString()).toString().toStdString();

  std::string config = buildLoadConfig(saved_config, path);
  if (auto status = handle.loadConfig(config); !status) {
    return fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  const auto dlg = dialog_presenter::showDataSourceDialog({
      .source = *source,
      .handle = handle,
      .catalog = extensions_,
      .parent = dialog_parent,
  });
  if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
    return fail(tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                    .arg(QString::fromStdString(dlg.error)));
  }
  if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
    return false;
  }
  if (dlg.payload.has_value()) {
    config = dlg.payload->saved_config;
    // DialogEngine already wrote the dialog's choices back via the dialog vtable,
    // but for plugins that split dialog state from source state the explicit
    // reload keeps the contract uniform.
    if (auto status = handle.loadConfig(config); !status) {
      return fail(tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                      .arg(source_name, QString::fromStdString(status.error())));
    }
  }

  // Persist before start() so dialog choices stick even if ingest fails.
  persisted_settings.setValue(config_key, QString::fromStdString(config));

  // Progress dialog — shown when the plugin calls progressStart().
  // The import runs synchronously on the main thread, so we drive the dialog
  // with processEvents() inside the update callback.
  // Suppress the " — PlotJuggler 4" suffix the window manager appends when
  // applicationDisplayName is set. Restored automatically on scope exit.
  const QString saved_display_name = QGuiApplication::applicationDisplayName();
  QGuiApplication::setApplicationDisplayName(QString{});
  struct RestoreDisplayName {
    QString name;
    ~RestoreDisplayName() {
      QGuiApplication::setApplicationDisplayName(name);
    }
  } restore_display_name{saved_display_name};

  QProgressDialog progress_dlg(dialog_parent);
  progress_dlg.setWindowTitle(QString{});
  progress_dlg.setWindowModality(Qt::WindowModal);
  progress_dlg.setMinimumDuration(0);
  progress_dlg.setAutoClose(false);
  progress_dlg.setAutoReset(false);
  progress_dlg.setMinimumWidth(400);
  if (auto* bar = progress_dlg.findChild<QProgressBar*>()) {
    bar->setAlignment(Qt::AlignCenter);
    bar->setTextVisible(true);
  }

  ingest_session.onProgressStart = [&progress_dlg](std::string_view label, uint64_t total, bool cancellable) {
    const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
    progress_dlg.setWindowTitle(title);
    progress_dlg.setLabelText(QString{});
    progress_dlg.setRange(0, total > 0 ? static_cast<int>(total) : 0);
    progress_dlg.setValue(0);
    progress_dlg.setCancelButtonText(cancellable ? tr("Cancel") : QString{});
    QCoreApplication::processEvents();
  };

  ingest_session.onProgressUpdate = [&progress_dlg, &ingest_session](uint64_t current) -> bool {
    progress_dlg.setValue(static_cast<int>(current));
    QCoreApplication::processEvents();
    if (progress_dlg.wasCanceled()) {
      ingest_session.requestStop("cancelled by user");
      return false;
    }
    return true;
  };

  ingest_session.onProgressFinish = [&progress_dlg]() {
    progress_dlg.setValue(progress_dlg.maximum());
    QCoreApplication::processEvents();
    progress_dlg.reset();
  };

  if (auto status = handle.start(); !status) {
    progress_dlg.reset();
    return fail(tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  if (ingest_session.stopRequested()) {
    qCWarning(lcFileLoader) << "[FileLoader] import cancelled by user, reason:"
                            << QString::fromStdString(ingest_session.lastError());
  }

  ingest_session.flushAll();
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
