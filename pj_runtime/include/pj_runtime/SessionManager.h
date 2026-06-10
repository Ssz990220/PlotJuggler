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
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/Time.h"

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

  // Session-scoped memory of each curve's assigned color, so a curve keeps its
  // color when dragged into another plot (issue #68). Plot widgets reach it
  // through the SessionManager pointer they already hold, so the registry need
  // not be threaded through the widget constructors. AppSession clears it when
  // the catalog empties.
  [[nodiscard]] CurveColorRegistry& curveColorRegistry() noexcept {
    return curve_color_registry_;
  }

  [[nodiscard]] DataReader createReader() const;

  /// Per-dataset display shift (display_time = raw_time - offset), read LIVE from
  /// the time-domain map (the dataset's own snapshot can go stale after
  /// setDisplayOffset). Zero for an unknown dataset or the default (id 0) domain.
  [[nodiscard]] DisplayOffset displayOffset(DatasetId dataset_id) const;

  [[nodiscard]] std::vector<TopicId> commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks);

  // Re-emit hook for callers that wrote straight through the DataEngine,
  // bypassing commitChunks() (file/stream ingest has no Qt awareness) — without
  // it, plot adapter caches never invalidate on the ingested data. `live`
  // propagates to samplesIngested; follow-live consumers (PlotWidget auto-fit,
  // Scene2DDockWidget frame advance) act only when it is true.
  void notifyIngest(QVector<PJ::TopicId> ids, bool live = false);

  // In-place reload swap: replace `primary_id`'s scalar + object data with the
  // data staged under `staged_id` in `staged_engine`/`staged_store`, keeping the
  // primary DatasetId/TopicId/ObjectTopicId — and thus every curve key and
  // 2D-dock binding — STABLE. Ordered transaction: datasetAboutToBeReplaced ->
  // DataEngine + ObjectStore replaceDatasetFrom -> re-register staged object
  // parsers under the stable primary ids and drop removed ones -> notifyIngest.
  // Runs NO event loop. Caller must stage into a throwaway engine/store, run no
  // event loop between staging and this call, and rebuild the catalog after it
  // returns (also with no event loop in between).
  void replaceDataset(
      DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
      std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers);

  void registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser);
  struct ParserBinding {
    MessageParserPluginBase* parser = nullptr;
    std::shared_ptr<std::mutex> mutex;
    std::shared_ptr<void> keepalive;

    [[nodiscard]] explicit operator bool() const noexcept {
      return parser != nullptr && keepalive != nullptr;
    }
  };

  /// Returns the parser pointer, shared parse mutex, and DSO keepalive for one
  /// object topic as a single snapshot. Empty when no valid parser is registered.
  [[nodiscard]] ParserBinding parserBindingForObjectTopic(ObjectTopicId id) const;
  [[nodiscard]] MessageParserPluginBase* parserForObjectTopic(ObjectTopicId id) const;

  // Mutex shared by every consumer of parserForObjectTopic(id). MessageParser
  // plugins are not thread-safe (fastcdr et al. keep stateful scratch), so
  // workers sharing the singleton parser MUST hold this around each parseObject
  // call. Returns nullptr if no parser is registered for the topic.
  [[nodiscard]] std::shared_ptr<std::mutex> parserMutexForObjectTopic(ObjectTopicId id) const;

  // Shared keepalive for the parser handle behind parserForObjectTopic(id):
  // holding it keeps the parser instance AND its plugin DSO mapped until the
  // consumer drops it. Display sources run a decode worker that calls
  // parseObject; on app shutdown the extension catalog (and the plugin DSO) can
  // be torn down before that worker is joined, so the worker MUST hold this to
  // avoid a use-after-free / use-after-dlclose. Returns nullptr if no parser is
  // registered for the topic.
  [[nodiscard]] std::shared_ptr<void> parserKeepaliveForObjectTopic(ObjectTopicId id) const;

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

  // Object eviction. DataEngine scalars are append-only, so dataset removal keeps
  // them (catalog tombstone); ObjectStore canonical objects are heavy and
  // evictable, so removal frees them outright.
  void evictDatasetObjects(DatasetId dataset_id);
  // Evicts a specific set of object topics (and their parsers), for trashing a
  // selection of object-topic entries rather than a whole dataset.
  void evictObjectTopics(const std::vector<ObjectTopicId>& topic_ids);
  void clearAllObjects();

 signals:
  // Emitted when topics receive new samples (commit/ingest path). `live` is
  // true only for follow-live writers (streaming today). Cache-invalidation
  // consumers can drop the trailing arg; auto-pan/auto-fit consumers branch
  // on it.
  void samplesIngested(QVector<PJ::TopicId> ids, bool live);

  // Emitted by replaceDataset just before a reload swaps a dataset's chunks in
  // place. Bound plot adapters must synchronously drop cached chunk pointers; the
  // DatasetId/TopicIds stay valid (unlike catalog removal), only chunks change.
  void datasetAboutToBeReplaced(PJ::DatasetId dataset_id);

 private:
  struct ObjectParserSlot {
    // shared_ptr (not unique_ptr) so a display source can hold the handle alive
    // past topic removal / app teardown — keeping the parser instance and its
    // plugin DSO mapped until that source's decode worker is joined.
    std::shared_ptr<MessageParserHandle> handle;
    // shared_ptr so consumers can keep the mutex alive past topic removal —
    // the lock guards their in-flight parseObject call to completion.
    std::shared_ptr<std::mutex> mutex;
  };

  // Returns the slot for `id` iff it holds a live parser handle, else nullptr.
  // Collapses the find + null + valid() guard shared by the parser* accessors.
  [[nodiscard]] const ObjectParserSlot* findValidParserSlot(ObjectTopicId id) const;

  DataEngine data_engine_;
  ObjectStore object_store_;
  CurveColorRegistry curve_color_registry_;
  std::unordered_map<uint32_t, ObjectParserSlot> object_topic_parsers_;
  std::optional<LoadedSource> last_loaded_source_;
};

}  // namespace PJ
