#include "pj_runtime/SessionManager.h"

namespace PJ {

SessionManager::SessionManager(QObject* parent) : QObject(parent) {}

SessionManager::~SessionManager() = default;

DataReader SessionManager::createReader() const {
  return data_engine_.createReader();
}

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  std::vector<TopicId> changed = data_engine_.commitChunks(std::move(chunks));
  if (!changed.empty()) {
    QVector<TopicId> ids;
    ids.reserve(static_cast<qsizetype>(changed.size()));
    for (const TopicId id : changed) {
      ids.push_back(id);
    }
    emit topicsCommitted(std::move(ids));
  }
  return changed;
}

}  // namespace PJ
