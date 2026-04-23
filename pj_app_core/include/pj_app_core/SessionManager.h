#pragma once

#include <QObject>

namespace PJ {

// Owns the datastore + ObjectStore and the DataSource plugin lifecycle.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
};

}  // namespace PJ
