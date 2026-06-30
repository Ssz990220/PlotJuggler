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
#include <QThread>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DialogPresenter.h"
#include "FanoutConfig.h"
#include "LayoutXml.h"
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

// Default ingest policies the app applies to every DataSourceRuntimeHost it
// builds: scalars eager, objects lazy (decoded on pull). The heaviest and/or
// scalar-less payloads are PURE-LAZY — their bytes are re-fetched from the
// source on every read instead of pinned in RAM at ingest:
//   * Point clouds / compressed point clouds: huge per-frame; pure-lazy also
//     defers the Draco/Cloudini transcode to the render path.
//   * Video frames: keeps each file-backed bitstream NON-resident.
//   * Images / depth images: a raw (uncompressed) Image frame is hundreds of KB
//     and a recording holds thousands; retaining them all dominated peak RSS
//     (~4 GB on the quadruped dataset). The 2D image/depth docks pull via
//     ObjectStore::latestAt, which resolves the deferred fetch transparently —
//     exactly how point clouds already render lazily.
//   * Occupancy grids / voxel grids: a metric map or dense 3D grid is large per
//     frame; the scene docks pull them via ObjectStore::latestAt, so pure-lazy
//     keeps them non-resident like point clouds.
//   * SceneEntities (markers) / ImageAnnotations: carry no scalar fields, so an
//     eager-scalar parse would fail; pure-lazy is the only correct mode.
// TF (kFrameTransforms) intentionally stays eager: its payload is tiny and its
// scalar fields are useful. Static so the pre-dialog scratch session and the
// per-fanout loop iterations stay in lockstep, and so it is unit-testable
// against a bare resolver.
void FileLoader::applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver) {
  using PJ::sdk::BuiltinObjectType;
  using PJ::sdk::ObjectIngestPolicy;
  resolver.setDefault(ObjectIngestPolicy::kLazyObjectsEagerScalars);
  resolver.setForType(BuiltinObjectType::kPointCloud, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kCompressedPointCloud, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kImage, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kDepthImage, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kOccupancyGrid, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kVoxelGrid, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kSceneEntities, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kImageAnnotations, ObjectIngestPolicy::kPureLazy);
  resolver.setForType(BuiltinObjectType::kVideoFrame, ObjectIngestPolicy::kPureLazy);
}

// Per-load state for a single-instance worker load. Holds the bound plugin
// handle + ingest host so they outlive the GUI prologue across the worker run.
struct FileLoader::LoadContext {
  // DataSourceHandle has no default ctor (it wraps a created plugin instance),
  // so the context is built by moving the bound handle + host in.
  LoadContext(DataSourceHandle bound_handle, std::unique_ptr<DataSourceRuntimeHost> bound_ingest)
      : handle(std::move(bound_handle)), ingest(std::move(bound_ingest)) {}

  QString path;
  QPointer<QWidget> dialog_parent;
  QString source_name;
  std::string config;
  DatasetId dataset_id = 0;
  bool replacing = false;
  // Engaged only on a REPLACING reload: the RAII transaction that detached the
  // prior data up front. Committed on success/keep (onWorkerFinished); otherwise
  // its destructor — fired by ctx_.reset()/destruction — rolls the dataset back.
  std::optional<RefillGuard> refill_guard;
  int file_index = 1;
  int file_total = 1;
  DataSourceHandle handle;
  std::unique_ptr<DataSourceRuntimeHost> ingest;
  // Worker-owned: started on the GUI before the thread runs, then only the
  // worker touches it (elapsed/restart) to pace flush+notify.
  QElapsedTimer flush_clock;
  // Set once by on_progress_start (worker); read by the queued progress lambda.
  uint64_t progress_total = 0;
  // Worker -> GUI handoff (read in onWorkerFinished after the worker joins).
  bool start_ok = false;
  QString start_error;
};

FileLoader::FileLoader(
    SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent)
    : QObject(parent), session_(session), extensions_(extensions), catalog_(catalog) {}

FileLoader::~FileLoader() {
  joinForShutdown();
}

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
  const QStringList paths = file_picker_ != nullptr
                                ? file_picker_(dialog_parent, tr("Load Data"), last_dir, filter)
                                : FileDialog::getOpenFileNames(dialog_parent, tr("Load Data"), last_dir, filter);
  if (paths.isEmpty()) {
    return;
  }
  // Remember the directory of the last pick for next time.
  settings.setValue(kLastDirKey, QFileInfo(paths.last()).absolutePath());
  // Load each selected file in order so several datasets populate in one go.
  // Each loadFile may pop its own data-source config dialog and reports its own
  // failures, so a single bad file doesn't abort the rest.
  for (const QString& path : paths) {
    loadFile(path, dialog_parent);
  }
}

