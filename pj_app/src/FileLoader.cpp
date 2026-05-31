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
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "DialogPresenter.h"
#include "FanoutConfig.h"
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

// Default ingest policies the app applies to every DataSourceRuntimeHost it
// builds: scalars eager, objects lazy (decoded on pull), point clouds always
// pure-lazy (they're the heaviest payload). Hoisted here so the pre-dialog
// scratch session and the per-fanout loop iterations stay in lockstep.
void applyDefaultIngestPolicies(DataSourceRuntimeHost& session) {
  session.policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);
  session.policyResolver().setForType(PJ::sdk::BuiltinObjectType::kPointCloud, PJ::sdk::ObjectIngestPolicy::kPureLazy);
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

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints) {
  // Restore same-source datasets if a replacement load is cancelled or fails.
  std::vector<DatasetId> tombstoned_for_replace;
  const auto rollbackTombstones = [&]() {
    for (const DatasetId id : tombstoned_for_replace) {
      catalog_.restoreDataset(id);
    }
    tombstoned_for_replace.clear();
  };

  // One unified failure path — log, optionally pop a dialog, emit signal.
  const auto fail = [&](const QString& reason) -> bool {
    rollbackTombstones();
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
  const std::string display_name_utf8 = display_name.toStdString();

  // Same-source handling: layout replay reuses the existing DatasetId;
  // interactive load/reload hides matches before a fresh ingest. Matching
  // still uses the file basename, so same-basename files remain ambiguous.
  for (const auto existing_id : engine.listDatasets()) {
    const DatasetInfo* info = engine.getDataset(existing_id);
    if (info == nullptr || info->source_name != display_name_utf8) {
      continue;
    }
    if (hints.prefer_reuse) {
      // Reuse the id referenced by the layout; keep the recorded config
      // when legacy XML has no preset.
      QString emit_config = hints.preset_config_json;
      if (emit_config.isEmpty()) {
        const auto prev = session_.lastLoadedSource();
        if (prev.has_value() && prev->path == path) {
          emit_config = prev->plugin_config_json;
        }
      }
      catalog_.restoreDataset(existing_id);
      emit fileLoaded(path, QString(), source_name, emit_config);
      return true;
    }
    // Interactive load/reload replaces the caller-visible dataset.
    // The datastore keeps old data; the catalog tombstone hides it.
    if (catalog_.removeDataset(existing_id)) {
      tombstoned_for_replace.push_back(existing_id);
    }
  }

  auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = display_name_utf8, .time_domain_id = td_id});
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
  applyDefaultIngestPolicies(ingest_session);

  ServiceRegistryBuilder registry;
  ingest_session.registerServices(registry);

  if (auto status = handle.bind(registry.view()); !status) {
    return fail(tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error())));
  }

  // Pre-populate the dialog with last-used settings so users don't re-pick
  // delimiter/time column on every load. The layout-driven path (hints
  // with a matching plugin id + a usable preset config) skips the dialog
  // entirely; we fall back to the dialog with the QSettings pre-fill if
  // either the id mismatches or loadConfig rejects the preset.
  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source->name);
  const std::string saved_config = persisted_settings.value(config_key, QString()).toString().toStdString();

  std::string config;
  bool skip_dialog = false;

  const bool hint_eligible =
      hints.skip_dialog && !hints.preset_config_json.isEmpty() && hints.expected_plugin_id == source_name;
  if (hint_eligible) {
    const std::string preset = hints.preset_config_json.toStdString();
    if (auto status = handle.loadConfig(preset); status) {
      config = preset;
      skip_dialog = true;
    } else {
      // Silent fallback: layout's config didn't take. Use the QSettings
      // pre-fill and let the dialog drive — caller's UX is "if it works,
      // skip; if not, ask."
      qCInfo(lcFileLoader).noquote() << tr("Layout preset rejected by '%1': %2 — falling back to dialog")
                                            .arg(source_name, QString::fromStdString(status.error()));
      config = buildLoadConfig(saved_config, path);
      if (auto retry = handle.loadConfig(config); !retry) {
        return fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(retry.error())));
      }
    }
  } else {
    config = buildLoadConfig(saved_config, path);
    if (auto status = handle.loadConfig(config); !status) {
      return fail(tr("Plugin '%1': loadConfig failed: %2").arg(source_name, QString::fromStdString(status.error())));
    }
  }

  if (!skip_dialog) {
    // Extract parser config saved from a previous session so DialogEngine can
    // restore the embedded parser dialog to its last state.
    std::string initial_parser_config;
    {
      auto saved_cfg = nlohmann::json::parse(saved_config, nullptr, false);
      if (!saved_cfg.is_discarded() && saved_cfg.contains("_parser_config")) {
        initial_parser_config = saved_cfg["_parser_config"].get<std::string>();
      }
    }

    const auto dlg = dialog_presenter::showDataSourceDialog({
        .source = *source,
        .handle = handle,
        .catalog = extensions_,
        .parent = dialog_parent,
        .initial_parser_config = initial_parser_config,
    });
    if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
      return fail(tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                      .arg(QString::fromStdString(dlg.error)));
    }
    if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
      rollbackTombstones();
      return false;
    }
    if (dlg.payload.has_value()) {
      config = dlg.payload->saved_config;
      // If the dialog had an embedded parser slot, embed the parser config in the
      // saved config so it survives across sessions and reaches the source via
      // loadConfig(). The source extracts it under the "_parser_config" key.
      if (!dlg.payload->parser_config.empty()) {
        auto cfg = nlohmann::json::parse(config, nullptr, false);
        if (!cfg.is_discarded()) {
          cfg["_parser_config"] = dlg.payload->parser_config;
          config = cfg.dump();
        }
      }
      // DialogEngine already wrote the dialog's choices back via the dialog vtable,
      // but for plugins that split dialog state from source state the explicit
      // reload keeps the contract uniform.
      if (auto status = handle.loadConfig(config); !status) {
        return fail(tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                        .arg(source_name, QString::fromStdString(status.error())));
      }
    }
  }

  // Persist before start() so dialog choices stick even if ingest fails.
  // Skip on the hint path: layout-driven reloads should NOT overwrite the
  // user's last interactive choice in QSettings (spec §11). Otherwise
  // opening a layout would silently mutate the global per-plugin pre-fill.
  if (!skip_dialog) {
    persisted_settings.setValue(config_key, QString::fromStdString(config));
  }

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

  // Latch cancellation the moment we observe it (inside onProgressUpdate). We
  // can't trust progress_dlg.wasCanceled() after an import returns: on normal
  // completion onProgressFinish calls reset(), which clears that flag, so the
  // post-import checks below (single-instance and the fanout loop) could miss a
  // real cancel. A separate sticky bool is reliable regardless of which exit
  // path ran.
  bool user_cancelled = false;

  // Progress callbacks are re-wired per ingest_session (once for single-instance,
  // N times in fanout mode) — the dialog itself is shared.
  auto wireProgress = [&progress_dlg, &user_cancelled](DataSourceRuntimeHost& session) {
    session.onProgressStart = [&progress_dlg](std::string_view label, uint64_t total, bool cancellable) {
      const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
      progress_dlg.setWindowTitle(title);
      progress_dlg.setLabelText(QString{});
      progress_dlg.setRange(0, total > 0 ? static_cast<int>(total) : 0);
      progress_dlg.setValue(0);
      progress_dlg.setCancelButtonText(cancellable ? tr("Cancel") : QString{});
      QCoreApplication::processEvents();
    };
    session.onProgressUpdate = [&progress_dlg, &session, &user_cancelled](uint64_t current) -> bool {
      progress_dlg.setValue(static_cast<int>(current));
      QCoreApplication::processEvents();
      if (progress_dlg.wasCanceled()) {
        user_cancelled = true;
        session.requestStop("cancelled by user");
        return false;
      }
      return true;
    };
    session.onProgressFinish = [&progress_dlg]() {
      progress_dlg.setValue(progress_dlg.maximum());
      QCoreApplication::processEvents();
      progress_dlg.reset();
    };
  };

  // Detect multi-instance fanout. A DataSource plugin emits a `__pj_fanout`
  // array on accept when one selection should expand into several independent
  // imports — each entry becomes its own DatasetId. For single-instance
  // importers the helper returns `{ config }` and the legacy flow runs unchanged.
  const auto fanouts = detail::extractFanout(config);

  if (fanouts.size() == 1) {
    // Single-instance: reuse the already-bound scratch handle + dataset.
    // issue #98: apply the plugin's dataset name BEFORE start() so the
    // commit-driven catalog rebuild surfaces curves already under the right
    // tree-root label. rebuildFromDatastore signals add/remove keyed by curve
    // identity, never relabels, so overriding after the curves are shown would
    // not reach the tree view on the initial load — mirror how fanout sets its
    // labels at createDataset time, before any topic is committed.
    if (const QString plugin_name = detail::parseDisplayName(config); !plugin_name.isEmpty()) {
      catalog_.setDatasetDisplayName(dataset_id, plugin_name);
    }
    wireProgress(ingest_session);
    if (auto status = handle.start(); !status) {
      progress_dlg.reset();
      return fail(tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(status.error())));
    }
    ingest_session.flushAll();
    // FileSourceBase::start() calls requestStop(..., "import complete") on the
    // normal success path (plotjuggler_core pj_base/.../sdk/data_source_patterns.hpp),
    // so stopRequested() can't distinguish completion from cancel — consult the
    // sticky flag. The rows already committed by flushAll() can't be rolled back
    // (ObjectStore writes are immediate and there is no removeDataset), so a
    // cancelled import leaves partial data; surface that instead of returning a
    // silent success.
    if (user_cancelled) {
      qCWarning(lcFileLoader) << "[FileLoader] import cancelled by user, reason:"
                              << QString::fromStdString(ingest_session.lastError());
      if (dialog_parent != nullptr) {
        MessageBox::warning(
            dialog_parent, tr("Import cancelled"), tr("The import was cancelled; the loaded data may be incomplete."));
      }
    }
  } else {
    // Multi-instance fanout. The pre-dialog scratch dataset is now an empty
    // orphan — pj_datastore has no removeDataset, so we accept the cost (an
    // empty dataset has no committed topics, so CatalogModel::rebuildFromDatastore
    // skips it — no phantom catalog entry). Each fanout entry mints its own
    // handle + dataset + ingest_session. Continue-on-error per the user-confirmed
    // policy: a bad entry does not lose the others.
    enum class EntryOutcome { Completed, Failed, Cancelled };

    const QString basename = QFileInfo(path).completeBaseName();
    // issue #98: let the plugin name the dataset root. `display_name` (if the
    // plugin emitted it in the accepted config) replaces the file basename as
    // the shared prefix; the per-episode `display_suffix` still forms the leaf.
    const QString fanout_name = detail::parseDisplayName(config);
    const QString base = fanout_name.isEmpty() ? basename : fanout_name;
    std::size_t completed = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    QStringList failed_labels;

    // Per-fanout-iteration runner. Creates a fresh dataset + handle + ingest
    // host, binds, loadConfig's the per-entry cfg, then runs the import. Logs a
    // context-rich warning on every failure mode so partial imports are
    // diagnosable from the log alone. Returns Failed before the import starts,
    // Cancelled if the user cancelled during it, else Completed.
    auto runFanoutEntry = [&](std::size_t idx, const std::string& cfg_i, const QString& iter_display) -> EntryOutcome {
      auto iter_dataset_or =
          engine.createDataset(DatasetDescriptor{.source_name = iter_display.toStdString(), .time_domain_id = td_id});
      if (!iter_dataset_or.has_value()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: createDataset failed:" << QString::fromStdString(iter_dataset_or.error());
        return EntryOutcome::Failed;
      }
      const auto iter_dataset_id = static_cast<DatasetId>(*iter_dataset_or);
      const PJ_data_source_handle_t iter_source_handle{static_cast<uint32_t>(iter_dataset_id)};

      DataSourceHandle iter_handle = source->library.createHandle();
      if (!iter_handle.valid()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx << "]: createHandle failed";
        return EntryOutcome::Failed;
      }

      DataSourceRuntimeHost iter_ingest(
          engine, extensions_, iter_dataset_id, iter_source_handle, session_.objectStore(), source->id,
          [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
            session_.registerObjectTopicParser(id, std::move(parser));
          });
      applyDefaultIngestPolicies(iter_ingest);

      ServiceRegistryBuilder iter_registry;
      iter_ingest.registerServices(iter_registry);

      if (auto status = iter_handle.bind(iter_registry.view()); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: bind failed:" << QString::fromStdString(status.error());
        return EntryOutcome::Failed;
      }
      if (auto status = iter_handle.loadConfig(cfg_i); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: loadConfig failed:" << QString::fromStdString(status.error());
        return EntryOutcome::Failed;
      }

      wireProgress(iter_ingest);
      progress_dlg.setLabelText(tr("Importing %1 (%2/%3)").arg(iter_display).arg(idx + 1).arg(fanouts.size()));

      if (auto status = iter_handle.start(); !status) {
        progress_dlg.reset();
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: start failed:" << QString::fromStdString(status.error());
        return EntryOutcome::Failed;
      }
      // Commit whatever landed so the dataset is internally consistent. On
      // cancel we still flush — the alternative (dropping unflushed scalars
      // while ObjectStore payloads, written immediately, stay committed) would
      // leave a half-written dataset — and report it as Cancelled rather than a
      // clean success. user_cancelled is the canonical cancel signal (see the
      // declaration above); it can only have flipped during this entry's import,
      // since the loop breaks on cancel.
      iter_ingest.flushAll();
      return user_cancelled ? EntryOutcome::Cancelled : EntryOutcome::Completed;
    };

    for (std::size_t i = 0; i < fanouts.size(); ++i) {
      const std::string& cfg_i = fanouts[i];
      const QString suffix = detail::parseDisplaySuffix(cfg_i, QString::number(i + 1));
      const QString iter_display = base + QChar('/') + suffix;

      switch (runFanoutEntry(i, cfg_i, iter_display)) {
        case EntryOutcome::Completed:
          ++completed;
          break;
        case EntryOutcome::Failed:
          ++failed;
          failed_labels << iter_display;
          break;
        case EntryOutcome::Cancelled:
          cancelled = true;
          break;
      }
      if (cancelled) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout: user cancelled at entry" << (i + 1) << "of" << fanouts.size();
        break;
      }
    }

    const int total = static_cast<int>(fanouts.size());
    const bool all_ok = !cancelled && failed == 0 && static_cast<int>(completed) == total;
    if (!all_ok && dialog_parent != nullptr) {
      QString msg = tr("Imported %1 of %2 dataset(s).").arg(completed).arg(total);
      if (failed > 0) {
        msg += QChar('\n') + tr("%1 failed: %2.").arg(failed).arg(failed_labels.join(QStringLiteral(", ")));
      }
      if (cancelled) {
        msg += QChar('\n') +
               tr("Import cancelled; the remaining datasets were skipped and the cancelled one may be partial.");
      }
      MessageBox::warning(dialog_parent, cancelled ? tr("Import cancelled") : tr("Partial import"), msg);
    }
  }

  catalog_.rebuildFromDatastore();

  // Capture the plugin's canonical post-load state AFTER start() + ingest
  // so any state computed during the actual load (discovered fields,
  // applied defaults, ingest-time policy overrides) is included in what
  // a layout file persists. saveConfig failures here are non-fatal —
  // the layout save just won't carry plugin config.
  std::string captured_config;
  if (auto status = handle.saveConfig(captured_config); !status) {
    qCWarning(lcFileLoader).noquote() << tr("Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                             .arg(source_name, QString::fromStdString(status.error()));
    captured_config.clear();
  }

  emit fileLoaded(path, QString(), source_name, QString::fromStdString(captured_config));
  return true;
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent) {
  return loadFile(path, dialog_parent, LoadHints{});
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
