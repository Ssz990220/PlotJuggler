// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SessionManager.h"

#include <QFile>
#include <QLoggingCategory>
#include <QString>
#include <QThread>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcSession, "pj.runtime.session")
}  // namespace

SessionManager::SessionManager(QObject* parent) : QObject(parent) {
  // data_engine_ is already alive (member init precedes the ctor body), so the
  // processor service can bind its DerivedEngine to it.
  processor_service_ = std::make_unique<DataProcessorService>(data_engine_);

  // Install the bundled Luau filter catalogue so applied/restored filters resolve
  // to Luau classes. The resource is embedded in the app; it is absent only in a
  // headless unit test, where that binary installs its own catalogue if it needs
  // filters (there is no native C++ builtin fallback after M9).
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  if (QFile f(QStringLiteral(":/filters/builtin_filters.luau")); f.open(QIODevice::ReadOnly)) {
    if (auto added = catalogue->addBundledSource(f.readAll().toStdString(), "bundled"); !added.has_value()) {
      qCWarning(lcSession) << "filter catalogue load failed:" << QString::fromStdString(added.error());
    }
  }
  // Install only a non-empty catalogue: a headless binary without the embedded
  // resource simply has no filters rather than an empty registry.
  if (!catalogue->entries().empty()) {
    processor_service_->setFilterCatalogue(std::move(catalogue));
  }
}

SessionManager::~SessionManager() = default;

DataReader SessionManager::createReader() const {
  return data_engine_.createReader();
}

Timestamp SessionManager::datasetDomainDisplayOffset(DatasetId dataset_id) const {
  // Base shift from the dataset's TimeDomain. Live lookup via the time-domain
  // map: the dataset's own time_domain is a snapshot from createDataset, so
  // reading its display_offset directly would go stale after setDisplayOffset.
  Timestamp offset_ns = 0;
  if (const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
      dataset != nullptr && dataset->time_domain.id != 0) {
    if (const TimeDomain* domain = data_engine_.getTimeDomain(dataset->time_domain.id)) {
      offset_ns = domain->display_offset;
    }
  }
  return offset_ns;
}

DisplayOffset SessionManager::sourceDisplayOffset(DatasetId dataset_id) const {
  // The per-source ALIGNMENT shift only (display_time = raw_time - offset): the
  // dataset's TimeDomain offset, which is exactly what the Source Timeline edits.
  // The global "Use time offset" reference is NOT folded in here (see
  // displayOffset), so a Timeline drag round-trips without double-counting and
  // the bars stay put when the global frame is toggled.
  return DisplayOffset{Duration{datasetDomainDisplayOffset(dataset_id)}};
}

DisplayOffset SessionManager::displayOffset(DatasetId dataset_id) const {
  // Total display shift for the display axis = per-source alignment + the global
  // "Use time offset" origin. Summed here (not stored together) so toggling the
  // global frame never disturbs the per-source alignment.
  return DisplayOffset{sourceDisplayOffset(dataset_id).value + Duration{globalTimeReference()}};
}

Timestamp SessionManager::globalTimeReference() const {
  // Zero (absolute display) when the toggle is off. When on, the earliest raw
  // sample ever observed across ALL datasets, applied uniformly. Per-dataset
  // mins are pinned so live retention cannot slide the display origin forward.
  if (!use_time_offset_) {
    return 0;
  }
  if (!global_min_cache_.has_value()) {
    Timestamp global_min = std::numeric_limits<Timestamp>::max();
    bool found = false;
    for (const DatasetId dataset_id : data_engine_.listDatasets()) {
      // rememberDatasetMinTimestamp pins the earliest-ever min in a single scan
      // (the current raw min only moves forward under retention, never below it).
      const auto bounds = datasetRawBounds(dataset_id);
      if (!bounds.has_value()) {
        continue;
      }
      global_min = std::min(global_min, rememberDatasetMinTimestamp(dataset_id, bounds->first));
      found = true;
    }
    global_min_cache_ = found ? global_min : 0;
  }
  return *global_min_cache_;
}