bool FileLoader::beginLoad(const LoadRequest& request) {
  const QString& path = request.path;
  QWidget* const dialog_parent = request.dialog_parent.data();
  const LoadHints& hints = request.hints;

  // Restore same-source datasets if a replacement load is cancelled or fails.
  std::vector<DatasetId> tombstoned_for_replace;
  DatasetId created_live_dataset_id = 0;
  const auto rollback_tombstones = [&]() {
    for (const DatasetId id : tombstoned_for_replace) {
      catalog_.restoreDataset(id);
    }
    tombstoned_for_replace.clear();
  };
  const auto erase_created_live_dataset = [&]() {
    if (created_live_dataset_id == 0) {
      return;
    }
    // Non-replacing loads create directly in the live engine. If they fail
    // before commit, drop any emitted catalog items before erasing the engine
    // dataset so no reader/adapter can keep a dangling TopicStorage pointer.
    session_.evictDatasetObjects(created_live_dataset_id);
    catalog_.removeDataset(created_live_dataset_id, /*tombstone=*/false);
    session_.dataEngine().removeDataset(created_live_dataset_id);
    created_live_dataset_id = 0;
  };

  // One unified failure path — log, optionally pop a dialog, emit signal.
  const auto fail = [&](const QString& reason) -> bool {
    erase_created_live_dataset();
    rollback_tombstones();
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

  const QString display_name = QFileInfo(path).fileName();
  const std::string display_name_utf8 = display_name.toStdString();

  // One TimeDomain per loaded source so each is independently time-shiftable
  // (the Source Timeline drives the per-domain display_offset). The staged
  // replace path below mints its own; the live first-load uses this one.
  auto td_or = engine.createTimeDomain(display_name_utf8);
  if (!td_or.has_value()) {
    return fail(tr("Could not create the time domain for %1.").arg(display_name));
  }
  const TimeDomainId td_id = *td_or;

  // Same-source handling: layout replay reuses the existing DatasetId; an interactive load/reload replaces the
  // dataset's data in place, keeping its DatasetId/TopicIds (and so all curve keys) stable. The engine names
  // datasets by basename, so the basename match is only a pre-filter — reuse is gated on full-path identity
  // below, so two different files that share a basename (e.g. log.mcap in separate run dirs) stay distinct
  // datasets instead of the second silently aliasing the first.
  DatasetId existing_primary_id = 0;
  for (const auto existing_id : engine.listDatasets()) {
    const DatasetInfo* info = engine.getDataset(existing_id);
    if (info == nullptr || info->source_name != display_name_utf8) {
      continue;
    }
    // Basename matches; require the same file on disk too. A dataset with no
    // recorded path (created outside FileLoader, e.g. streaming/test data)
    // keeps the legacy basename-only behavior.
    if (const auto path_it = dataset_source_path_.find(existing_id);
        path_it != dataset_source_path_.end() && !layout_xml::isSamePath(path_it->second, path)) {
      continue;
    }
    if (hints.prefer_reuse) {
      // Reuse the id referenced by the layout; keep the recorded config
      // when legacy XML has no preset. Match the recorded source by path
      // (not just the most-recent one) so a multi-file session recovers the
      // right file's config when reloading any of its sources.
      QString emit_config = hints.preset_config_json;
      if (emit_config.isEmpty()) {
        const auto& prior = session_.loadedSources();
        const auto it = std::find_if(prior.begin(), prior.end(), [&path](const auto& src) { return src.path == path; });
        if (it != prior.end()) {
          emit_config = it->plugin_config_json;
        }
      }
      catalog_.restoreDataset(existing_id);
      dataset_source_path_[existing_id] = path;
      emit fileLoaded(path, QString(), source_name, emit_config);
      return false;  // layout-replay reuse: done synchronously, no worker
    }
    // Tombstone is deferred to the post-ingest swap (single-instance) or the fanout fallback below: don't disturb
    // the live dataset until the staged ingest has succeeded.
    existing_primary_id = existing_id;
    break;
  }

  // Ingest target — always the LIVE engine/store (no staging engine). A first
  // load creates a fresh dataset. A same-source single-instance reload binds to
  // the EXISTING dataset and refills it in place under the transactional
  // RefillGuard from beginRefill() (below), so its DatasetId/TopicIds — and every
  // curve key — stay stable: the write host's ensureTopic reuses each topic by
  // name, writing back into the original ids. (A fanout reload can't refill one
  // dataset into N, so it falls back to remove-then-fresh-load; see the fanout branch.)
  const bool replacing = (existing_primary_id != 0);
  DatasetId dataset_id = existing_primary_id;
  if (!replacing) {
    auto dataset_or =
        engine.createDataset(DatasetDescriptor{.source_name = display_name_utf8, .time_domain_id = td_id});
    if (!dataset_or.has_value()) {
      return fail(tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error())));
    }
    dataset_id = static_cast<DatasetId>(*dataset_or);
  }
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(dataset_id)};

  // A non-replacing first load just created its dataset in the LIVE engine
  // (above). Track it so a SYNCHRONOUS prologue failure (bind / loadConfig below,
  // before the worker starts) erases the abandoned shell via fail()'s
  // erase_created_live_dataset, instead of leaving an empty dataset a prefer_reuse
  // layout replay could reattach to. Worker-thread failure / discard is handled
  // symmetrically in onWorkerFinished (which knows the same dataset via !replacing).
  if (!replacing) {
    created_live_dataset_id = dataset_id;
  }

  // Parsers register directly under the live ids (no staged remap needed).
  DataSourceRuntimeHost::ObjectTopicParserRegistrar object_parser_registrar =
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_.registerObjectTopicParser(id, std::move(parser));
      };
  // Heap-owned so a single-instance load can MOVE it into ctx_ and let the
  // worker thread run handle.start() against it after this prologue returns.
  // The reference stays valid across that move (unique_ptr move transfers
  // ownership; the object's address does not change).
  auto ingest_session_ptr = std::make_unique<DataSourceRuntimeHost>(
      engine, extensions_, dataset_id, source_handle, session_.objectStore(), source->id,
      std::move(object_parser_registrar), nullptr, nullptr, handle.libraryOwner());
  DataSourceRuntimeHost& ingest_session = *ingest_session_ptr;
  if (dialog_parent != nullptr) {
    ingest_session.setMessageBoxHandler(
        [dialog_parent](int type, std::string_view title, std::string_view message, int buttons) -> int {
          const QString q_title = QString::fromUtf8(title.data(), static_cast<int>(title.size()));
          const QString q_text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));

          // The C-ABI contract (data_source_protocol.h: show_message_box is tagged
          // [main-thread]) promises the host marshals this to the GUI thread. On the
          // single-instance load path importData() runs on a worker QThread, so building or
          // exec'ing the QMessageBox directly here would touch a GUI-thread-owned parent
          // off-thread -- a Qt thread-affinity violation that segfaults in the font engine
          // while painting. Build/run the dialog on the GUI thread and block the worker until
          // the user closes the modal (the documented blocking semantics). On the fanout path
          // we are already on the GUI thread, so call directly to avoid a self-deadlock.
          auto show = [&]() -> int {
            QMessageBox msg_box(dialog_parent);
            msg_box.setWindowTitle(q_title);
            msg_box.setText(q_text);
            switch (type) {
              case PJ_MESSAGE_BOX_WARNING:
                msg_box.setIcon(QMessageBox::Warning);
                break;
              case PJ_MESSAGE_BOX_ERROR:
                msg_box.setIcon(QMessageBox::Critical);
                break;
              case PJ_MESSAGE_BOX_QUESTION:
                msg_box.setIcon(QMessageBox::Question);
                break;
              default:
                msg_box.setIcon(QMessageBox::Information);
                break;
            }
            QPushButton* btn_ok = (buttons & PJ_MSG_BTN_OK) ? msg_box.addButton(QMessageBox::Ok) : nullptr;
            QPushButton* btn_cancel = (buttons & PJ_MSG_BTN_CANCEL) ? msg_box.addButton(QMessageBox::Cancel) : nullptr;
            QPushButton* btn_yes = (buttons & PJ_MSG_BTN_YES) ? msg_box.addButton(QMessageBox::Yes) : nullptr;
            QPushButton* btn_no = (buttons & PJ_MSG_BTN_NO) ? msg_box.addButton(QMessageBox::No) : nullptr;
            QPushButton* btn_continue = (buttons & PJ_MSG_BTN_CONTINUE)
                                            ? msg_box.addButton(QObject::tr("Continue"), QMessageBox::AcceptRole)
                                            : nullptr;
            QPushButton* btn_abort = (buttons & PJ_MSG_BTN_ABORT)
                                         ? msg_box.addButton(QObject::tr("Abort"), QMessageBox::RejectRole)
                                         : nullptr;

            msg_box.exec();
            const auto* clicked = msg_box.clickedButton();
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
          };

          if (QThread::currentThread() == qApp->thread()) {
            return show();  // already on the GUI thread (fanout path)
          }
          int result = -1;
          QMetaObject::invokeMethod(qApp, [&]() { result = show(); }, Qt::BlockingQueuedConnection);
          return result;
        });
  }
  applyDefaultIngestPolicies(ingest_session.policyResolver());

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
      rollback_tombstones();
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
  enum class CancelAction { kNone, kKeep, kDiscard };
  CancelAction user_action = CancelAction::kNone;

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
  auto wire_progress = [&](DataSourceRuntimeHost& session) {
    session.on_progress_start = [&](std::string_view label, uint64_t total, bool cancellable) {
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
    session.on_progress_update = [&](uint64_t current) -> bool {
      progress_dlg.setValue(static_cast<int>(current));
      QCoreApplication::processEvents();
      switch (progress_dlg.action()) {
        case ProgressDialog::Action::kPrimary:
          user_action = CancelAction::kKeep;
          break;
        case ProgressDialog::Action::kSecondary:
          user_action = CancelAction::kDiscard;
          break;
        case ProgressDialog::Action::kNone:
          return true;
      }
      session.requestStop(
          user_action == CancelAction::kKeep ? "cancelled by user (keep partial)" : "cancelled by user (discard)");
      return false;
    };
    session.on_progress_finish = [&]() {
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
    // --- Single-instance load: run the read loop on a worker thread, filling
    // the datastore progressively while the GUI stays interactive. The modal
    // ProgressDialog above is unused here (destroyed when this prologue returns);
    // progress + cancellation flow through ingestStarted/ingestProgress +
    // cancelCurrent (the title-bar IngestProgressWidget), wired by MainWindow. ---
    // issue #98: apply the plugin's dataset name before start() so the
    // commit-driven catalog rebuild surfaces curves under the right tree-root.
    if (const QString plugin_name = detail::parseDisplayName(config); !plugin_name.isEmpty()) {
      catalog_.setDatasetDisplayName(dataset_id, plugin_name);
    }

    // Move the bound handle + host into the per-load context so they outlive
    // this prologue into the worker. The `ingest_session` reference stays valid
    // (the move transfers ownership without relocating the heap object).
    ctx_ = std::make_unique<LoadContext>(std::move(handle), std::move(ingest_session_ptr));
    ctx_->path = path;
    ctx_->dialog_parent = request.dialog_parent;
    ctx_->source_name = source_name;
    ctx_->config = config;
    ctx_->dataset_id = dataset_id;
    ctx_->replacing = replacing;
    ctx_->file_index = request.file_index;
    ctx_->file_total = request.file_total;

    if (replacing) {
      // Transactional in-place refill: DETACH (move aside, not free) the existing
      // dataset's prior data, keeping its topics registered so the refill's
      // ensureTopic rebinds each by name into the same ids. Constructed as the LAST
      // prologue step — after every synchronous fail()-return above and after ctx_
      // exists — so a synchronous prologue failure never touches the live data, and
      // a worker failure / discard / shutdown rolls back via the guard (the prior
      // data lives in the snapshot until commit()). Replaces the old up-front
      // in-place clear, which destroyed the prior data with no rollback.
      ctx_->refill_guard = session_.beginRefill(existing_primary_id);
    }

    // Worker-driven progress. on_progress_* run on the WORKER; they touch only
    // ctx_ (set before the thread starts, not mutated by the GUI until join) and
    // the atomic cancel flag, and marshal every GUI access via invokeMethod.
    DataSourceRuntimeHost& host = *ctx_->ingest;
    host.on_progress_start = [this](std::string_view label, uint64_t total, bool /*cancellable*/) {
      ctx_->progress_total = total;
      const QString title = QString::fromUtf8(label.data(), static_cast<int>(label.size()));
      const bool determinate = total > 0;
      const int file_index = ctx_->file_index;
      const int file_total = ctx_->file_total;
      QMetaObject::invokeMethod(
          this,
          [this, title, determinate, file_index, file_total]() {
            emit ingestStarted(title, file_index, file_total, determinate);
          },
          Qt::QueuedConnection);
    };
    host.on_progress_update = [this](uint64_t current) -> bool {
      if (cancel_mode_.load() != 0) {
        return false;  // user asked to stop; the read loop exits cooperatively
      }
      if (ctx_->flush_clock.elapsed() < flush_throttle_ms_) {
        return true;
      }
      ctx_->ingest->flushPending();  // worker-side: seal+commit -> rows visible
      ctx_->flush_clock.restart();
      const DatasetId notify_dataset = ctx_->dataset_id;
      const int cur = static_cast<int>(current);
      const int max = static_cast<int>(ctx_->progress_total);
      QMetaObject::invokeMethod(
          this,
          [this, notify_dataset, cur, max]() {
            // GUI: recompute the topic-id set here (never capture it from the
            // worker — the parser registrar may add topics concurrently).
            const auto ids = session_.dataEngine().listTopics(notify_dataset);
            session_.notifyIngest(QVector<TopicId>(ids.begin(), ids.end()), /*live=*/false);
            // Fold the FrameTransforms loaded so far into the TF buffer incrementally
            // (cursor-based — each call ingests only what is new). Otherwise TF is
            // ingested in a single pass at completion (finishLoadOnGui) and 3D scenes
            // stay empty until the load finishes. This emits datasetTransformsReady,
            // which 3D docks observe to re-render at the playhead as the file loads.
            if (transform_service_ != nullptr) {
              transform_service_->ingestFrameTransformsForDataset(notify_dataset);
            }
            emit ingestProgress(cur, max);
          },
          Qt::QueuedConnection);
      return true;
    };
    host.on_progress_finish = [] {};  // terminal flush is done in onWorkerFinished

    cancel_mode_.store(0);
    ctx_->flush_clock.start();
    worker_ = std::unique_ptr<QThread>(QThread::create([this]() { runIngestOnWorker(); }));
    worker_->start();
    return true;  // worker running; onWorkerFinished resumes the queue
  } else {
    // A same-source reload that fans out cannot refill in place (one source becomes N datasets). Fall back to
    // remove-then-fresh: tombstone the existing dataset now (objects evicted past the rollback point below) and let
    // the fanout create fresh datasets on the live engine. The handle bound to existing_primary_id above is never
    // start()ed here (fanout mints its own per-entry handles), so the dataset takes no data before its removal.
    if (replacing && catalog_.removeDataset(existing_primary_id)) {
      tombstoned_for_replace.push_back(existing_primary_id);
    }
    // Multi-instance fanout. On a FRESH load the dataset created above for the bind is now an empty orphan;
    // pj_datastore has no removeDataset, but an empty dataset has no committed topics so
    // CatalogModel::rebuildFromDatastore skips it (no phantom entry). Each fanout entry mints its own handle +
    // dataset + ingest_session. Continue-on-error per the user-confirmed policy: a bad entry does not lose the others.
    // Outcomes per fanout entry. Kept keeps the entry's partial flush
    // ("Cancel" — stop here but keep what was already parsed); Discarded
    // throws it away. Both stop the outer loop.
    enum class EntryOutcome { kCompleted, kFailed, kKept, kDiscarded };

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
    auto run_fanout_entry = [&](std::size_t idx, const std::string& cfg_i,
                                const QString& iter_display) -> EntryOutcome {
      // Each fanned dataset gets its own TimeDomain so it is independently
      // draggable on the Source Timeline (rather than sharing the primary's).
      auto iter_td = engine.createTimeDomain(iter_display.toStdString());
      if (!iter_td.has_value()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: createTimeDomain failed:" << QString::fromStdString(iter_td.error());
        return EntryOutcome::kFailed;
      }
      auto iter_dataset_or = engine.createDataset(
          DatasetDescriptor{.source_name = iter_display.toStdString(), .time_domain_id = *iter_td});
      if (!iter_dataset_or.has_value()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: createDataset failed:" << QString::fromStdString(iter_dataset_or.error());
        return EntryOutcome::kFailed;
      }
      const auto iter_dataset_id = static_cast<DatasetId>(*iter_dataset_or);
      const PJ_data_source_handle_t iter_source_handle{static_cast<uint32_t>(iter_dataset_id)};

      DataSourceHandle iter_handle = source->library.createHandle();
      if (!iter_handle.valid()) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx << "]: createHandle failed";
        return EntryOutcome::kFailed;
      }

      DataSourceRuntimeHost iter_ingest(
          engine, extensions_, iter_dataset_id, iter_source_handle, session_.objectStore(), source->id,
          [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
            session_.registerObjectTopicParser(id, std::move(parser));
          },
          nullptr, nullptr, iter_handle.libraryOwner());
      applyDefaultIngestPolicies(iter_ingest.policyResolver());

      ServiceRegistryBuilder iter_registry;
      iter_ingest.registerServices(iter_registry);

      if (auto status = iter_handle.bind(iter_registry.view()); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: bind failed:" << QString::fromStdString(status.error());
        return EntryOutcome::kFailed;
      }
      if (auto status = iter_handle.loadConfig(cfg_i); !status) {
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: loadConfig failed:" << QString::fromStdString(status.error());
        return EntryOutcome::kFailed;
      }

      wire_progress(iter_ingest);
      progress_dlg.setMessage(tr("Importing %1 (%2/%3)").arg(iter_display).arg(idx + 1).arg(fanouts.size()));

      if (auto status = iter_handle.start(); !status) {
        progress_dlg.hide();
        qCWarning(lcFileLoader) << "[FileLoader] fanout[" << idx
                                << "]: start failed:" << QString::fromStdString(status.error());
        return EntryOutcome::kFailed;
      }
      // Discard = drop this entry entirely. Skip flushAll() so its buffered
      // scalar rows never become visible, then evict the immediately-written
      // ObjectStore payloads and drop the dataset — no half-written remnant.
      // user_action can only have flipped during this entry's import, since
      // the loop breaks on Keep/Discard. Entries that already Completed stay
      // loaded.
      if (user_action == CancelAction::kDiscard) {
        session_.evictDatasetObjects(iter_dataset_id);
        catalog_.removeDataset(iter_dataset_id);
        return EntryOutcome::kDiscarded;
      }
      // Cancel(Keep) = keep what was already parsed for this entry
      // (flushAll), then stop the outer loop so subsequent entries are
      // skipped.
      iter_ingest.flushAll();
      fanout_loaded_ids.push_back(iter_dataset_id);
      if (user_action == CancelAction::kKeep) {
        return EntryOutcome::kKept;
      }
      return EntryOutcome::kCompleted;
    };

    for (std::size_t i = 0; i < fanouts.size(); ++i) {
      const std::string& cfg_i = fanouts[i];
      const QString suffix = detail::parseDisplaySuffix(cfg_i, QString::number(i + 1));
      const QString iter_display = base + QChar('/') + suffix;

      switch (run_fanout_entry(i, cfg_i, iter_display)) {
        case EntryOutcome::kCompleted:
          ++completed;
          break;
        case EntryOutcome::kFailed:
          ++failed;
          failed_labels << iter_display;
          break;
        case EntryOutcome::kKept:
          // Cancel: this entry kept its partial flush; the remaining entries
          // are skipped.
          ++completed;
          stopped = true;
          break;
        case EntryOutcome::kDiscarded:
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
    // Summary so a partial fanout import is diagnosable from the log alone.
    qCInfo(lcFileLoader) << "[FileLoader] fanout complete:" << completed << "ok," << failed << "failed"
                         << (failed > 0 ? failed_labels : QStringList{});
  }

  // Past the last rollback point: the load committed in place (no staging swap).
  // Free the ObjectStore topics, derived TF state, AND scalar engine storage of any
  // datasets the fanout-reload fallback tombstoned. A fanout reload can't refill one
  // dataset into N, so the old dataset is replaced by fresh ones and must be ERASED
  // from the engine — not left as a shell a prefer_reuse layout replay could reattach
  // to (#249's real-delete, fanout face). Deferred to here (not the tombstone site)
  // because a mid-load failure rolls the tombstones back.
  for (const DatasetId tombstoned_id : tombstoned_for_replace) {
    // Invalidate BEFORE eviction: invalidateDataset's cursor cleanup walks
    // listTopics(tombstoned_id), so the topics must still resolve. Evicting
    // first would empty that list and leak every per-topic cursor key.
    if (transform_service_ != nullptr) {
      transform_service_->invalidateDataset(tombstoned_id);
    }
    session_.evictDatasetObjects(tombstoned_id);
    engine.removeDataset(tombstoned_id);
  }
  tombstoned_for_replace.clear();

  catalog_.rebuildFromDatastore();  // reload: same keys ⇒ no spurious itemsRemoved

  // Per pj_scene3D REQUIREMENTS §9: TF buffer is per-dataset, populated eagerly
  // at load time. A single-instance reload changed its data in place, so
  // invalidate before re-ingesting (ingest is idempotent per dataset and would
  // otherwise skip). No service wired (non-3D builds) -> skipped.
  if (transform_service_ != nullptr) {
    if (fanouts.size() == 1) {
      if (replacing) {
        transform_service_->invalidateDataset(dataset_id);
      }
      transform_service_->ingestFrameTransformsForDataset(dataset_id);
    } else {
      for (const DatasetId loaded_id : fanout_loaded_ids) {
        transform_service_->ingestFrameTransformsForDataset(loaded_id);
      }
    }
  }

  // Capture the plugin's canonical post-load state AFTER start() + ingest so a
  // layout persists discovered fields / applied defaults / ingest-time policy
  // overrides. saveConfig failures here are non-fatal.
  std::string captured_config;
  if (auto status = handle.saveConfig(captured_config); !status) {
    qCWarning(lcFileLoader).noquote() << tr("Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                             .arg(source_name, QString::fromStdString(status.error()));
    captured_config.clear();
  }

  // Remember which file each dataset came from so a later load of a DIFFERENT
  // file sharing this basename is not mistaken for a reload of it (the match
  // loop above gates reuse on this). Single-instance: `dataset_id` is the stable
  // id (existing on reload, else the fresh one). Fanout: each dataset that took
  // data.
  if (fanouts.size() == 1) {
    dataset_source_path_[dataset_id] = path;
  } else {
    for (const DatasetId loaded_id : fanout_loaded_ids) {
      dataset_source_path_[loaded_id] = path;
    }
  }

  emit fileLoaded(path, QString(), source_name, QString::fromStdString(captured_config));
  return false;  // fanout ran synchronously to completion; no worker — process the next request
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints) {
  queue_.push_back(LoadRequest{.path = path, .dialog_parent = dialog_parent, .hints = hints});
  startNext();
  return true;  // accepted/enqueued — completion is async (fileLoaded/fileLoadFailed)
}

void FileLoader::startNext() {
  if (active_load_) {
    return;  // a prologue or worker load is in progress; it resumes the queue when done
  }
  while (!queue_.empty()) {
    const LoadRequest request = queue_.front();
    queue_.pop_front();
    active_load_ = true;  // guards re-entrancy, incl. while the modal dialog pumps events
    if (beginLoad(request)) {
      return;  // single-instance worker running; onWorkerFinished resumes the queue
    }
    active_load_ = false;  // fanout / reuse / fail / reject completed synchronously
  }
  emit queueDrained();
}

void FileLoader::runIngestOnWorker() {
  // WORKER thread. ctx_ is set on the GUI thread before this thread starts and
  // is not mutated by the GUI until after we post onWorkerFinished, so reading
  // it here races nothing. start() blocks until the plugin finishes (or stops
  // cooperatively when on_progress_update returns false).
  auto status = ctx_->handle.start();
  ctx_->start_ok = static_cast<bool>(status);
  if (!status) {
    ctx_->start_error = QString::fromStdString(status.error());
  }
  QMetaObject::invokeMethod(this, [this]() { onWorkerFinished(); }, Qt::QueuedConnection);
}

void FileLoader::onWorkerFinished() {
  if (!ctx_) {
    return;  // joinForShutdown already tore the load down
  }
  if (worker_) {
    worker_->wait();  // the worker posted us as its last act, so this returns promptly
    worker_.reset();
  }

  const int cancel = cancel_mode_.load();
  const DatasetId dataset_id = ctx_->dataset_id;
  const bool replacing = ctx_->replacing;
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;

  if (cancel == 2) {  // Discard
    qCWarning(lcFileLoader) << "[FileLoader] import discarded by user; partial data dropped";
    if (!replacing) {
      // Real-delete the abandoned first-load shell: evict its objects, drop catalog
      // items WITHOUT a tombstone, and erase the engine's scalar storage, so a later
      // prefer_reuse layout replay mints a fresh dataset instead of reattaching to an
      // empty one. The eviction MUST stay on this !replacing arm — on the replacing
      // path it would wipe the objects the guard is about to restore.
      session_.evictDatasetObjects(dataset_id);
      catalog_.removeDataset(dataset_id, /*tombstone=*/false);
      session_.dataEngine().removeDataset(dataset_id);
    } else {
      // Roll back to the pre-reload data (guard dtor reattaches scalar + object data,
      // retires/removes topics the failed refill added, evicts their parsers).
      ctx_->refill_guard.reset();
      refreshAfterReplacingRollback(dataset_id);
    }
    ctx_.reset();
    emit fileLoadFailed(path, tr("Import discarded"));
  } else if (!ctx_->start_ok && cancel == 0) {  // start() failed (and not a user stop)
    const QString reason = tr("Plugin '%1': start failed: %2").arg(source_name, ctx_->start_error);
    qCWarning(lcFileLoader).noquote() << reason;
    if (!replacing) {
      // Real-delete the abandoned first-load shell (no tombstone) + erase its engine
      // storage, so a later prefer_reuse layout replay re-ingests instead of
      // reattaching to the empty dataset a failed start() left behind.
      session_.evictDatasetObjects(dataset_id);
      catalog_.removeDataset(dataset_id, /*tombstone=*/false);
      session_.dataEngine().removeDataset(dataset_id);
    } else {
      // Same rollback as Discard: a failed start() on a reload must restore the prior
      // data rather than leave the dataset empty.
      ctx_->refill_guard.reset();
      refreshAfterReplacingRollback(dataset_id);
    }
    ctx_.reset();
    emit fileLoadFailed(path, reason);
  } else {  // Completed, or Cancel(keep): make the parsed rows visible and finalize
    if (cancel == 1) {
      qCInfo(lcFileLoader) << "[FileLoader] import cancelled by user; keeping the partial load";
    }
    ctx_->ingest->flushAll();
    if (ctx_->refill_guard) {
      // On a COMPLETE reload (cancel == 0), retire prior topics the new file no
      // longer has — they stayed empty through the refill (codex #2). Skipped on
      // Cancel-Keep: a topic the partial load never reached is not "vanished". Must
      // run BEFORE commit(), which frees the prior-topic snapshots it reads.
      if (cancel == 0) {
        ctx_->refill_guard->pruneVanishedTopics();
      }
      // COMMIT the refill: both Completed and Cancel-Keep keep the refilled data, so
      // free the prior-data snapshot. Must run BEFORE finishLoadOnGui() — it resets ctx_
      // (destroying the guard), and an uncommitted guard would then silently roll back
      // over the new data.
      ctx_->refill_guard->commit();
    }
    finishLoadOnGui();  // emits fileLoaded; resets ctx_
  }

  cancel_mode_.store(0);
  active_load_ = false;
  startNext();
}

void FileLoader::refreshAfterReplacingRollback(DatasetId dataset_id) {
  // The RefillGuard already restored the scalar + object data and re-notified plot
  // adapters; reflect the restored topic set in the catalog tree and rebuild the
  // per-dataset TF buffer from the restored objects (mirrors finishLoadOnGui's
  // replacing-path TF handling, but on the rolled-back data).
  catalog_.rebuildFromDatastore();
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
    transform_service_->ingestFrameTransformsForDataset(dataset_id);
  }
}

void FileLoader::finishLoadOnGui() {
  const DatasetId dataset_id = ctx_->dataset_id;

  catalog_.rebuildFromDatastore();

  // Per pj_scene3D REQUIREMENTS §9: TF buffer is per-dataset, populated at load
  // time. A reload changed its data in place, so invalidate before re-ingesting
  // (ingest is idempotent per dataset and would otherwise skip).
  if (transform_service_ != nullptr) {
    if (ctx_->replacing) {
      transform_service_->invalidateDataset(dataset_id);
    }
    transform_service_->ingestFrameTransformsForDataset(dataset_id);
  }

  // Capture the plugin's canonical post-load state for layout persistence.
  std::string captured_config;
  if (auto status = ctx_->handle.saveConfig(captured_config); !status) {
    qCWarning(lcFileLoader).noquote() << tr("Plugin '%1': saveConfig failed: %2 — layout save will skip plugin config")
                                             .arg(ctx_->source_name, QString::fromStdString(status.error()));
    captured_config.clear();
  }

  dataset_source_path_[dataset_id] = ctx_->path;
  const QString path = ctx_->path;
  const QString source_name = ctx_->source_name;
  ctx_.reset();  // drop the handle/host before notifying — the load is complete
  emit fileLoaded(path, QString(), source_name, QString::fromStdString(captured_config));
}

void FileLoader::cancelCurrent(bool keep_partial) {
  if (ctx_ == nullptr) {
    return;  // no worker load in progress (fanout cancellation flows through its modal dialog)
  }
  cancel_mode_.store(keep_partial ? 1 : 2);
  if (ctx_->ingest) {
    // Flag-only stop: cancelCurrent runs on the GUI thread while the worker may be in a
    // host callback, so writing a reason here would race the worker's last_error_. The
    // cooperative stop only needs the atomic flag; the reason is unused on this path.
    ctx_->ingest->requestStop();
  }
}

bool FileLoader::isBusy() const {
  return active_load_ || !queue_.empty();
}

DatasetId FileLoader::activeLoadDatasetId() const {
  return ctx_ != nullptr ? ctx_->dataset_id : 0;
}

void FileLoader::joinForShutdown() {
  cancel_mode_.store(2);  // discard whatever is mid-flight
  if (ctx_ != nullptr && ctx_->ingest) {
    ctx_->ingest->requestStop();  // flag-only: worker may be mid-ingest, avoid racing last_error_
  }
  if (worker_) {
    worker_->wait();
    worker_.reset();
  }
  // The worker has joined; the queued onWorkerFinished will no-op once ctx_ is reset, so
  // run the abandoned-load cleanup here. Capture the reload identity BEFORE ctx_.reset()
  // destroys the guard. A discarded mid-flight NON-replacing first load created a dataset
  // in the live engine — erase it (objects + catalog without a tombstone + engine storage)
  // so a later prefer_reuse layout replay re-ingests instead of reattaching to the empty
  // shell (matches onWorkerFinished's discard path). A REPLACING reload instead rolls back
  // via the guard's destructor when ctx_ is reset, restoring the pre-reload data.
  const bool was_replacing = (ctx_ != nullptr && ctx_->replacing);
  const DatasetId reload_id = (ctx_ != nullptr) ? ctx_->dataset_id : 0;
  if (ctx_ != nullptr && !was_replacing) {
    session_.evictDatasetObjects(reload_id);
    catalog_.removeDataset(reload_id, /*tombstone=*/false);
    session_.dataEngine().removeDataset(reload_id);
  }
  ctx_.reset();  // guard dtor rolls back a replacing reload; any queued onWorkerFinished no-ops
  if (was_replacing) {
    refreshAfterReplacingRollback(reload_id);  // reflect the restored data in catalog + TF
  }
  queue_.clear();
  active_load_ = false;
}

QString FileLoader::sourcePathForDataset(DatasetId dataset_id) const {
  const auto it = dataset_source_path_.find(dataset_id);
  return it != dataset_source_path_.end() ? it->second : QString();
}

void FileLoader::untrackDataset(DatasetId dataset_id) {
  dataset_source_path_.erase(dataset_id);
}

bool FileLoader::loadFile(const QString& path, QWidget* dialog_parent) {
  return loadFile(path, dialog_parent, LoadHints{});
}

}  // namespace PJ
