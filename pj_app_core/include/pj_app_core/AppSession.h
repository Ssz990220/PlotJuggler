#pragma once

#include <QObject>

#include <memory>

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
  explicit AppSession(QObject* parent = nullptr);
  ~AppSession() override;

  AppSession(const AppSession&) = delete;
  AppSession& operator=(const AppSession&) = delete;

  // Services are owned by AppSession for its full lifetime; widgets always
  // outlive the objects they reference here. Returning references makes that
  // ownership contract explicit and drops the raw-pointer hazard.
  SessionManager& sessionManager() const { return *session_manager_; }
  PlaybackEngine& playbackEngine() const { return *playback_engine_; }
  CatalogModel& catalogModel() const { return *catalog_model_; }
  ExtensionCatalogService& extensionCatalog() const { return *extension_catalog_; }

 private:
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
  // Declared last: its ctor hits disk (scan + load) and must run after the
  // other services are alive.
  std::unique_ptr<ExtensionCatalogService> extension_catalog_;
};

}  // namespace PJ