std::optional<std::pair<Timestamp, Timestamp>> SessionManager::datasetRawBounds(DatasetId dataset_id) const {
  // The one scalar(DataEngine) + object(ObjectStore) time-bounds union. Shared by
  // datasetDisplayRange (needs both ends) and datasetMinTimestamp (min only).
  const DataReader reader = createReader();
  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  bool found = false;
  for (const TopicId topic_id : reader.listTopics(dataset_id)) {
    const auto metadata = reader.getMetadata(topic_id);
    if (metadata.has_value() && metadata->total_row_count > 0) {
      t_min = std::min(t_min, metadata->time_range_min);
      t_max = std::max(t_max, metadata->time_range_max);
      found = true;
    }
  }
  for (const ObjectTopicId object_topic_id : object_store_.listTopics(dataset_id)) {
    if (object_store_.entryCount(object_topic_id) > 0) {
      const auto [object_min, object_max] = object_store_.timeRange(object_topic_id);
      t_min = std::min(t_min, object_min);
      t_max = std::max(t_max, object_max);
      found = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  return std::pair{t_min, t_max};
}

Timestamp SessionManager::datasetMinTimestamp(DatasetId dataset_id) const {
  // Pinned earliest sample for the dataset's relative-time frame. It is cached
  // because displayOffset() is a hot path, but unlike a normal bounds cache it
  // intentionally does not move forward when streaming retention raises the
  // current readable minimum. That keeps the live edge advancing instead of
  // projecting every retained window back to 0..buffer_seconds.
  if (const auto it = dataset_min_cache_.find(dataset_id); it != dataset_min_cache_.end()) {
    return it->second;
  }
  const auto bounds = datasetRawBounds(dataset_id);
  if (!bounds) {
    return 0;
  }
  return rememberDatasetMinTimestamp(dataset_id, bounds->first);
}

Timestamp SessionManager::rememberDatasetMinTimestamp(DatasetId dataset_id, Timestamp observed_min) const {
  const auto [it, inserted] = dataset_min_cache_.emplace(dataset_id, observed_min);
  if (!inserted && observed_min < it->second) {
    it->second = observed_min;
  }
  return it->second;
}

void SessionManager::refreshDatasetMinTimestampsForTopics(const QVector<TopicId>& ids) const {
  // Refreshing per-dataset mins invalidates the across-datasets memo. Reset up
  // front so it still fires on the empty-but-live notify path (early-return below).
  global_min_cache_.reset();
  if (ids.isEmpty()) {
    return;
  }

  std::unordered_set<DatasetId> dataset_ids;
  {
    auto lock = data_engine_.lockEngine();
    dataset_ids.reserve(static_cast<std::size_t>(ids.size()));
    for (const TopicId id : ids) {
      if (const TopicStorage* storage = data_engine_.getTopicStorage(id)) {
        dataset_ids.insert(storage->descriptor().dataset_id);
      }
    }
  }

  for (const DatasetId dataset_id : dataset_ids) {
    if (const auto bounds = datasetRawBounds(dataset_id); bounds.has_value()) {
      (void)rememberDatasetMinTimestamp(dataset_id, bounds->first);
    } else {
      invalidateDatasetMinTimestamp(dataset_id);
    }
  }
}

void SessionManager::invalidateDatasetMinTimestamp(DatasetId dataset_id) const {
  dataset_min_cache_.erase(dataset_id);
  global_min_cache_.reset();
}

void SessionManager::setUseTimeOffset(bool use) {
  if (use_time_offset_ == use) {
    return;
  }
  use_time_offset_ = use;
  // Flip ONLY the global frame: displayOffset() now adds globalTimeReference()
  // (earliest raw sample across all datasets when on, 0 when off) on top of each
  // dataset's per-source alignment. No per-source TimeDomain offset is written, so
  // the Source Timeline's bar positions are untouched — only the numbers reframe.
  // No topic changed, only the display->raw mapping; tell every offset reader
  // (curve adapters, scenes, the playback seed, the Timeline) to re-resolve. This
  // global frame change uses the no-arg overload; the per-dataset overload is
  // reserved for single-source Timeline edits.
  emit displayOffsetChanged();
}

std::optional<DisplayRange> SessionManager::datasetDisplayRange(DatasetId dataset_id) const {
  const auto bounds = datasetRawBounds(dataset_id);
  if (!bounds) {
    return std::nullopt;
  }
  // The single raw-ns -> display-seconds crossing for the streaming range, via
  // the dataset's own offset — identical to the file-load seed (both origins
  // match) so a bare raw-ns range can never reach the playback axis.
  const DisplayOffset offset = displayOffset(dataset_id);
  return DisplayRange{
      .min = rawToDisplaySeconds(bounds->first, offset), .max = rawToDisplaySeconds(bounds->second, offset)};
}

void SessionManager::setDisplayOffset(DatasetId dataset_id, DisplayOffset offset) {
  // Mirror displayOffset()'s resolution: dataset -> its TimeDomain id. Writing
  // the domain (not the dataset snapshot) is what makes displayOffset() read it
  // back live; emit so consumers re-snap/re-map without re-indexing samples.
  const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
  if (dataset == nullptr || dataset->time_domain.id == 0) {
    qCWarning(lcSession) << "setDisplayOffset: unknown dataset or default domain" << dataset_id;
    return;
  }
  // Idempotent: an unchanged offset emits nothing, so a no-op write (resetAll over
  // already-zero datasets, a settled live drag re-sending the same value) doesn't
  // churn consumers — every displayOffsetChanged triggers a PlotWidget adapter
  // drop + replot and a timeline offset refresh.
  if (sourceDisplayOffset(dataset_id).value == offset.value) {
    return;
  }
  data_engine_.setDisplayOffset(dataset->time_domain.id, static_cast<Timestamp>(offset.value.count()));
  emit displayOffsetChanged(dataset_id);
}

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  auto changed = data_engine_.commitChunks(std::move(chunks));
  if (changed.empty()) {
    return changed;
  }
  // Run eager filters over the freshly committed input, then notify both the raw
  // and the derived output topics so filtered curves refresh. (Loaded files keep
  // full history, so there is no retention race on this path.)
  const std::vector<TopicId> derived_outputs =
      processor_service_ ? processor_service_->advanceOnCommit(changed) : std::vector<TopicId>{};

  QVector<TopicId> ids;
  ids.reserve(static_cast<qsizetype>(changed.size() + derived_outputs.size()));
  for (const TopicId id : changed) {
    ids.push_back(id);
  }
  for (const TopicId id : derived_outputs) {
    ids.push_back(id);
  }
  refreshDatasetMinTimestampsForTopics(ids);  // also resets global_min_cache_
  emit samplesIngested(std::move(ids), /*live=*/false);
  return changed;
}

void SessionManager::notifyIngest(QVector<TopicId> ids, bool live) {
  if (ids.isEmpty() && !live) {
    return;
  }
  refreshDatasetMinTimestampsForTopics(ids);  // also resets global_min_cache_
  emit samplesIngested(std::move(ids), live);
}

void SessionManager::notifyDatasetAboutToBeReplaced(DatasetId dataset_id) {
  invalidateDatasetMinTimestamp(dataset_id);
  emit datasetAboutToBeReplaced(dataset_id);
}

RefillGuard SessionManager::beginRefill(DatasetId dataset_id) {
  return RefillGuard(*this, dataset_id);  // guaranteed copy elision (move-only)
}

// --- RefillGuard: the transactional in-place reload (see SessionManager::beginRefill). ---

RefillGuard::RefillGuard(SessionManager& session, DatasetId dataset_id) : session_(&session), dataset_id_(dataset_id) {
  // GUI thread, no event loop — same contract as replaceDataset.
  Q_ASSERT(session.thread() == QThread::currentThread());

  // Capture the prior topic id sets BEFORE detaching: scalars for the empty-state
  // notify, object ids so rollback can tell which object topics a failed refill added.
  const std::vector<TopicId> scalar_topics = session.dataEngine().listTopics(dataset_id);
  prior_object_topic_ids_ = session.objectStore().listTopics(dataset_id);

  // (1) Adapters drop cached TopicChunk* before any deque is moved (same ordering
  //     as replaceDataset).
  session.notifyDatasetAboutToBeReplaced(dataset_id);
  // (2) scalar + (3) object: DETACH (move aside, not free), keeping ids registered
  //     so the progressive refill writes back into the same ids.
  scalar_snapshot_ = session.dataEngine().detachDatasetChunks(dataset_id);
  object_snapshot_ = session.objectStore().detachDataset(dataset_id);
  // (4) UI sees the dataset empty (non-live). no-ops on an empty id list.
  session.notifyIngest(QVector<TopicId>(scalar_topics.begin(), scalar_topics.end()), /*live=*/false);
}

RefillGuard::RefillGuard(RefillGuard&& other) noexcept
    : session_(other.session_),
      dataset_id_(other.dataset_id_),
      scalar_snapshot_(std::move(other.scalar_snapshot_)),
      object_snapshot_(std::move(other.object_snapshot_)),
      prior_object_topic_ids_(std::move(other.prior_object_topic_ids_)),
      committed_(other.committed_) {
  other.session_ = nullptr;  // the moved-from guard must not roll back
  other.committed_ = true;
}

RefillGuard& RefillGuard::operator=(RefillGuard&& other) noexcept {
  if (this != &other) {
    if (session_ != nullptr && !committed_) {
      rollback();  // discard the transaction this guard still owns before taking over
    }
    session_ = other.session_;
    dataset_id_ = other.dataset_id_;
    scalar_snapshot_ = std::move(other.scalar_snapshot_);
    object_snapshot_ = std::move(other.object_snapshot_);
    prior_object_topic_ids_ = std::move(other.prior_object_topic_ids_);
    committed_ = other.committed_;
    other.session_ = nullptr;
    other.committed_ = true;
  }
  return *this;
}

RefillGuard::~RefillGuard() {
  if (session_ != nullptr && !committed_) {
    rollback();
  }
}

void RefillGuard::commit() {
  committed_ = true;
  scalar_snapshot_ = {};  // free the held-aside prior data; the refilled data is kept
  object_snapshot_ = {};
  prior_object_topic_ids_.clear();
}

void RefillGuard::pruneVanishedTopics() {
  if (session_ == nullptr) {
    return;
  }
  // Scalar: a prior topic still empty after the refill vanished from the new file.
  // Collect under one engine lock (getTopicStorage needs it held), then retire —
  // retireTopic re-locks (recursive mutex) and clears its already-empty deque.
  std::vector<TopicId> vanished_scalar;
  {
    auto lock = session_->dataEngine().lockEngine();
    for (const TopicId topic_id : scalar_snapshot_.prior_topic_ids) {
      const TopicStorage* storage = session_->dataEngine().getTopicStorage(topic_id);
      if (storage != nullptr && storage->empty()) {
        vanished_scalar.push_back(topic_id);
      }
    }
  }
  for (const TopicId topic_id : vanished_scalar) {
    session_->dataEngine().retireTopic(topic_id);
  }
  // Object: a prior object topic with no entries after the refill vanished too.
  std::vector<ObjectTopicId> vanished_object;
  for (const ObjectTopicId object_topic_id : prior_object_topic_ids_) {
    if (session_->objectStore().entryCount(object_topic_id) == 0) {
      vanished_object.push_back(object_topic_id);
    }
  }
  if (!vanished_object.empty()) {
    session_->evictObjectTopics(vanished_object);  // drops the empty store series + its parser slot
  }
}

void RefillGuard::rollback() {
  // (a) Adapters drop any partial-refill chunk pointers cached via progress notifies.
  session_->notifyDatasetAboutToBeReplaced(dataset_id_);
  // (b) Scalar: drop partial refill chunks + retire topics it added, then move the
  //     prior chunks back into the stable ids.
  session_->dataEngine().reattachDatasetChunks(dataset_id_, std::move(scalar_snapshot_));
  // (c) Object: evict topics the refill ADDED (drops their store series + parser
  //     slots) BEFORE reattach, so reattach's own "current - prior" removal is a
  //     no-op. evictObjectTopics re-takes store_mutex_, so it must run OUTSIDE it —
  //     it does here (no ObjectStore lock held on this thread).
  std::unordered_set<uint32_t> prior;
  prior.reserve(prior_object_topic_ids_.size());
  for (const ObjectTopicId id : prior_object_topic_ids_) {
    prior.insert(id.id);
  }
  std::vector<ObjectTopicId> added;
  for (const ObjectTopicId id : session_->objectStore().listTopics(dataset_id_)) {
    if (prior.find(id.id) == prior.end()) {
      added.push_back(id);
    }
  }
  if (!added.empty()) {
    session_->evictObjectTopics(added);
  }
  // (d) Object: move the prior entries back into the stable ids.
  session_->objectStore().reattachDataset(dataset_id_, std::move(object_snapshot_));
  // (e) UI: reflect the restored topic set (non-live).
  const std::vector<TopicId> current = session_->dataEngine().listTopics(dataset_id_);
  session_->notifyIngest(QVector<TopicId>(current.begin(), current.end()), /*live=*/false);
}

std::size_t RefillGuard::snapshotBytes() const noexcept {
  std::size_t bytes = 0;
  // Scalar: approximate — rows * (1 timestamp column + N value columns) * 8 bytes/cell.
  for (const auto& [topic_id, topic] : scalar_snapshot_.topics) {
    (void)topic_id;
    for (const TopicChunk& chunk : topic.chunks) {
      const std::size_t cells = static_cast<std::size_t>(chunk.stats.row_count) * (1 + chunk.columns.size());
      bytes += cells * sizeof(double);
    }
  }
  // Object: exact — the store tracks per-series resident bytes.
  for (const auto& [raw_id, series] : object_snapshot_.series) {
    (void)raw_id;
    bytes += series.memory_bytes;
  }
  return bytes;
}

void SessionManager::replaceDataset(
    DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
    std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers) {
  // (1) Adapters drop cached TopicChunk* before any deque is touched. Same-thread
  // direct connection: every slot returns before the emit does, and this method
  // runs no event loop (the caller must not either) — so the pointers stay dead.
  notifyDatasetAboutToBeReplaced(primary_id);

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

  // (4) Recompute the filters whose input was just swapped: a reload replaces the
  // input chunks wholesale, so the derived output must be reset+replayed (not
  // appended). Notify those outputs too, so plots showing filtered curves refresh.
  if (processor_service_ && !changed.isEmpty()) {
    const std::vector<TopicId> outputs =
        processor_service_->recomputeForReplacedSources(std::vector<TopicId>(changed.begin(), changed.end()));
    for (const TopicId out : outputs) {
      changed.push_back(out);
    }
  }

  // (5) Re-index the (already-cleared) adapters against the swapped-in data.
  notifyIngest(std::move(changed), /*live=*/false);
}

std::optional<DatasetMergeReport> SessionManager::mergeDatasets(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // (1) Adapters bound to the anchor or any source drop cached TopicChunk* before
  // the engine clears/rebuilds chunks. Same-thread direct connections; this method
  // runs no event loop (the caller must not either), so the pointers stay dead.
  // The helper also invalidates the per-dataset/global min caches; object-only
  // merges may not notify any scalar topics later.
  notifyDatasetAboutToBeReplaced(anchor);
  for (const auto& source : sources) {
    notifyDatasetAboutToBeReplaced(source.dataset_id);
  }

  // (2) Fold the scalar data. The engine validates all caller/data-driven inputs
  // up front (unknown/duplicate/self source), so a reachable failure leaves the
  // engine untouched — log and return an empty report. (Its in-loop guards cover
  // only unreachable internal invariants.)
  DatasetMergeReport report;
  if (auto result = data_engine_.mergeDatasets(anchor, sources); result.has_value()) {
    report = std::move(*result);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets:" << QString::fromStdString(result.error());
    return std::nullopt;  // engine rejected: nothing mutated — let the caller skip the catalog update
  }

  // (3) Fold the object topics the same way. This shares the scalar merge's
  // structural validation, so it cannot fail once the scalar merge above
  // succeeded on the same inputs.
  if (auto objects = object_store_.mergeDatasets(anchor, sources); objects.has_value()) {
    // Shared-name source topics are now empty — their entries folded into the
    // anchor topic, which the anchor's own parser decodes. Evict those redundant
    // source-side topics AND their (now-orphaned) parser slots via evictObjectTopics
    // (which defers slot teardown past the lock, matching the reload path). Source-
    // only topics were reparented under the anchor and KEEP their ids + parsers, so
    // they are deliberately not evicted here.
    std::vector<ObjectTopicId> folded_sources;
    folded_sources.reserve(objects->remapped.size());
    for (const auto& [source_id, dest_id] : objects->remapped) {
      folded_sources.push_back(source_id);
    }
    evictObjectTopics(folded_sources);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets(objects):" << QString::fromStdString(objects.error());
  }

  // (4) Re-index adapters against the rebuilt anchor topics.
  QVector<TopicId> changed;
  changed.reserve(static_cast<int>(report.modified_topics.size() + report.added_topics.size()));
  for (const TopicId t : report.modified_topics) {
    changed.push_back(t);
  }
  for (const TopicId t : report.added_topics) {
    changed.push_back(t);
  }
  notifyIngest(std::move(changed), /*live=*/false);
  return report;
}

void SessionManager::registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
  const bool incoming_valid = parser != nullptr && parser->valid();
  // The old slot is moved out under the lock and destroyed AFTER it releases:
  // its dtor runs MessageParserHandle teardown (potentially dlclose), which must
  // never run while holding object_parsers_mutex_.
  ObjectParserSlot replaced_slot;
  {
    std::unique_lock lock(object_parsers_mutex_);
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
      qCWarning(lcSession) << "registerObjectTopicParser: invalid parser for topic" << id.id
                           << "— erasing registration";
      if (existing != object_topic_parsers_.end()) {
        replaced_slot = std::move(existing->second);
        object_topic_parsers_.erase(existing);
      }
      return;
    }
    // Fresh mutex per registration: a re-registration swaps in a new parser, but
    // existing consumers still guard the old one. Reusing the lock would let the
    // new caller race with leftover work on the old parser pointer.
    ObjectParserSlot fresh{std::shared_ptr<MessageParserHandle>(std::move(parser)), std::make_shared<std::mutex>()};
    auto& slot = object_topic_parsers_[id.id];
    replaced_slot = std::move(slot);  // keep the old handle off the lock-held dtor path
    slot = std::move(fresh);
  }
  // replaced_slot destructs here, after the lock is released.
}

