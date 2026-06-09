#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <memory>
#include <unordered_map>

#include "pj_base/types.hpp"

QT_BEGIN_NAMESPACE
class QThread;
class QWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
class DataEngine;
class ExtensionCatalogService;
class ObjectStore;
class SessionManager;

// Drives the streaming-source path: pick a stream plugin from the catalog,
// build a per-session ingest host (mirrors FileLoader's flow), run the
// plugin's start() and then a worker thread that calls poll() + flush()
// at a fixed cadence until stopped. Multistream-capable by design: each Start
// creates an independent dataset/handle/worker triple keyed by DatasetId,
// even though the LeftPanel UX currently drives one active stream at a time.
//
// Lives in pj_app because it talks to dialog_presenter and owns Qt threads.
class StreamingSourceManager : public QObject {
  Q_OBJECT
 public:
  StreamingSourceManager(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QWidget* dialog_parent,
      QObject* parent = nullptr);
  ~StreamingSourceManager() override;

  StreamingSourceManager(const StreamingSourceManager&) = delete;
  StreamingSourceManager& operator=(const StreamingSourceManager&) = delete;

  // True iff any session is currently live.
  bool hasActiveSession() const;

 public slots:
  // Wired from LeftPanel::streamingStartRequested. Starts a session for the
  // plugin currently held in selected_plugin_. There is no UI-driven stop —
  // sessions live until app shutdown or an unrecoverable plugin error.
  void onStartRequested();

  // Wired from LeftPanel::streamingPauseToggled. Toggles follow-live for the
  // active session(s): when paused, the worker keeps polling and committing
  // samples to the datastore but the `live` flag carried by samplesIngested
  // is forced to false so PlotWidget stops auto-fitting the viewport to the
  // live edge. Mirrors PJ3 pause semantics.
  void onPauseToggled(bool paused);

  // Wired from LeftPanel::streamingSourceChanged. Remembers the currently
  // selected plugin name; consumed by the next onStartRequested().
  void onSourceChanged(const QString& plugin_id);

  // Wired from LeftPanel::streamingBufferChanged. Records the buffer size
  // (in seconds). v1 stores the value but does not yet apply it to topic
  // retention budgets — follow-up wiring.
  void onBufferChanged(int seconds);

 signals:
  // Emitted on the UI thread once a session is wired and the plugin's
  // start() has returned successfully.
  void streamStarted(DatasetId dataset_id);

  // Emitted on the UI thread after a worker has exited and the session has
  // been torn down. `reason` carries the cooperative-stop message ("user
  // stop", "plugin poll error: ...", etc.).
  void streamStopped(DatasetId dataset_id, QString reason);

  // Emitted on the UI thread for setup-time failures that happen before a
  // session is wired (plugin not found, createHandle / createDataset / time
  // domain creation failed, etc.). No DatasetId is available yet.
  void setupError(QString message);

  // Emitted on the UI thread for errors on an already-created session
  // (dialog rejection, bind failure, plugin start/poll error). The session
  // typically stops shortly after via streamStopped().
  void streamError(DatasetId dataset_id, QString message);

 private slots:
  // Joins the QThread, tears the session down, emits streamStopped().
  // Posted by the worker thread via QueuedConnection on loop exit.
  void onWorkerFinished(DatasetId dataset_id);

 private:
  struct StreamingSession;

  // Bootstraps one new session: resolves the plugin by display name, creates
  // handle/dataset/ingest, runs the dialog flow, calls start(), spawns the
  // worker. Emits streamStarted() on success, streamError() and tears down
  // on any failure path.
  void startSession(const QString& plugin_id);

  // Requests cooperative stop on every live session. Worker threads observe
  // the atomic on their next iteration and exit; final teardown happens in
  // onWorkerFinished().
  void requestStopAll(const QString& reason);

  // Worker thread body. Looks the session up by dataset_id (the map is only
  // mutated on the UI thread, so reads from the worker are safe as long as
  // its own entry exists).
  void workerLoop(DatasetId dataset_id);

  // Lazy-creates the default time domain on first use (mirrors FileLoader).
  TimeDomainId ensureDefaultTimeDomainId();

  // Drain every entry of every object topic in the secondary store back into
  // the primary (SessionManager::objectStore), then evict the secondary so
  // the next pause starts from an empty tail buffer. Called from
  // onPauseToggled(false) AFTER the target swap back to primary so no new
  // pushes are landing on the secondary during the drain.
  void flushSecondaryIntoPrimary();

  // Scalar twin of flushSecondaryIntoPrimary: drain every sealed chunk of
  // every topic in the secondary DataEngine back into the primary, then
  // clear the secondary's topic chunks. Called from onPauseToggled(false)
  // AFTER the target swap back to primary engine so no new chunks are
  // sealing on the secondary during the drain.
  void flushSecondaryDataEngineIntoPrimary();

  SessionManager& session_manager_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  QWidget* dialog_parent_;

  QString selected_plugin_;
  int retention_seconds_ = 5;
  TimeDomainId default_time_domain_id_ = 0;
  // Follow-live gate. Only mutated and read on the UI thread (the worker
  // reads it inside a QueuedConnection lambda that runs on the UI thread).
  // No atomic needed.
  bool paused_ = false;

  // Streaming two-store secondary. Empty during live ingest; receives every
  // object push during pause via DataSourceRuntimeHost::setObjectStoreTarget,
  // and is drained into the primary at resume. Owned by the manager so a
  // single instance serves every session (matches the pause-global model:
  // §3.1.7 of the streaming-manager report). Cheap when empty.
  std::unique_ptr<ObjectStore> secondary_object_store_;

  // Scalar twin of secondary_object_store_. Empty during live ingest;
  // receives every scalar push (source-level and parser bindings) during
  // pause via DataSourceRuntimeHost::setDataEngineTarget, and is drained
  // into the primary DataEngine at resume. Matching TimeDomainIds,
  // DatasetIds, and TopicIds with the primary are requested explicitly by
  // startSession + cbEnsureParserBinding so chunks can be drained by id
  // without any translation.
  std::unique_ptr<DataEngine> secondary_data_engine_;

  // DatasetId → owning session. unique_ptr so the worker thread can keep a
  // stable pointer to the session struct across map mutations on the UI
  // thread.
  std::unordered_map<DatasetId, std::unique_ptr<StreamingSession>> sessions_;
};

}  // namespace PJ
