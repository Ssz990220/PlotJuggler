#pragma once

#include <QObject>

#include <memory>

namespace PJ {

class CatalogModel;
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

  SessionManager* sessionManager() const { return session_manager_.get(); }
  PlaybackEngine* playbackEngine() const { return playback_engine_.get(); }
  CatalogModel* catalogModel() const { return catalog_model_.get(); }

 private:
  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
};

}  // namespace PJ