const SessionManager::ObjectParserSlot* SessionManager::findValidParserSlotLocked(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second.handle == nullptr || !it->second.handle->valid()) {
    return nullptr;
  }
  return &it->second;
}

SessionManager::ParserBinding SessionManager::parserBindingForObjectTopic(ObjectTopicId id) const {
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  if (slot == nullptr) {
    return {};
  }
  // Copy the shared_ptrs out under the lock so the snapshot keeps the parser
  // alive even if the streaming worker replaces the slot right after we release.
  return ParserBinding{
      static_cast<MessageParserPluginBase*>(slot->handle->context()),
      slot->mutex,
      slot->handle,
  };
}

std::shared_ptr<void> SessionManager::parserKeepaliveForObjectTopic(ObjectTopicId id) const {
  // shared_ptr<MessageParserHandle> -> shared_ptr<void>: holding it keeps the
  // handle (and thus the parser instance + plugin DSO) alive for the consumer.
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? slot->handle : nullptr;
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  // Returns a RAW pointer with no keepalive: only safe for synchronous GUI-thread
  // use that does not outlive the call. Cross-thread / cached consumers must take
  // parserBindingForObjectTopic and hold its keepalive instead.
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? static_cast<MessageParserPluginBase*>(slot->handle->context()) : nullptr;
}

