#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>

#include "pj_base/types.hpp"

QT_BEGIN_NAMESPACE
class QWidget;
class QThread;
QT_END_NAMESPACE

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ::sdk {
class ObjectIngestPolicyResolver;
}  // namespace PJ::sdk

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class SessionManager;

// Hints supplied by callers that already know what plugin to use and what
// config to apply (e.g. layout-driven reload). When skip_dialog is true and
// the layout's preset_config_json applies cleanly to the matching plugin,
// FileLoader::loadFile bypasses the data-source dialog entirely. On any
// failure (id mismatch, loadConfig rejection), the dialog falls back open
// with the existing QSettings-based pre-fill.
struct LoadHints {
  QString expected_plugin_id;  // Empty -> no hint; FileLoader picks plugin by extension as usual.
  QString preset_config_json;  // Empty -> no hint; QSettings pre-fill is used.
  bool skip_dialog = false;    // Only honored when both fields above are non-empty AND the plugin id matches.
  // Layout replay reuses matching DatasetIds; normal load/reload replaces them.
  bool prefer_reuse = false;
};

// Drives the file-import path: pick a file, find the matching DataSource
// plugin via ExtensionCatalogService, ingest into the SessionManager's data
// engine, and refresh the curve catalog. Lives in pj_app because it talks to
// QFileDialog/QMessageBox and to pj_marketplace's plugin handles.
class FileLoader : public QObject {
  Q_OBJECT
 public:
  FileLoader(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent = nullptr);
  ~FileLoader() override;

  FileLoader(const FileLoader&) = delete;
  FileLoader& operator=(const FileLoader&) = delete;

  // Opens a multi-select file dialog filtered by every installed file-import
  // plugin's extensions. On accept, runs loadFile() for EACH chosen path in
  // order (so you can populate several datasets in one go). The chosen directory
  // is persisted in QSettings under "FileLoader/lastDir".
  void openFromDialog(QWidget* dialog_parent);

  // Resolves the "pick file(s)" interaction inside openFromDialog(). Returns the
  // selected paths (empty on cancel). The shell injects one that threads
  // MainWindow's chrome metrics into PJ::FileDialog, keeping FileLoader free of a
  // MainWindow link (which also makes it testable headlessly). Unset -> plain
  // PJ::FileDialog::getOpenFileNames, no metrics.
  using FilePicker =
      std::function<QStringList(QWidget* parent, const QString& caption, const QString& dir, const QString& filter)>;
  void setFilePicker(FilePicker picker) {
    file_picker_ = std::move(picker);
  }

