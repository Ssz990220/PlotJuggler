#pragma once

#include <QObject>
#include <QVector>
#include <memory>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"

namespace PJ {

// Owns the datastore for the current app session. v1 commit calls are expected
// on the GUI thread so plot adapters never observe mutation during paint.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<SessionManager>;

  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  [[nodiscard]] DataEngine& dataEngine() noexcept {
    return data_engine_;
  }
  [[nodiscard]] DataReader createReader() const;

  [[nodiscard]] std::vector<TopicId> commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks);

 signals:
  void topicsCommitted(QVector<PJ::TopicId> ids);

 private:
  DataEngine data_engine_;
};

}  // namespace PJ