std::shared_ptr<std::mutex> SessionManager::parserMutexForObjectTopic(ObjectTopicId id) const {
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? slot->mutex : nullptr;
}

void SessionManager::recordLoadedSource(QString path, QString prefix, QString plugin_id, QString plugin_config_json) {
  LoadedSource source{std::move(path), std::move(prefix), std::move(plugin_id), std::move(plugin_config_json)};
  // Dedup by path: a reload of an already-tracked file updates its entry in
  // place (keeping list order) rather than appending a duplicate.
  const auto it = std::find_if(loaded_sources_.begin(), loaded_sources_.end(), [&source](const LoadedSource& existing) {
    return existing.path == source.path;
  });
  if (it != loaded_sources_.end()) {
    *it = std::move(source);
  } else {
    loaded_sources_.push_back(std::move(source));
  }
}

void SessionManager::evictDatasetObjects(DatasetId dataset_id) {
  evictObjectTopics(object_store_.listTopics(dataset_id));
}

void SessionManager::removeDataset(DatasetId dataset_id) {
  const Timestamp old_reference = globalTimeReference();
  data_engine_.removeDataset(dataset_id);
  // The engine no longer holds this dataset; drop its pinned earliest-sample and
  // the memoized cross-dataset origin so globalTimeReference() re-scans the
  // survivors (removing the earliest dataset must re-base the display origin).
  invalidateDatasetMinTimestamp(dataset_id);
  // If that re-base actually moved the origin, every surviving curve adapter now
  // holds a stale display offset — no per-topic samplesIngested covers a pure
  // origin shift, so signal the global reframe. No-op when "Use time offset" is
  // off (globalTimeReference() is 0 both times).
  if (globalTimeReference() != old_reference) {
    emit displayOffsetChanged();
  }
}

