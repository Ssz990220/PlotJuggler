#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QVector>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
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
class RefillGuard;

/// Result of resolving a persisted dataset identity against the live session.
/// `ambiguous` distinguishes "not loaded yet" from multiple equally valid
/// candidates, which lets deferred scene/plot restore wait for the former and
/// reject the latter without ever selecting by load order. `id == nullopt` with
/// `ambiguous == false` means no live dataset matches (yet).
struct DatasetIdentityResolution {
  std::optional<DatasetId> id;
  bool ambiguous = false;
};

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

  /// Associates a file-backed dataset with the normalized full path from which
  /// FileLoader created it. DatasetInfo::source_name is intentionally only a
  /// display/raw-source label (often a basename), so it cannot distinguish two
  /// files with the same name in different directories. The path is normalized
  /// on store (canonicalFilePath, falling back to absoluteFilePath) so lookups
  /// match regardless of symlink/relative aliasing. Empty `path` removes the
  /// association. GUI-thread only.
  void setDatasetSourcePath(DatasetId dataset_id, QString path);

  /// Normalized full source path registered for `dataset_id`, or empty for a
  /// streaming/test dataset and for an id no longer tracked by FileLoader.
  [[nodiscard]] QString datasetSourcePath(DatasetId dataset_id) const;

  /// Resolves a persisted identity without guessing. Resolution order:
  ///  1. The exact `saved_id`, trusted only while every supplied qualifier (raw
  ///     source label and/or full source path) still agrees — this is what lets
  ///     a same-session undo keep its exact DatasetId even amid duplicates.
  ///  2. If the id was reminted, a path-qualified identity falls back to exactly
  ///     one dataset whose registered path (and source label) matches.
  ///  3. A legacy layout without a path falls back to an exactly-one source-label
  ///     match.
  /// More than one candidate at step 2/3 returns `{id=nullopt, ambiguous=true}`
  /// rather than choosing by load order. An id carrying no portable qualifiers
  /// (empty source AND empty path) is valid only while that exact id still
  /// exists; otherwise it resolves to nothing (not ambiguous).
  [[nodiscard]] DatasetIdentityResolution resolveDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path = {}) const;

  /// Topic-aware fallback for a same-file single<->fan-out remint. When full-path
  /// identity alone names several datasets (resolveDatasetIdentity returns
  /// ambiguous), returns a dataset only if exactly one of those siblings owns an
  /// object topic named `object_topic_name`; still ambiguous if several do.
  [[nodiscard]] DatasetIdentityResolution resolveObjectDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path,
      const QString& object_topic_name) const;

  /// TOTAL display shift used by everything that renders on the display axis
  /// (plot curves, scenes, the playback range/cursor): the dataset's per-source
  /// alignment offset PLUS the global "Use time offset" reference. So
  /// display_time = raw_time - sourceDisplayOffset - globalTimeReference().
  /// The two are summed here, not stored together, so a toggle of the global
  /// frame never disturbs the per-source alignment the Source Timeline owns.
  [[nodiscard]] DisplayOffset displayOffset(DatasetId dataset_id) const;

  /// Per-source ALIGNMENT shift ONLY (the dataset's TimeDomain offset, read
  /// LIVE): exactly what the Source Timeline edits via setDisplayOffset (drag /
  /// align / reset) and what positions its bars. Excludes the global "Use time
  /// offset" reference, so the Timeline's bar positions stay invariant when that
  /// global frame is toggled — only their number FORMATTING changes. Use this
  /// (not displayOffset) anywhere bar/track positions must not follow the toggle.
  [[nodiscard]] DisplayOffset sourceDisplayOffset(DatasetId dataset_id) const;

  /// The single global origin (raw ns) subtracted from ALL displayed times when
  /// "Use time offset" is on: the earliest raw sample across every loaded
  /// dataset, applied UNIFORMLY so cross-dataset time gaps are preserved (unlike
  /// a per-source rebase, which collapses every dataset to zero). Zero when the
  /// toggle is off. Memoized; invalidated on every commit/ingest. The host (the
  /// Source Timeline controller) reads it to bridge the playback frame
  /// (raw - source - global) and the Timeline frame (raw - source).
  [[nodiscard]] Timestamp globalTimeReference() const;

  /// Write a source's display shift (display_time = raw_time - offset) and
  /// notify consumers via displayOffsetChanged(DatasetId). Resolves dataset ->
  /// its TimeDomain -> DataEngine; no-op + warning if the dataset is unknown or
  /// bound to the default (id 0) domain. This is the per-source Timeline-drag
  /// seam; it composes with the global "Use time offset" frame below (the offset
  /// it writes is the latent TimeDomain shift displayOffset() reads).
  void setDisplayOffset(DatasetId dataset_id, DisplayOffset offset);

  /// Time bounds of ONE dataset's data — scalar topics (DataEngine) and object
  /// topics (ObjectStore) unioned — in DISPLAY-relative seconds (display_time =
  /// raw_time - the dataset's display_offset). nullopt when the dataset holds no
  /// data. Centralizes the raw-ns -> display-seconds conversion so the streaming
  /// playback seed shares the file-load seed's offset-aware origin
  /// (AppSession::seedPlaybackFromSession); a bare raw-ns range can never reach
  /// the playback axis offset-blind.
  [[nodiscard]] std::optional<DisplayRange> datasetDisplayRange(DatasetId dataset_id) const;

  // --- "Use time offset": switch the displayed time frame between absolute
  // Unix-epoch seconds (off) and seconds relative to a single GLOBAL origin (on).
  // The shift is one uniform global reference (globalTimeReference) applied to
  // every dataset, so multi-dataset time gaps are preserved. It is layered on top
  // of (never overwrites) the per-source alignment offset, so toggling it leaves
  // the Source Timeline's bar positions untouched. Only the boolean is state. ---

  /// Whether the relative-time frame is enabled. Default off (neutral); the app
  /// shell sets the user-facing policy (PJ3 parity = on) via setUseTimeOffset.
  [[nodiscard]] bool useTimeOffset() const noexcept {
    return use_time_offset_;
  }

  /// Toggle the global relative-time frame and emit the no-arg
  /// displayOffsetChanged() on an actual change. It flips only globalTimeReference
  /// (ON → earliest raw sample across all datasets; OFF → 0); it does NOT touch
  /// any per-source alignment offset, so a Timeline drag/align is independent and
  /// survives a toggle. Callers re-render on the signal: drop curve-adapter offset
  /// caches + replot, re-seed the playback range, reformat the Timeline numbers.
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

  // Begin a TRANSACTIONAL in-place refill of `dataset_id` — the progressive-load
  // reload prep that keeps its DatasetId / TopicIds / ObjectTopicIds registered, so
  // bound plot adapters stay valid and the refill writes back into the same ids. The
  // returned RAII guard has already, on the GUI thread with NO event loop:
  //   1) emit datasetAboutToBeReplaced(dataset_id)  // adapters drop cached TopicChunk*
  //   2) DETACHED (moved aside, NOT freed) the scalar chunks + object entries
  //   3) notifyIngest(<dataset's topic ids>, live=false)  // UI shows it empty
  // Step 1 BEFORE 2 is the invariant that prevents a use-after-free on cached chunk
  // pointers (same ordering as replaceDataset); a dataset with no topics skips the
  // notify (notifyIngest no-ops on an empty id list). The caller commit()s the guard
  // once the refill succeeds (or to keep a partial load); otherwise the guard's
  // destructor ROLLS BACK, restoring the exact prior data, retiring/removing topics
  // the failed refill added, and re-notifying the UI. Detaching rather than freeing is
  // what makes the rollback possible — the prior data lives in the snapshot until
  // commit(). Runs NO event loop until commit/rollback.
  [[nodiscard]] RefillGuard beginRefill(PJ::DatasetId dataset_id);

  // Destructively fold `sources` (each with a raw timestamp shift) into the
  // `anchor` dataset via DataEngine::mergeDatasets. Mirrors replaceDataset's
  // ordered transaction: emit datasetAboutToBeReplaced for the anchor + every
  // source (adapters drop cached chunk pointers) -> engine merge -> object-store
  // merge (fold shared names, reparent source-only object topics) -> notifyIngest
  // the anchor's changed topics. Returns the engine's report on success, or
  // std::nullopt when the engine REJECTED the merge (the store is untouched), so
  // the caller can leave the catalog consistent rather than removing sources
  // whose data still lives in the store — distinct from a successful merge that
  // happens to fold no topics (a non-null but empty report). The caller computes
  // the anchor/shifts (display-offset policy) and, only on success, updates the
  // catalog (remove the consumed datasets, relabel the anchor). Runs no event
  // loop; the caller must rebuild the catalog after it returns.
  std::optional<DatasetMergeReport> mergeDatasets(DatasetId anchor, const std::vector<DatasetMergeSource>& sources);

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
  // Records a loaded file. `path` is normalized on store (same rules as
  // setDatasetSourcePath), so a symlink/relative alias of an already-tracked
  // file updates its entry in place (preserving list order — a reload keeps the
  // file's position); otherwise the source is appended. This dedup-by-physical-
  // path keeps reloads from growing duplicate <fileInfo> entries while additive
  // loads of distinct files all persist.
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

  // REAL removal of a dataset's scalar storage from the engine, paired with
  // invalidating its time-origin caches so globalTimeReference() re-bases to the
  // surviving data. Use this instead of reaching into dataEngine().removeDataset
  // directly — the latter leaves the memoized earliest-sample origin stale.
  // Callers must first tear down readers/catalog items (removeDataset's
  // invalidate-first contract) and evict the dataset's objects.
  void removeDataset(DatasetId dataset_id);

  // Recompute `dataset_id`'s pinned earliest-sample origin from its CURRENT data
  // and emit the global reframe if the cross-dataset origin moved. FileLoader calls
  // this at a load-completion seam: plugin ingest commits straight to DataEngine via
  // the C-ABI write host (bypassing commitChunks/notifyIngest), so a finished short
  // file whose data is earlier than any prior dataset would otherwise leave curve
  // adapters on a stale display offset. No-op when "Use time offset" is off (the
  // global reference is 0 regardless). Invalidates the dataset's min pin first so a
  // shrunk/earlier range is picked up.
  void refreshDatasetTimeReference(DatasetId dataset_id);

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
  // per-topic samplesIngested would skip them all. Connect with qOverload<>(...)
  // (overloaded against the per-dataset signal below).
  void displayOffsetChanged();

  // Emitted when ONE dataset's display offset changes (a Timeline drag/align via
  // setDisplayOffset). No data moved — only its display->raw mapping; consumers
  // drop that dataset's offset caches and replot/re-snap without re-indexing
  // samples. Connect with qOverload<PJ::DatasetId>(...).
  void displayOffsetChanged(PJ::DatasetId dataset_id);

 private:
  // RefillGuard drives the transactional reload through this class's public
  // detach/reattach + notify surface; it needs the private signal-emit helper
  // below so the datasetAboutToBeReplaced emission stays inside SessionManager.
  friend class RefillGuard;
  // Emit datasetAboutToBeReplaced from inside the class (a foreign object's
  // signal must not be emitted from outside its own members under moc).
  void notifyDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  // The distinct datasets that own the given object topics (skipping the unset
  // dataset id 0). Callers snapshot this set BEFORE removing the topics, so the
  // affected datasets' pinned minima can be invalidated once their descriptors
  // are gone. Shared by evictObjectTopics and clearAllObjects.
  [[nodiscard]] std::unordered_set<DatasetId> datasetsOwningObjectTopics(
      const std::vector<ObjectTopicId>& topic_ids) const;

  // [min, max] raw-ns bounds across one dataset's scalar + object topics, or
  // nullopt when it holds no data. The one time-bounds union loop.
  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> datasetRawBounds(DatasetId dataset_id) const;

  // Earliest raw-ns timestamp of one dataset (0 when empty), pinned in
  // dataset_min_cache_. The per-dataset shift "Use time offset" subtracts it.
  // The value may move earlier when older/out-of-order data arrives, but does
  // not move later just because streaming retention evicted old rows.
  [[nodiscard]] Timestamp datasetMinTimestamp(DatasetId dataset_id) const;
  [[nodiscard]] Timestamp datasetDomainDisplayOffset(DatasetId dataset_id) const;
  [[nodiscard]] Timestamp rememberDatasetMinTimestamp(DatasetId dataset_id, Timestamp observed_min) const;
  void refreshDatasetMinTimestampsForTopics(const QVector<TopicId>& ids) const;
  void invalidateDatasetMinTimestamp(DatasetId dataset_id) const;
  // Emits the no-arg displayOffsetChanged signal exactly when the numerical
  // global reference moved since consumers were last notified. Transactional
  // replace/refill/eviction paths invalidate the origin cache before mutating
  // data, so comparing at this final seam is more reliable than retaining a
  // local "old" value across the mutation.
  void notifyGlobalTimeReferenceIfChanged();

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
  // Per-dataset earliest-stamp pin for displayOffset() (read per playback tick +
  // per catalog item). Normal ingest can only lower it; dataset replacement
  // invalidates it so reloads rebase to the new contents.
  mutable std::unordered_map<DatasetId, Timestamp> dataset_min_cache_;
  // Earliest raw stamp across ALL datasets, memoized for globalTimeReference().
  // Independent of the toggle (it's a raw-data fact), so it survives a
  // setUseTimeOffset but is cleared on every commit/ingest like the per-dataset
  // memo. nullopt = not yet computed; 0 = computed, no data.
  //
  // INVARIANT: the globalTimeReference() recompute TRUSTS the per-dataset pins
  // (dataset_min_cache_) instead of rescanning raw bounds, so anything that can
  // RAISE a dataset's earliest sample (removeDataset / evictObjectTopics /
  // clearAllObjects / refill / refreshDatasetTimeReference) MUST invalidate that
  // dataset's pin AND reset this global memo — invalidateDatasetMinTimestamp does
  // both. Ingest can only LOWER a pin (rememberDatasetMinTimestamp never raises
  // it), so the ingest path only resets this memo. Skipping either reset on a
  // raise path would leave the recompute reading a stale-low origin.
  mutable std::optional<Timestamp> global_min_cache_;
  // The globalTimeReference() value at the most recent displayOffsetChanged
  // emission (from setUseTimeOffset or notifyGlobalTimeReferenceIfChanged), so
  // the latter suppresses a redundant frame-change signal when the origin is
  // unmoved. 0 matches the neutral "no data / offset off" origin.
  Timestamp last_notified_global_reference_ = 0;
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
  // FileLoader-owned portable identity, centralized here so plots, processors,
  // and both scene families resolve the same (id, source, full-path) contract.
  // Paths are stored normalized (see setDatasetSourcePath).
  std::unordered_map<DatasetId, QString> dataset_source_paths_;
  std::vector<LoadedSource> loaded_sources_;
};

