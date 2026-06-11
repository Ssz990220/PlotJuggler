// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileLoader.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DialogPresenter.h"
#include "FanoutConfig.h"
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
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/ProgressDialog.h"

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
// builds: scalars eager, objects lazy (decoded on pull), point clouds and
// video frames always pure-lazy. Both carry the heaviest payloads; for a
// CompressedPointCloud this defers the Draco/Cloudini transcode to the render
// path, and for file-backed video it keeps each entry's bitstream NON-resident
// by re-invoking the producer's fetcher on every read instead of pinning the
// bytes at ingest. Hoisted here so the pre-dialog scratch session and the
// per-fanout loop iterations stay in lockstep.
void applyDefaultIngestPolicies(DataSourceRuntimeHost& session) {
  session.policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);
  session.policyResolver().setForType(PJ::sdk::BuiltinObjectType::kPointCloud, PJ::sdk::ObjectIngestPolicy::kPureLazy);
  session.policyResolver().setForType(
      PJ::sdk::BuiltinObjectType::kCompressedPointCloud, PJ::sdk::ObjectIngestPolicy::kPureLazy);
  // SceneEntities (markers) and ImageAnnotations carry no scalar fields — an
  // eager-scalar parse would fail, so keep them pure-lazy like point clouds.
  session.policyResolver().setForType(
      PJ::sdk::BuiltinObjectType::kSceneEntities, PJ::sdk::ObjectIngestPolicy::kPureLazy);
  session.policyResolver().setForType(
      PJ::sdk::BuiltinObjectType::kImageAnnotations, PJ::sdk::ObjectIngestPolicy::kPureLazy);
  session.policyResolver().setForType(PJ::sdk::BuiltinObjectType::kVideoFrame, PJ::sdk::ObjectIngestPolicy::kPureLazy);
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
  // for stability. The shell-injected picker threads MainWindow's chrome
  // metrics into the dialog (toolbar icon size, kept in step via
  // chromeMetricsChanged) — see setFilePicker().
  const QString path = file_picker_ != nullptr
                           ? file_picker_(dialog_parent, tr("Load Data"), last_dir, filter)
                           : FileDialog::getOpenFileName(dialog_parent, tr("Load Data"), last_dir, filter);
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

  // Same-source handling: layout replay reuses the existing DatasetId; an interactive load/reload replaces the
  // dataset's data in place, keeping its DatasetId/TopicIds (and so all curve keys) stable. Matching is by file
  // basename, so same-basename files are one source.
  DatasetId existing_primary_id = 0;
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
    // Tombstone is deferred to the post-ingest swap (single-instance) or the fanout fallback below: don't disturb
    // the live dataset until the staged ingest has succeeded.
    existing_primary_id = existing_id;
    break;
  }

  // Ingest target. A first load writes straight into the live engine/store. A same-source reload stages into a
  // throwaway secondary engine/store, so the primary stays live and readable until one synchronous swap after a
  // successful ingest. Only the single-instance case swaps; a fanout reload falls back to the legacy path.
  const bool replacing = (existing_primary_id != 0);
  DataEngine staged_engine;
  ObjectStore staged_store;
  DataEngine& target_engine = replacing ? staged_engine : engine;
  ObjectStore& target_store = replacing ? staged_store : session_.objectStore();
  TimeDomainId target_td_id = td_id;
  if (replacing) {
    auto staged_td = staged_engine.createTimeDomain("default");
    if (!staged_td.has_value()) {
      return fail(tr("Could not create the staging time domain."));
    }
    target_td_id = *staged_td;
  }

  auto dataset_or =
      target_engine.createDataset(DatasetDescriptor{.source_name = display_name_utf8, .time_domain_id = target_td_id});
  if (!dataset_or.has_value()) {
    return fail(tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error())));
  }

  const auto dataset_id = static_cast<DatasetId>(*dataset_or);
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(*dataset_or)};

  // On the replace path the staged ObjectTopicIds are throwaway; collect the
  // parsers and re-register them under the stable primary ids after the swap.
  std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers;
  DataSourceRuntimeHost::ObjectTopicParserRegistrar object_parser_registrar;
  if (replacing) {
    object_parser_registrar = [&staged_object_parsers](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
      staged_object_parsers.emplace_back(id, std::move(parser));
    };
  } else {
    object_parser_registrar = [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
      session_.registerObjectTopicParser(id, std::move(parser));
    };
  }
  // Must write into target_engine/target_store — the staged pair the swap
  // consumes on the replace path (see the staging block above; pinned by
  // file_loader_test).
  DataSourceRuntimeHost ingest_session(
      target_engine, extensions_, dataset_id, source_handle, target_store, source->id,
      std::move(object_parser_registrar), nullptr, nullptr, handle.libraryOwner());
  if (dialog_parent != nullptr) {
    ingest_session.setMessageBoxHandler(
        [dialog_parent](int type, std::string_view title, std::string_view message, int buttons) -> int {
          const QString q_title = QString::fromUtf8(title.data(), static_cast<int>(title.size()));
          const QString q_text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));

          QMessageBox msgBox(dialog_parent);
          msgBox.setWindowTitle(q_title);
          msgBox.setText(q_text);
          switch (type) {
            case PJ_MESSAGE_BOX_WARNING:
              msgBox.setIcon(QMessageBox::Warning);
              break;
            case PJ_MESSAGE_BOX_ERROR:
              msgBox.setIcon(QMessageBox::Critical);
              break;
            case PJ_MESSAGE_BOX_QUESTION:
              msgBox.setIcon(QMessageBox::Question);
              break;
            default:
              msgBox.setIcon(QMessageBox::Information);
              break;
          }
          QPushButton* btn_ok = (buttons & PJ_MSG_BTN_OK) ? msgBox.addButton(QMessageBox::Ok) : nullptr;
          QPushButton* btn_cancel = (buttons & PJ_MSG_BTN_CANCEL) ? msgBox.addButton(QMessageBox::Cancel) : nullptr;
          QPushButton* btn_yes = (buttons & PJ_MSG_BTN_YES) ? msgBox.addButton(QMessageBox::Yes) : nullptr;
          QPushButton* btn_no = (buttons & PJ_MSG_BTN_NO) ? msgBox.addButton(QMessageBox::No) : nullptr;
          QPushButton* btn_continue = (buttons & PJ_MSG_BTN_CONTINUE)
                                          ? msgBox.addButton(QObject::tr("Continue"), QMessageBox::AcceptRole)
                                          : nullptr;
          QPushButton* btn_abort =
              (buttons & PJ_MSG_BTN_ABORT) ? msgBox.addButton(QObject::tr("Abort"), QMessageBox::RejectRole) : nullptr;

          msgBox.exec();
          const auto* clicked = msgBox.clickedButton();
          if (clicked == btn_continue) {
            return PJ_MSG_BTN_CONTINUE;
          }
          if (clicked == btn_abort) {
            return PJ_MSG_BTN_ABORT;
          }
          if (clicked == btn_yes) {
            return PJ_MSG_BTN_YES;
          }
          if (clicked == btn_no) {
            return PJ_MSG_BTN_NO;
          }
          if (clicked == btn_ok) {
            return PJ_MSG_BTN_OK;
          }
          if (clicked == btn_cancel) {
            return PJ_MSG_BTN_CANCEL;
          }
          return -1;
        });
  }
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
  // Two-way stop semantics: both interrupt the ingest, they differ in what
  // happens to the data parsed before the click.
  //   - Keep:    stop reading; flush the partial data so it appears in the
  //              tree (button labelled "Cancel" in the UI).
  //   - Discard: stop reading and throw the partial data away (no flush,
  //              evict ObjectStore payloads, drop the dataset).
  // None  = no user request, the import ran to completion.
  enum class CancelAction { None, Keep, Discard };
  CancelAction user_action = CancelAction::None;

  // App-styled progress dialog with two stop buttons. It is domain-neutral:
  // it reports Primary / Secondary and we map those to CancelAction here
  // (Primary = Cancel/keep, Secondary = Discard).
  ProgressDialog progress_dlg(dialog_parent);
  progress_dlg.setPrimaryButton(
      tr("Cancel"), QStringLiteral(":/resources/svg/cancel_keep.svg"),
      tr("Stop reading; keep the data parsed so far."));
  progress_dlg.setSecondaryButton(
      tr("Discard"), QStringLiteral(":/resources/svg/cancel_discard.svg"),
      tr("Stop reading and discard the partial data."));

  // Progress callbacks are re-wired per ingest_session (once for single-instance,
  // N times in fanout mode) — the dialog itself is shared.
  auto wireProgress = [&](DataSourceRuntimeHost& session) {
    session.onProgressStart = [&](std::string_view label, uint64_t total, bool cancellable) {
      const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
      progress_dlg.setDialogTitle(title);
      progress_dlg.setMessage(QString{});
      progress_dlg.setRange(0, total > 0 ? static_cast<int>(total) : 0);
      progress_dlg.setValue(0);
      progress_dlg.setStopButtonsVisible(cancellable);
      if (!progress_dlg.isVisible()) {
        progress_dlg.show();
      }
      QCoreApplication::processEvents();
    };
    session.onProgressUpdate = [&](uint64_t current) -> bool {
      progress_dlg.setValue(static_cast<int>(current));
      QCoreApplication::processEvents();
      switch (progress_dlg.action()) {
        case ProgressDialog::Action::Primary:
          user_action = CancelAction::Keep;
          break;
        case ProgressDialog::Action::Secondary:
          user_action = CancelAction::Discard;
          break;
        case ProgressDialog::Action::None:
          return true;
      }
      session.requestStop(
          user_action == CancelAction::Keep ? "cancelled by user (keep partial)" : "cancelled by user (discard)");
      return false;
    };
    session.onProgressFinish = [&]() {
      progress_dlg.setValue(progress_dlg.maximum());
      QCoreApplication::processEvents();
    };
  };

  // Detect multi-instance fanout. A DataSource plugin emits a `__pj_fanout`
  // array on accept when one selection should expand into several independent
  // imports — each entry becomes its own DatasetId. For single-instance
  // importers the helper returns `{ config }` and the legacy flow runs unchanged.
  const auto fanouts = detail::extractFanout(config);

  // Live DatasetIds that fanout entries actually loaded data into (Completed or
  // Cancel-kept); the post-load TF ingest below runs on these. The pre-branch
  // scratch dataset never qualifies — it stays empty in fanout mode, and on a
  // replacing fanout its id is staged-engine-scoped (it may alias an unrelated
  // live dataset).
  std::vector<DatasetId> fanout_loaded_ids;

  if (fanouts.size() == 1) {
    // Single-instance: reuse the already-bound scratch handle + dataset.
    // issue #98: apply the plugin's dataset name BEFORE start() so the
    // commit-driven catalog rebuild surfaces curves already under the right
    // tree-root label. rebuildFromDatastore signals add/remove keyed by curve
    // identity, never relabels, so overriding after the curves are shown would
    // not reach the tree view on the initial load — mirror how fanout sets its
    // labels at createDataset time, before any topic is committed.
    // On the replace path `dataset_id` is the throwaway staged id; the swap
    // block applies the plugin name to the stable primary id instead.
    if (const QString plugin_name = detail::parseDisplayName(config); !replacing && !plugin_name.isEmpty()) {
      catalog_.setDatasetDisplayName(dataset_id, plugin_name);
    }
    wireProgress(ingest_session);
    if (auto status = handle.start(); !status) {
      progress_dlg.hide();
      return fail(tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(status.error())));
    }
    // Two-way stop: Discard throws the partial parse away, Cancel keeps it.
    //
    // Discard path: a file load never flushes until the terminal flushAll()
    // below, and ~DataSourceRuntimeHost never flushes (flushAll() is the only
    // path that makes rows visible), so skipping it leaves every buffered
    // scalar row invisible. ObjectStore payloads are written immediately, so
    // evict them explicitly; removeDataset() drops the never-committed dataset
    // from the catalog. Return early — no flush, no rebuild, no fileLoaded.
    // The replace path needs none of this: its swap below is gated on
    // user_action == None and the staged engine/store are discarded on
    // return, so the original data stays intact.
    //
    // Cancel(keep) path: fall through to flushAll() so the rows parsed before
    // the user clicked Cancel surface in the tree. Replace-on-cancel is still
    // refused below (the swap requires a complete read).
    if (user_action == CancelAction::Discard) {
      qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; partial data dropped";
      if (!replacing) {
        session_.evictDatasetObjects(dataset_id);
        catalog_.removeDataset(dataset_id);
      }
      return false;
    }
    if (user_action == CancelAction::Keep) {
      qCInfo(lcFileLoader) << "[FileLoader] import cancelled by user; keeping the partial load";
    }
    ingest_session.flushAll();
  } else {
    // A same-source reload that fans out cannot replace in place (one source becomes N datasets). Fall back to legacy
    // replace: tombstone the existing dataset now (objects evicted past the rollback point below) and let the fanout
    // create fresh datasets on the primary engine. The staged scratch is discarded with staged_engine.
    if (replacing && catalog_.removeDataset(existing_primary_id)) {
      tombstoned_for_replace.push_back(existing_primary_id);
    }
    // Multi-instance fanout. The first-load scratch dataset is now an empty orphan; pj_datastore has no removeDataset,
    // but an empty dataset has no committed topics so CatalogModel::rebuildFromDatastore skips it (no phantom entry).
    // Each fanout entry mints its own handle + dataset + ingest_session. Continue-on-error per the user-confirmed
    // policy: a bad entry does not lose the others.
    // Outcomes per fanout entry. Kept keeps the entry's partial flush
    // ("Cancel" — stop here but keep what was already parsed); Discarded
    // throws it away. Both stop the outer loop.
    enum class EntryOutcome { Completed, Failed, Kept, Discarded };

    const QString basename = QFileInfo(path).completeBaseName();
    // issue #98: let the plugin name the dataset root. `display_name` (if the
    // plugin emitted it in the accepted config) replaces the file basename as
    // the shared prefix; the per-episode `display_suffix` still forms the leaf.
    const QString fanout_name = detail::parseDisplayName(config);
    const QString base = fanout_name.isEmpty() ? basename : fanout_name;
    std::size_t completed = 0;
    std::size_t failed = 0;
    bool stopped = false;  // Cancel or Abort by the user during the loop.
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
          },
          nullptr, nullptr, iter_handle.libraryOwner());
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
      progress_dlg.setMessage(tr("Importing %1 (%2/%3)").arg(iter_display).arg(idx + 1).arg(fanouts.size()));

      if (auto status = iter_handle.start(); !status) {
        progress_dlg.hide();
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: start failed:" << QString::fromStdString(status.error());
        return EntryOutcome::Failed;
      }
      // Discard = drop this entry entirely. Skip flushAll() so its buffered
      // scalar rows never become visible, then evict the immediately-written
      // ObjectStore payloads and drop the dataset — no half-written remnant.
      // user_action can only have flipped during this entry's import, since
      // the loop breaks on Keep/Discard. Entries that already Completed stay
      // loaded.
      if (user_action == CancelAction::Discard) {
        session_.evictDatasetObjects(iter_dataset_id);
        catalog_.removeDataset(iter_dataset_id);
        return EntryOutcome::Discarded;
      }
      // Cancel(Keep) = keep what was already parsed for this entry
      // (flushAll), then stop the outer loop so subsequent entries are
      // skipped.
      iter_ingest.flushAll();
      fanout_loaded_ids.push_back(iter_dataset_id);
      if (user_action == CancelAction::Keep) {
        return EntryOutcome::Kept;
      }
      return EntryOutcome::Completed;
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
        case EntryOutcome::Kept:
          // Cancel: this entry kept its partial flush; the remaining entries
          // are skipped.
          ++completed;
          stopped = true;
          break;
        case EntryOutcome::Discarded:
          // Discard: this entry is dropped; the remaining entries are skipped.
          stopped = true;
          break;
      }
      if (stopped) {
        qCInfo(lcFileLoader) << "[FileLoader] fanout: user stopped (" << static_cast<int>(user_action) << ") at entry"
                             << (i + 1) << "of" << fanouts.size();
        break;
      }
    }
  }

  // Past the last rollback point: the load committed. Reconcile the staged data (replace path) or the tombstones
  // (legacy path) into the live session. A cancelled reload skips the swap, keeping the original data intact.
  // Allow the swap on Cancel(Keep) too: the partial flush has already landed
  // on the staged engine/store, and "keep what was already parsed" means the
  // user wants those rows to replace the previous live data. Only Discard
  // suppresses the swap (the staged side is intentionally thrown away).
  const bool swapped_in_place = replacing && fanouts.size() == 1 && user_action != CancelAction::Discard;
  if (swapped_in_place) {
    // Single-instance reload: in-place replace swap. SessionManager owns the ordered, no-event-loop swap (invalidate
    // adapters -> engine + object replace -> parser remap -> re-index). It keeps the primary
    // DatasetId/TopicIds/ObjectTopicIds — and so curve keys + 2D dock bindings — stable, so widgets keep their
    // curves. Do not pump events before the catalog rebuild below.
    session_.replaceDataset(
        staged_engine, staged_store, dataset_id, existing_primary_id, std::move(staged_object_parsers));

    // Un-tombstone the reused id: removeDataset hides it assuming a reload mints a new DatasetId, but the in-place
    // replace keeps it stable — without this the rebuild below skips the reloaded dataset (empty curve tree).
    catalog_.restoreDataset(existing_primary_id);

    // Re-apply the plugin's dataset-root name on the stable primary id (#98). An empty name clears a stale override.
    catalog_.setDatasetDisplayName(existing_primary_id, detail::parseDisplayName(config));
  } else {
    // Legacy path (first load, or fanout-reload fallback). Tombstoned same-source datasets are now permanently gone;
    // free their heavy ObjectStore topics (removeDataset only hid scalar data, which the engine keeps append-only)
    // and the TF state derived from them. Eviction is deferred to here, not the tombstone site, because a mid-load
    // failure rolls the tombstones back.
    for (const DatasetId tombstoned_id : tombstoned_for_replace) {
      session_.evictDatasetObjects(tombstoned_id);
      if (transform_service_ != nullptr) {
        transform_service_->invalidateDataset(tombstoned_id);
      }
    }
  }
  tombstoned_for_replace.clear();

  catalog_.rebuildFromDatastore();  // T6 (replace path): same keys ⇒ no spurious itemsRemoved

  // Per pj_scene3D REQUIREMENTS §9: TF buffer is per-dataset, populated
  // eagerly at MCAP load time. Synchronous so drag-dropping a 3D topic
  // is instant — the cost lives in the (expected-to-be-slow) load path,
  // not in interaction. Scene3DDockWidget borrows the shared buffer from
  // the same TransformService. When no service is wired (non-3D builds)
  // TF ingest is simply skipped.
  if (transform_service_ != nullptr) {
    if (swapped_in_place) {
      // The primary id survived the swap but its object topics now hold the
      // reloaded data. Ingest is idempotent per dataset, so without the
      // invalidation it would skip and 3D views would keep the previous
      // load's transforms.
      transform_service_->invalidateDataset(existing_primary_id);
      transform_service_->ingestFrameTransformsForDataset(existing_primary_id);
    } else if (fanouts.size() == 1) {
      transform_service_->ingestFrameTransformsForDataset(dataset_id);
    } else {
      for (const DatasetId loaded_id : fanout_loaded_ids) {
        transform_service_->ingestFrameTransformsForDataset(loaded_id);
      }
    }
  }

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
