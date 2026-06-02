#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <memory>

#include "pj_base/diagnostic_sink.hpp"

namespace PJ {

class CatalogModel;
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

  // Scans every dataset's topics + object topics for time bounds and applies
  // them to the playback engine.
  //
  // First call (no prior seed): sets the range and snaps currentTime to the
  // new minimum so the user lands at the start of the data.
  //
  // Subsequent calls: expand the range monotonically so additional file loads
  // never shrink it, and leave currentTime alone so the user's scrub position
  // is preserved across loads.
  //
  // Returns true if any topic with data was found and the engine was updated.
  bool seedPlaybackFromSession();

 private:
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
  // Declared last: its ctor hits disk (scan + load) and must run after the
  // other services are alive. The destructor resets services explicitly so
  // session-owned plugin handles die before loaded plugin libraries unload.
  std::unique_ptr<ExtensionCatalogService> extension_catalog_;

  // Flips to true on the first successful seedPlaybackFromSession(). Used to
  // distinguish "first load" (snap currentTime) from "additional load"
  // (preserve currentTime).
  bool playback_seeded_ = false;
};

}  // namespace PJ