/// RAII transaction wrapping a "replacing reload" (see SessionManager::beginRefill).
/// Construction emits datasetAboutToBeReplaced, then DETACHES the dataset's prior data
/// into a side snapshot instead of freeing it, leaving every TopicId/ObjectTopicId
/// registered + empty so a progressive refill writes back into the same ids. Default
/// outcome is ROLLBACK: a guard destroyed without commit()
/// restores the exact prior scalar + object data, retires/removes topics the failed
/// refill added, evicts parsers for ADDED object topics, and re-notifies the UI.
/// commit() keeps the refilled data (frees the snapshot; the dtor then no-ops).
///
/// GUI-thread only; runs NO event loop between construction and commit/rollback.
/// Move-only (held in std::optional<RefillGuard> by FileLoader's LoadContext). Does
/// NOT touch the per-dataset TF buffer — FileLoader owns transform_service_ and
/// rebuilds TF after rollback from the restored ObjectStore.
class RefillGuard {
 public:
  RefillGuard(SessionManager& session, DatasetId dataset_id);
  ~RefillGuard();
  RefillGuard(RefillGuard&&) noexcept;
  RefillGuard& operator=(RefillGuard&&) noexcept;
  RefillGuard(const RefillGuard&) = delete;
  RefillGuard& operator=(const RefillGuard&) = delete;

