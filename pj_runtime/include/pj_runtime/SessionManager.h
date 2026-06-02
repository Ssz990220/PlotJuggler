#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"

namespace PJ {

class MessageParserPluginBase;

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
  [[nodiscard]] ObjectStore& objectStore() noexcept {
    return object_store_;
  }
  [[nodiscard]] DataReader createReader() const;

  [[nodiscard]] std::vector<TopicId> commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks);

  // Re-emit hook for callers that wrote straight through the DataEngine,
  // bypassing commitChunks() (file/stream ingest has no Qt awareness) — without
  // it, plot adapter caches never invalidate on the ingested data. `live`
  // propagates to samplesIngested; follow-live consumers (PlotWidget auto-fit,
  // Media2DDockWidget frame advance) act only when it is true.
  void notifyIngest(QVector<PJ::TopicId> ids, bool live = false);

  void registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser);
  [[nodiscard]] MessageParserPluginBase* parserForObjectTopic(ObjectTopicId id) const;

  // Mutex shared by every consumer of parserForObjectTopic(id). MessageParser
  // plugins are not thread-safe (fastcdr et al. keep stateful scratch), so
  // workers sharing the singleton parser MUST hold this around each parseObject
  // call. Returns nullptr if no parser is registered for the topic.
  [[nodiscard]] std::shared_ptr<std::mutex> parserMutexForObjectTopic(ObjectTopicId id) const;

  struct LoadedSource {
    QString path;
    QString prefix;
    QString plugin_id;           // Empty when the loader didn't record a plugin (e.g. legacy paths).
    QString plugin_config_json;  // Plugin's saveConfig() JSON at load time.
  };

  [[nodiscard]] std::optional<LoadedSource> lastLoadedSource() const noexcept {
    return last_loaded_source_;
  }
  void recordLoadedSource(QString path, QString prefix, QString plugin_id = {}, QString plugin_config_json = {});
  void clearLoadedSource() noexcept {
    last_loaded_source_.reset();
  }

 signals:
  // Emitted when topics receive new samples (commit/ingest path). `live` is
  // true only for follow-live writers (streaming today). Cache-invalidation
  // consumers can drop the trailing arg; auto-pan/auto-fit consumers branch
  // on it.
  void samplesIngested(QVector<PJ::TopicId> ids, bool live);

 private:
  struct ObjectParserSlot {
    std::unique_ptr<MessageParserHandle> handle;
    // shared_ptr so consumers can keep the mutex alive past topic removal —
    // the lock guards their in-flight parseObject call to completion.
    std::shared_ptr<std::mutex> mutex;
  };

  DataEngine data_engine_;
  ObjectStore object_store_;
  std::unordered_map<uint32_t, ObjectParserSlot> object_topic_parsers_;
  std::optional<LoadedSource> last_loaded_source_;
};

}  // namespace PJ
