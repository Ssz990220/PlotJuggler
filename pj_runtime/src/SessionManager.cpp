// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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
    emit samplesIngested(std::move(ids), /*live=*/false);
  }
  return changed;
}

void SessionManager::notifyIngest(QVector<TopicId> ids, bool live) {
  if (ids.isEmpty()) {
    return;
  }
  emit samplesIngested(std::move(ids), live);
}

void SessionManager::registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
  const bool incoming_valid = parser != nullptr && parser->valid();
  if (!incoming_valid) {
    // Do not silently drop a previously valid registration just because the
    // caller handed us an invalid replacement — that produced "topic suddenly
    // can't be decoded anymore" with no diagnostic. Keep the prior parser and
    // warn loudly so the operator can see something is wrong with the binding.
    const auto existing = object_topic_parsers_.find(id.id);
    if (existing != object_topic_parsers_.end() && existing->second.handle != nullptr &&
        existing->second.handle->valid()) {
      qCWarning(lcSession) << "registerObjectTopicParser: ignoring invalid replacement for topic" << id.id
                           << "(previous valid parser preserved)";
      return;
    }
    qCWarning(lcSession) << "registerObjectTopicParser: invalid parser for topic" << id.id << "— erasing registration";
    object_topic_parsers_.erase(id.id);
    return;
  }
  // Fresh mutex per registration: a re-registration swaps in a new parser, but
  // existing consumers still guard the old one. Reusing the lock would let the
  // new caller race with leftover work on the old parser pointer.
  object_topic_parsers_[id.id] = ObjectParserSlot{std::move(parser), std::make_shared<std::mutex>()};
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second.handle == nullptr || !it->second.handle->valid()) {
    return nullptr;
  }
  return static_cast<MessageParserPluginBase*>(it->second.handle->context());
}

std::shared_ptr<std::mutex> SessionManager::parserMutexForObjectTopic(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second.handle == nullptr || !it->second.handle->valid()) {
    return nullptr;
  }
  return it->second.mutex;
}

void SessionManager::recordLoadedSource(QString path, QString prefix, QString plugin_id, QString plugin_config_json) {
  last_loaded_source_ =
      LoadedSource{std::move(path), std::move(prefix), std::move(plugin_id), std::move(plugin_config_json)};
}

}  // namespace PJ