  // Programmatic entry point. ENQUEUES the load and returns immediately; the
  // bool now means "accepted/enqueued", NOT "loaded" — a single-instance load
  // runs progressively on a worker thread, so completion is asynchronous and
  // signalled by fileLoaded()/fileLoadFailed(). Loads run sequentially; a
  // second call while one is in flight queues behind it. (Fanout loads still run
  // synchronously to completion before this returns — a follow-up moves them to
  // the worker too.)
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);
  bool loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints);

  // Cancel the in-progress worker load. keep_partial=true keeps the rows parsed
  // so far (Primary/"Cancel"); false discards the dataset being filled
  // (Secondary/"Discard"). No-op when no worker load is running.
  void cancelCurrent(bool keep_partial);

  // True while a load is running (worker active or mid-prologue) or queued.
  [[nodiscard]] bool isBusy() const;

  // DatasetId of the single-instance load currently filling on the worker, or 0
  // when none. Lets the shell grow the playback range as that dataset fills.
  // GUI-thread only.
  [[nodiscard]] DatasetId activeLoadDatasetId() const;

  // Stop the worker (discard) and drain the queue. Call from MainWindow::closeEvent
  // BEFORE tearing down the session/datastore; also invoked by the destructor.
  void joinForShutdown();

  // 3D TF ingest is triggered at load time through this service (owned by the
  // app shell, not the domain-neutral runtime). When unset, TF ingest is
  // skipped — non-3D builds simply never set it.
  void setTransformService(pj::scene3d::TransformService* service) {
    transform_service_ = service;
  }

  // Normalized full filesystem path the given dataset was loaded from, or empty
  // if this loader did not create it (e.g. a streaming or test dataset, or an id
  // it has since forgotten). Reads through SessionManager's source-path registry
  // (the single owner of dataset->path identity). The shell uses this to
  // translate a DatasetId back to the loaded-source entry when a dataset is
  // removed.
  [[nodiscard]] QString sourcePathForDataset(DatasetId dataset_id) const;
  // Drop the session's dataset->path association after dataset removal. Safe to
  // call for unknown ids.
  void untrackDataset(DatasetId dataset_id);

  // Configure the object-ingest policy every load uses: scalars eager, objects
  // lazy-on-pull by default, and the heavy / scalar-less payloads (point clouds,
  // compressed point clouds, video frames, images, depth images, scene entities,
  // image annotations) PURE-LAZY so their bytes are re-fetched on read instead of
  // pinned in RAM at ingest. Static + resolver-typed so it is unit-testable
  // without standing up a full DataSourceRuntimeHost. TF stays eager on purpose:
  // its payload is tiny and its scalar fields are useful.
  static void applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver);

 signals:
  /// A same-source fan-out replacement is about to retire a DatasetId. The
  /// shell captures path-qualified workspace state before the old catalog item
  /// is hidden, then rebinds it after fileLoaded exposes the reminted datasets.
  void sourceReplacementAboutToCommit(const QString& path);

  void fileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json);
  void fileLoadFailed(const QString& path, const QString& reason);

  // Emitted when a load begins ingesting (after the modal dialog). `title` is the
  // filename; `file_index`/`file_total` are 1-based position among queued loads
  // (total<=1 => no N-of-M). `determinate` is false when the plugin reported no
  // step total (the bar should show busy/indeterminate).
  void ingestStarted(const QString& title, int file_index, int file_total, bool determinate);
  // Progress of the current load (maximum 0 => busy). Marshalled to the GUI thread.
  void ingestProgress(int current, int maximum);
  // The active load and the queue are both empty.
  void queueDrained();

 private:
  // One queued load request (a single loadFile call).
  struct LoadRequest {
    QString path;
    QPointer<QWidget> dialog_parent;
    LoadHints hints;
    int file_index = 1;
    int file_total = 1;
  };
  // Per-load state that must outlive the GUI prologue into the worker and back.
  // Defined in the .cpp (holds a DataSourceHandle + DataSourceRuntimeHost).
  struct LoadContext;

  // GUI: dequeue and begin the next load if idle. Re-entrant-safe (the modal
  // dialog pumps events). Emits queueDrained when the queue empties with no worker.
  void startNext();
  // GUI prologue for one request: resolve plugin, dialog, target dataset. For a
  // single-instance load it spins the worker and returns true ("a worker is
  // running"); for fanout / early-exit (reuse, fail, reject) it completes
  // synchronously and returns false ("process the next request").
  [[nodiscard]] bool beginLoad(const LoadRequest& request);
  // WORKER thread body: runs ctx_->handle.start(), driving throttled
  // flushPending()+queued notifyIngest() from on_progress_update.
  void runIngestOnWorker();
  // GUI (queued from the worker): join the worker, finalize or discard, then
  // startNext().
  void onWorkerFinished();
  // GUI: post-ingest reconciliation for the just-finished single-instance load
  // (catalog rebuild, TF ingest, saveConfig, source-path tracking, fileLoaded).
  void finishLoadOnGui();
  // GUI: after a replacing reload's RefillGuard has rolled the dataset back to its
  // pre-reload data (start-fail / discard / shutdown), reflect the restored data in
  // the catalog and rebuild the per-dataset TF buffer. The guard restores the data +
  // re-notifies adapters; this refreshes the catalog tree + scene TF on top.
  void refreshAfterReplacingRollback(DatasetId dataset_id);

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  FilePicker file_picker_;
  pj::scene3d::TransformService* transform_service_ = nullptr;

  // --- Sequential async load queue (single-instance loads run on a worker) ---
  std::deque<LoadRequest> queue_;
  std::unique_ptr<QThread> worker_;
  std::unique_ptr<LoadContext> ctx_;  // current single-instance load; null when idle
  // True while a load is being processed (prologue or worker). GUI-thread only:
  // guards startNext re-entrancy, including while the modal dialog pumps events.
  bool active_load_ = false;
  // Cancellation request for the current worker load: 0=none, 1=keep, 2=discard.
  // Written by cancelCurrent/joinForShutdown (GUI), read by the worker.
  std::atomic<int> cancel_mode_{0};
  // Minimum wall-clock between worker-side flush+notify cycles (test seam).
  int flush_throttle_ms_ = 50;
};

}  // namespace PJ