  /// Keep the refilled data: drop the snapshot so the destructor no longer rolls back.
  void commit();
  /// Retire prior topics that VANISHED from the reloaded file (codex #2): a prior
  /// scalar/object topic still empty after the refill is one the new file no longer
  /// has (beginRefill emptied every prior topic; the refill writes back only the
  /// ones still present), so retire the empty scalar topics and remove the empty
  /// object topics to avoid a ghost entry lingering in the catalog. Call ONLY after
  /// a COMPLETE refill (an unreached topic on a partial/cancelled load is not
  /// "vanished") and BEFORE commit() (which frees the prior-topic snapshots this
  /// reads). No-op when nothing was detached. A prior topic that the new file still
  /// declares but with zero rows is also retired — a dataless topic has no curve.
  void pruneVanishedTopics();
  /// Held-aside scalar (approximate) + object (exact) bytes — observability for the
  /// transient ~1x memory overshoot. 0 when nothing was detached or after commit().
  [[nodiscard]] std::size_t snapshotBytes() const noexcept;

 private:
  void rollback();

  SessionManager* session_ = nullptr;  // nulled after a move so the moved-from dtor no-ops
  DatasetId dataset_id_ = 0;
  DataEngine::DatasetChunkSnapshot scalar_snapshot_;
  ObjectStore::ObjectDatasetSnapshot object_snapshot_;
  std::vector<ObjectTopicId> prior_object_topic_ids_;
  bool committed_ = false;
};

}  // namespace PJ
