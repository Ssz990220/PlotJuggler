// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SessionManager.h"

#include <QLoggingCategory>
#include <QString>
#include <cstdint>
#include <unordered_map>

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

DisplayOffset SessionManager::displayOffset(DatasetId dataset_id) const {
  // Live lookup via the time-domain map: the dataset's own time_domain is a
  // snapshot from createDataset, so reading its display_offset directly would go
  // stale after setDisplayOffset. Same discipline as DatastoreCurveAdapter.
  if (const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
      dataset != nullptr && dataset->time_domain.id != 0) {
    if (const TimeDomain* domain = data_engine_.getTimeDomain(dataset->time_domain.id)) {
      return offsetOf(*domain);
    }
  }
  return DisplayOffset{};  // unknown dataset or default domain → no shift
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

void SessionManager::replaceDataset(
    DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
    std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers) {
  // (1) Adapters drop cached TopicChunk* before any deque is touched. Same-thread
  // direct connection: every slot returns before the emit does, and this method
  // runs no event loop (the caller must not either) — so the pointers stay dead.
  emit datasetAboutToBeReplaced(primary_id);

  // (2a) Scalar swap. replaceDatasetFrom only errors on a programming mistake (same
  // engine, unknown dataset id), never on user data, and validates before mutating —
  // so on the unreachable error path the primary keeps its prior data. Log and continue.
  QVector<TopicId> changed;
  if (auto scalars = data_engine_.replaceDatasetFrom(staged_engine, staged_id, primary_id); scalars.has_value()) {
    changed.reserve(static_cast<int>(scalars->replaced_topics.size() + scalars->added_topics.size()));
    for (const TopicId t : scalars->replaced_topics) {
      changed.push_back(t);
    }
    for (const TopicId t : scalars->added_topics) {
      changed.push_back(t);
    }
  } else {
    qCWarning(lcSession).noquote() << "replaceDataset (scalars):" << QString::fromStdString(scalars.error());
  }

  // (2b) Object swap, then (3) re-register the collected parsers under the stable
  // primary ObjectTopicIds and drop parsers for object topics that vanished.
  if (auto objects = object_store_.replaceDatasetFrom(staged_store, staged_id, primary_id); objects.has_value()) {
    std::unordered_map<uint32_t, ObjectTopicId> staged_to_primary;
    staged_to_primary.reserve(objects->remapped.size());
    for (const auto& [staged_obj, primary_obj] : objects->remapped) {
      staged_to_primary.emplace(staged_obj.id, primary_obj);
    }
    for (auto& [staged_obj, parser] : staged_object_parsers) {
      const auto it = staged_to_primary.find(staged_obj.id);
      registerObjectTopicParser(it != staged_to_primary.end() ? it->second : staged_obj, std::move(parser));
    }
    evictObjectTopics(objects->removed_topics);
  } else {
    qCWarning(lcSession).noquote() << "replaceDataset (objects):" << QString::fromStdString(objects.error());
  }

  // (4) Re-index the (already-cleared) adapters against the swapped-in data.
  notifyIngest(std::move(changed), /*live=*/false);
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
  object_topic_parsers_[id.id] =
      ObjectParserSlot{std::shared_ptr<MessageParserHandle>(std::move(parser)), std::make_shared<std::mutex>()};
}

const SessionManager::ObjectParserSlot* SessionManager::findValidParserSlot(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second.handle == nullptr || !it->second.handle->valid()) {
    return nullptr;
  }
  return &it->second;
}

SessionManager::ParserBinding SessionManager::parserBindingForObjectTopic(ObjectTopicId id) const {
  const auto* slot = findValidParserSlot(id);
  if (slot == nullptr) {
    return {};
  }
  return ParserBinding{
      static_cast<MessageParserPluginBase*>(slot->handle->context()),
      slot->mutex,
      slot->handle,
  };
}

std::shared_ptr<void> SessionManager::parserKeepaliveForObjectTopic(ObjectTopicId id) const {
  // shared_ptr<MessageParserHandle> -> shared_ptr<void>: holding it keeps the
  // handle (and thus the parser instance + plugin DSO) alive for the consumer.
  const auto* slot = findValidParserSlot(id);
  return slot != nullptr ? slot->handle : nullptr;
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  const auto* slot = findValidParserSlot(id);
  return slot != nullptr ? static_cast<MessageParserPluginBase*>(slot->handle->context()) : nullptr;
}

std::shared_ptr<std::mutex> SessionManager::parserMutexForObjectTopic(ObjectTopicId id) const {
  const auto* slot = findValidParserSlot(id);
  return slot != nullptr ? slot->mutex : nullptr;
}

void SessionManager::recordLoadedSource(QString path, QString prefix, QString plugin_id, QString plugin_config_json) {
  last_loaded_source_ =
      LoadedSource{std::move(path), std::move(prefix), std::move(plugin_id), std::move(plugin_config_json)};
}

void SessionManager::evictDatasetObjects(DatasetId dataset_id) {
  evictObjectTopics(object_store_.listTopics(dataset_id));
}

void SessionManager::evictObjectTopics(const std::vector<ObjectTopicId>& topic_ids) {
  for (const ObjectTopicId topic_id : topic_ids) {
    object_store_.removeTopic(topic_id);
    // Topic gone: drop its parser too, so a reload (fresh ObjectTopicId) re-registers cleanly.
    object_topic_parsers_.erase(topic_id.id);
  }
}

void SessionManager::clearAllObjects() {
  object_store_.clear();
  object_topic_parsers_.clear();
}

}  // namespace PJ
