#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
class DataProcessorService;

// Owns the datastore for the current app session. v1 scalar commit calls are
// expected on the GUI thread so plot adapters never observe mutation during
// paint. The object-topic parser registry is the exception: it is written from
// the streaming worker thread (the registrar callback fires when a plugin
// discovers a topic mid-stream) and read from the GUI thread every render tick,
// so it carries its own lock (object_parsers_mutex_) — see the parser* accessors
// and object_topic_parsers_ for the contract.
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

  // The session's data-processor service (filters/transforms run as eager
  // DerivedEngine nodes over this session's DataEngine). Plot widgets reach it
  // through the SessionManager pointer they already hold — the same "not threaded
  // through widget constructors" access pattern as curveColorRegistry above.
  [[nodiscard]] DataProcessorService& dataProcessorService() noexcept {
    return *processor_service_;
  }

  [[nodiscard]] DataReader createReader() const;

  /// Per-dataset display shift (display_time = raw_time - offset). Combines the
  /// dataset's TimeDomain offset (latent; configured only in tests today) with
  /// the "Use time offset" shift: when enabled, the dataset's OWN earliest
  /// timestamp, so its axis starts near zero. Read LIVE (no stale snapshot).
  [[nodiscard]] DisplayOffset displayOffset(DatasetId dataset_id) const;

  /// Time bounds of ONE dataset's data — scalar topics (DataEngine) and object
  /// topics (ObjectStore) unioned — in DISPLAY-relative seconds (display_time =
  /// raw_time - the dataset's display_offset). nullopt when the dataset holds no
  /// data. Centralizes the raw-ns -> display-seconds conversion so the streaming
  /// playback seed shares the file-load seed's offset-aware origin
  /// (AppSession::seedPlaybackFromSession); a bare raw-ns range can never reach
  /// the playback axis offset-blind.
  [[nodiscard]] std::optional<DisplayRange> datasetDisplayRange(DatasetId dataset_id) const;

  // --- "Use time offset": re-base each dataset's axis between absolute Unix-epoch
  // seconds and seconds-relative-to-its-own-start. The shift is PER-DATASET (each
  // dataset re-bases to its own earliest sample), so with several datasets each
  // starts at zero and their starts align — a deliberate UI choice. Only the
  // boolean is state; per-dataset shifts are computed live from data bounds in
  // displayOffset(), which is also the seam for future fine-tuned alignment. ---

  /// Whether the relative-time frame is enabled. Default off (neutral); the app
  /// shell sets the user-facing policy (PJ3 parity = on) via setUseTimeOffset.
  [[nodiscard]] bool useTimeOffset() const noexcept {
    return use_time_offset_;
  }

  /// Flip the frame and emit displayOffsetChanged() on an actual change. The
  /// numeric shifts follow automatically (displayOffset recomputes per dataset),
  /// so callers only re-render: drop curve-adapter offset caches + replot,
  /// re-seed the playback range, and shift the playhead by the per-dataset delta.
  void setUseTimeOffset(bool use);

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

  // Registers (or replaces) the parser for one object topic. Called from the
  // streaming worker thread via the registrar callback when a plugin discovers a
  // topic mid-stream; takes object_parsers_mutex_ exclusively. A replacement
  // installs a fresh ObjectParserSlot but never frees a parser a consumer still
  // holds: ParserBinding captures the keepalive (the old handle's shared_ptr), so
  // an in-flight parse keeps running against its snapshot until that binding
  // drops. The replaced slot is destructed AFTER the lock is released — its dtor
  // can run plugin teardown (and potentially dlclose), which must not happen
  // under the parser lock.
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
  ///
  /// Thread-safety: read from the GUI thread every render tick while the streaming
  /// worker may concurrently (re-)register the same topic; the read takes
  /// object_parsers_mutex_ shared, copies the three shared_ptrs out, and releases.
  /// A subsequent slot replacement never frees a parser this snapshot still names:
  /// take the binding per use and HOLD its keepalive across the parseObject call —
  /// do not cache the raw `parser` pointer past the snapshot's lifetime.
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

  // All data files loaded into this session, in load order (deduped by path —
  // see recordLoadedSource). The layout-save path serializes one <fileInfo> per
  // entry so a multi-file session round-trips; callers that only care about the
  // most recent file use lastLoadedSource() instead.
  [[nodiscard]] const std::vector<LoadedSource>& loadedSources() const noexcept {
    return loaded_sources_;
  }
  // The most recently loaded source, or nullopt if none. Backs the quick-reload
  // button, the 3D dock's source-path seeding, and the layout same-source check.
  [[nodiscard]] std::optional<LoadedSource> lastLoadedSource() const noexcept {
    if (loaded_sources_.empty()) {
      return std::nullopt;
    }
    return loaded_sources_.back();
  }
  // Records a loaded file. If a source with the same `path` is already tracked,
  // its entry is updated in place (preserving list order — a reload keeps the
  // file's position); otherwise the source is appended. This dedup-by-path keeps
  // reloads from growing duplicate <fileInfo> entries while additive loads of
  // distinct files all persist.
  void recordLoadedSource(QString path, QString prefix, QString plugin_id = {}, QString plugin_config_json = {});
  void clearLoadedSource() noexcept {
    loaded_sources_.clear();
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

  // Emitted when the shared display offset changes (the "Use time offset" frame
  // toggled, or a load moved the earliest sample). No topic changed, so plot
  // widgets must drop EVERY curve adapter's cached offset and replot — a
  // per-topic samplesIngested would skip them all.
  void displayOffsetChanged();

 private:
  // [min, max] raw-ns bounds across one dataset's scalar + object topics, or
  // nullopt when it holds no data. The one time-bounds union loop.
  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> datasetRawBounds(DatasetId dataset_id) const;

  // Earliest raw-ns timestamp of one dataset (0 when empty), memoized in
  // dataset_min_cache_. The per-dataset shift "Use time offset" subtracts.
  [[nodiscard]] Timestamp datasetMinTimestamp(DatasetId dataset_id) const;

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
  // Precondition: the caller already holds object_parsers_mutex_ (shared is
  // enough). Does NOT lock itself — so locking accessors never recurse into the
  // mutex, and the returned pointer stays valid only while that lock is held.
  [[nodiscard]] const ObjectParserSlot* findValidParserSlotLocked(ObjectTopicId id) const;

  DataEngine data_engine_;
  ObjectStore object_store_;
  CurveColorRegistry curve_color_registry_;
  // "Use time offset" frame state. Neutral default (off); the app shell drives
  // the user-facing default (on, PJ3 parity) through setUseTimeOffset.
  bool use_time_offset_ = false;
  // Per-dataset earliest-stamp memo for displayOffset() (read per playback tick +
  // per catalog item). Cleared on every commit/ingest so it can't go stale.
  mutable std::unordered_map<DatasetId, Timestamp> dataset_min_cache_;
  // Owns the session's filter/transform engine; constructed in the ctor body
  // after data_engine_ is alive (it binds a DerivedEngine to data_engine_).
  std::unique_ptr<DataProcessorService> processor_service_;
  // Per-object-topic parser slots. WRITTEN from the streaming worker thread (the
  // registrar callback fires when a plugin discovers/replaces a topic mid-stream)
  // and READ from the GUI thread on every render tick (each scene3D layer +
  // TransformService resolves its binding per use). Guarded by
  // object_parsers_mutex_: shared on the parser* accessors, exclusive on
  // register/evict/clear. Slot replacement keeps the keepalive shared_ptr
  // semantics — a replaced slot is destroyed OUTSIDE the lock (its dtor may run
  // plugin teardown / dlclose), and any ParserBinding snapshot keeps the replaced
  // parser alive for the consumer that captured it.
  mutable std::shared_mutex object_parsers_mutex_;
  std::unordered_map<uint32_t, ObjectParserSlot> object_topic_parsers_;
  std::vector<LoadedSource> loaded_sources_;
};

}  // namespace PJ