void SessionManager::evictObjectTopics(const std::vector<ObjectTopicId>& topic_ids) {
  // removeTopic touches the ObjectStore, not the parser map, so keep it out of
  // the parser lock. Erased slots are collected and destroyed after the lock
  // releases: a slot dtor may run plugin teardown (dlclose), which must not run
  // under object_parsers_mutex_.
  std::vector<ObjectParserSlot> erased_slots;
  erased_slots.reserve(topic_ids.size());
  for (const ObjectTopicId topic_id : topic_ids) {
    object_store_.removeTopic(topic_id);
    // Topic gone: drop its parser too, so a reload (fresh ObjectTopicId) re-registers cleanly.
    std::unique_lock lock(object_parsers_mutex_);
    if (const auto it = object_topic_parsers_.find(topic_id.id); it != object_topic_parsers_.end()) {
      erased_slots.push_back(std::move(it->second));
      object_topic_parsers_.erase(it);
    }
  }
  // erased_slots destructs here, after the last unlock.
}

void SessionManager::clearAllObjects() {
  object_store_.clear();
  // Swap the map into a local under the lock, then let it destruct after the
  // lock releases — slot dtors may run plugin teardown (dlclose).
  std::unordered_map<uint32_t, ObjectParserSlot> drained;
  {
    std::unique_lock lock(object_parsers_mutex_);
    drained.swap(object_topic_parsers_);
  }
  // drained destructs here, after the lock is released.
}

}  // namespace PJ
