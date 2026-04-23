#pragma once

#include <QObject>

#include <memory>

namespace PJ {

// Owns the datastore + ObjectStore and the DataSource plugin lifecycle.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<SessionManager>;

  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;
};

}  // namespace PJ
