#pragma once

#include <QObject>

namespace PJ {

// Central runtime object for PlotJuggler 4.
// Owns long-lived application services. For the initial milestone it is
// intentionally empty — services will be added as Phase 0 lands (SessionManager,
// PlaybackEngine, WorkspaceManager, etc. per PJ4_PLAN §6.1).
class AppSession : public QObject {
  Q_OBJECT
 public:
  explicit AppSession(QObject* parent = nullptr);
  ~AppSession() override;

  AppSession(const AppSession&) = delete;
  AppSession& operator=(const AppSession&) = delete;
};

}  // namespace PJ
