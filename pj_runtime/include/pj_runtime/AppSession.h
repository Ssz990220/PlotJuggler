#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class CatalogModel;
class CurveColorRegistry;
class ExtensionCatalogService;
class PlaybackEngine;
class SessionManager;

// Central runtime object for PlotJuggler 4. Owns long-lived application
// services; pj_app instantiates one of these at startup and wires the
// shell's widgets against the services it exposes.
class AppSession : public QObject {
  Q_OBJECT
 public:
  // Creates a session using the default extension directory.
  explicit AppSession(QObject* parent = nullptr);

  // Creates a session using an explicit extension directory.
  explicit AppSession(QString extensions_dir, QObject* parent = nullptr);

  // Creates a session with an explicit extension directory and diagnostics.
  AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  // Releases all long-lived application services.
  ~AppSession() override;

  // AppSession owns stateful services and cannot be copied.
  AppSession(const AppSession&) = delete;

  // AppSession owns stateful services and cannot be assigned.
  AppSession& operator=(const AppSession&) = delete;

  // Returns the session manager owned by this session.
  SessionManager& sessionManager() const {
    return *session_manager_;
  }

  // Returns the playback engine owned by this session.
  PlaybackEngine& playbackEngine() const {
    return *playback_engine_;
  }

  // Returns the catalog model owned by this session.
  CatalogModel& catalogModel() const {
    return *catalog_model_;
  }

  // Returns the extension catalog owned by this session.
  ExtensionCatalogService& extensionCatalog() const {
    return *extension_catalog_;
  }

  // Returns the session's curve-color registry. It is owned by SessionManager
  // (so plot widgets can reach it through the SessionManager pointer they hold
  // without it being threaded through their constructors); this is a convenience
  // delegate. Remembers each curve's color so it stays consistent across plots
  // (issue #68); cleared whenever the catalog empties.
  [[nodiscard]] CurveColorRegistry& curveColorRegistry() const;

  // Recomputes the playback range from the time bounds of the CATALOG-VISIBLE
  // topics + object topics (per-topic granularity, not per-dataset).
  // Visibility matters: the engine keeps removed/trashed topics' data
  // (append-only tombstones), and those must not stretch the timeline.
  // Recomputed from scratch each call, so the range also shrinks (remove or
  // trash data, reload a shorter file). Caller contract: rebuild the catalog
  // first, as the load/ingest paths already do.
  //
  // First seed after the session starts (or after the catalog emptied): also
  // snaps currentTime to the new minimum so the user lands at the start of the
  // data. Later seeds preserve the scrub position (setRange re-clamps it).
  //
  // Returns true if any visible topic with data was found and the engine was
  // updated; false when there is none.
  bool seedPlaybackFromSession();

  // FOCUS playback on specific datasets (a toolbox bulk import, e.g. a cloud
  // fetch): set the range to THEIR time bounds and snap currentTime to their
  // start — unlike seedPlaybackFromSession this intentionally REPLACES the
  // range, so a 10s snippet presents a 10s timeline even when older datasets
  // span hours. Scalar series bound the range; object topics are consulted
  // only when the datasets carry no scalar data at all (a 3D-only import) —
  // latched/static objects (tf_static, stale markers) must not stretch it.
  //
  // Returns true if any data was found and the engine was updated (callers
  // fall back to seedPlaybackFromSession otherwise).
  bool focusPlaybackOnDatasets(const std::vector<DatasetId>& datasets);

 private:
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
  // Declared last: its ctor hits disk (scan + load) and must run after the
  // other services are alive. The destructor resets services explicitly so
  // session-owned plugin handles die before loaded plugin libraries unload.
  std::unique_ptr<ExtensionCatalogService> extension_catalog_;

  // Flips to true on the first successful seedPlaybackFromSession(); re-armed
  // (set false) by the CatalogModel::cleared() hook when the catalog empties.
  // Used to distinguish "first load" (snap currentTime) from "additional load"
  // (preserve currentTime).
  bool playback_seeded_ = false;
};

}  // namespace PJ
