#pragma once

#include <QObject>

namespace PJ {

// Owns the datastore + ObjectStore and the DataSource plugin lifecycle.
// Empty skeleton for the prototype milestone — the real implementation wraps
// pj_datastore::Engine and drives data-source sessions in Phase 1.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
};

}  // namespace PJ
