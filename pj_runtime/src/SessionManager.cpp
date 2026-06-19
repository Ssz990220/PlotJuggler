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

DisplayOffset SessionManager::displayOffset(DatasetId dataset_id) const {
  // Base shift from the dataset's TimeDomain. Live lookup via the time-domain
  // map: the dataset's own time_domain is a snapshot from createDataset, so
  // reading its display_offset directly would go stale after setDisplayOffset.
  // (Latent today — no production caller of setDisplayOffset.)
  Timestamp offset_ns = 0;
  if (const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
      dataset != nullptr && dataset->time_domain.id != 0) {
    if (const TimeDomain* domain = data_engine_.getTimeDomain(dataset->time_domain.id)) {
      offset_ns = domain->display_offset;
    }
  }
  // "Use time offset" layers a per-dataset shift on top: the dataset's OWN
  // earliest sample, so its axis starts near zero. Computed live so it tracks
  // loads; this is the single seam for future fine-tuned alignment.
  if (use_time_offset_) {
    offset_ns += datasetMinTimestamp(dataset_id);
  }
  return DisplayOffset{Duration{offset_ns}};
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
  // Memoized: the earliest sample is near-static (only an earlier-stamped ingest
  // moves it), yet displayOffset() reads it on hot paths — per playback tick from
  // scene docks and once per catalog item from seedPlaybackFromSession. The cache
  // is cleared on every commit/ingest, so a stale min can't outlive a data change
  // that could lower it. Empty datasets aren't cached, so the first real sample
  // recomputes.
  if (const auto it = dataset_min_cache_.find(dataset_id); it != dataset_min_cache_.end()) {
    return it->second;
  }
  const auto bounds = datasetRawBounds(dataset_id);
  if (!bounds) {
    return 0;
  }
  dataset_min_cache_.emplace(dataset_id, bounds->first);
  return bounds->first;
}

void SessionManager::setUseTimeOffset(bool use) {
  if (use_time_offset_ == use) {
    return;
  }
  use_time_offset_ = use;
  // The per-dataset shifts follow automatically in displayOffset(); tell every
  // offset reader (curve adapters, scenes, the playback seed) to re-resolve.
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

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  auto changed = data_engine_.commitChunks(std::move(chunks));
  if (changed.empty()) {
    return changed;
  }
  dataset_min_cache_.clear();  // new samples may lower a dataset's earliest stamp
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
  emit samplesIngested(std::move(ids), /*live=*/false);
  return changed;
}

void SessionManager::notifyIngest(QVector<TopicId> ids, bool live) {
  if (ids.isEmpty()) {
    return;
  }
  dataset_min_cache_.clear();  // direct-write ingest (incl. objects, reload) may move the earliest stamp
  emit samplesIngested(std::move(ids), live);
}

void SessionManager::notifyDatasetAboutToBeReplaced(DatasetId dataset_id) {
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
