#include "pj_runtime/SessionManager.h"

#include <QLoggingCategory>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcSession, "pj.runtime.session")
}  // namespace

SessionManager::SessionManager(QObject* parent) : QObject(parent) {}

SessionManager::~SessionManager() = default;

DataReader SessionManager::createReader() const {
  return data_engine_.createReader();
}

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  auto changed = data_engine_.commitChunks(std::move(chunks));
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

void SessionManager::registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
  const bool incoming_valid = parser != nullptr && parser->valid();
  if (!incoming_valid) {
    // Do not silently drop a previously valid registration just because the
    // caller handed us an invalid replacement — that produced "topic suddenly
    // can't be decoded anymore" with no diagnostic. Keep the prior parser and
    // warn loudly so the operator can see something is wrong with the binding.
    const auto existing = object_topic_parsers_.find(id.id);
    if (existing != object_topic_parsers_.end() && existing->second != nullptr && existing->second->valid()) {
      qCWarning(lcSession) << "registerObjectTopicParser: ignoring invalid replacement for topic" << id.id
                           << "(previous valid parser preserved)";
      return;
    }
    qCWarning(lcSession) << "registerObjectTopicParser: invalid parser for topic" << id.id << "— erasing registration";
    object_topic_parsers_.erase(id.id);
    return;
  }
  object_topic_parsers_[id.id] = std::move(parser);
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second == nullptr || !it->second->valid()) {
    return nullptr;
  }
  return static_cast<MessageParserPluginBase*>(it->second->context());
}

}  // namespace PJ
