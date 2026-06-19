// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/engine.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/expected.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {

struct DataEngine::Impl {
  TypeRegistry type_registry;
  PJ::DatasetId next_dataset_id = 1;
  PJ::TopicId next_topic_id = 1;
  PJ::TimeDomainId next_time_domain_id = 1;
  tsl::robin_map<PJ::DatasetId, PJ::DatasetInfo> datasets;
  // unique_ptr so a rehash on insert (createTopic) moves only 8-byte pointers,
  // never a TopicStorage or its sealed_chunks_ deque — addresses stay stable for
  // the engine's lifetime (topics are never erased, only retired). This is what
  // lets a worker-thread append be safe for a GUI-cached TopicChunk* with the
  // engine lock alone (no drain). See docs/plans/datastore-thread-safety.md (D3).
  tsl::robin_map<PJ::TopicId, std::unique_ptr<TopicStorage>> topics;
  tsl::robin_map<PJ::TimeDomainId, PJ::TimeDomain> time_domains;
  // Topics retired by replaceDatasetFrom(): their storage is kept (so any
  // cached reader pointer dereferences an empty deque, not freed memory) but
  // they are hidden from listTopics so the catalog drops them.
  std::unordered_set<PJ::TopicId> retired_topic_ids;

  // Guards every field above. One recursive exclusive lock for all engine clients
  // (worker ingest + GUI reads/recompute); DataReader cursors hold it for their
  // lifetime. Recursive so the same thread may nest acquisitions (a held cursor +
  // another read, a compound mutator calling another). mutable so a const reader
  // can lock. See engine.hpp lockEngine() for the full contract.
  mutable std::recursive_mutex mutex_;
};

DataEngine::DataEngine() : impl_(std::make_unique<Impl>()) {}

DataEngine::~DataEngine() = default;

DataEngine::DataEngine(DataEngine&&) noexcept = default;

DataEngine& DataEngine::operator=(DataEngine&&) noexcept = default;

std::unique_lock<std::recursive_mutex> DataEngine::lockEngine() const {
  return std::unique_lock<std::recursive_mutex>(impl_->mutex_);
}

std::unique_lock<std::recursive_mutex> DataEngine::lockEngineDeferred() const {
  return std::unique_lock<std::recursive_mutex>(impl_->mutex_, std::defer_lock);
}

// ---------------------------------------------------------------------------
// Dataset management
// ---------------------------------------------------------------------------

Expected<DatasetId> DataEngine::createDataset(DatasetDescriptor descriptor, DatasetId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  DatasetId id = requested_id != 0 ? requested_id : impl_->next_dataset_id;
  if (requested_id != 0) {
    if (impl_->datasets.find(requested_id) != impl_->datasets.end()) {
      return PJ::unexpected(fmt::format("Dataset {} already exists", requested_id));
    }
  }

  // Verify time domain exists if specified
  if (descriptor.time_domain_id != 0) {
    auto it = impl_->time_domains.find(descriptor.time_domain_id);
    if (it == impl_->time_domains.end()) {
      return PJ::unexpected(fmt::format("Time domain {} not found", descriptor.time_domain_id));
    }
  }
  if (requested_id != 0) {
    if (impl_->next_dataset_id <= id) {
      impl_->next_dataset_id = id + 1;
    }
  } else {
    ++impl_->next_dataset_id;
  }

  DatasetInfo info;
  info.id = id;
  info.source_name = std::move(descriptor.source_name);
  if (descriptor.time_domain_id != 0) {
    info.time_domain = impl_->time_domains.at(descriptor.time_domain_id);
  }
  impl_->datasets.emplace(id, std::move(info));
  return id;
}

const DatasetInfo* DataEngine::getDataset(DatasetId id) const {
  auto it = impl_->datasets.find(id);
  if (it == impl_->datasets.end()) {
    return nullptr;
  }
  return &it->second;
}

// ---------------------------------------------------------------------------
// Topic management
// ---------------------------------------------------------------------------

Expected<TopicId> DataEngine::createTopic(DatasetId dataset_id, TopicDescriptor descriptor, TopicId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return createTopicLocked(dataset_id, std::move(descriptor), requested_id);
}

Expected<TopicId> DataEngine::createTopicLocked(
    DatasetId dataset_id, TopicDescriptor descriptor, TopicId requested_id) {
  auto it = impl_->datasets.find(dataset_id);
  if (it == impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("Dataset {} not found", dataset_id));
  }

  // Validate schema_id if non-zero (zero means inline columns, e.g. scalar series)
  if (descriptor.schema_id != 0) {
    if (impl_->type_registry.lookup(descriptor.schema_id) == nullptr) {
      return PJ::unexpected(fmt::format("Schema {} not found", descriptor.schema_id));
    }
  }

  TopicId id = requested_id != 0 ? requested_id : impl_->next_topic_id;
  if (requested_id != 0) {
    if (impl_->topics.find(requested_id) != impl_->topics.end()) {
      return PJ::unexpected(fmt::format("Topic {} already exists", requested_id));
    }
    if (impl_->next_topic_id <= id) {
      impl_->next_topic_id = id + 1;
    }
  } else {
    ++impl_->next_topic_id;
  }
  descriptor.dataset_id = dataset_id;
  impl_->topics.emplace(id, std::make_unique<TopicStorage>(id, std::move(descriptor)));
  it.value().topic_ids.push_back(id);  // `it` is the datasets iterator (DatasetInfo)
  return id;
}

Expected<FieldId> DataEngine::createTopicField(
    TopicId topic_id, std::string_view field_name, PrimitiveType type, std::optional<FieldId> requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto topic_it = impl_->topics.find(topic_id);
  if (topic_it == impl_->topics.end()) {
    return PJ::unexpected(fmt::format("createTopicField: topic {} not found", topic_id));
  }
  TopicStorage& storage = *topic_it.value();

  // Idempotent re-mirror: if a column with this name already exists with the
  // same type, return its id. With a non-matching type, fail loudly — the
  // caller is racing two registrations with different shapes.
  const auto& cols = storage.columnDescriptors();
  for (const auto& col : cols) {
    if (col.field_path == field_name) {
      if (col.logical_type != type) {
        return PJ::unexpected(
            fmt::format("createTopicField: field '{}' already exists with a different type", field_name));
      }
      if (requested_id.has_value() && *requested_id != col.field_id) {
        return PJ::unexpected(
            fmt::format(
                "createTopicField: field '{}' already exists with id {} but requested {}", field_name, col.field_id,
                *requested_id));
      }
      return col.field_id;
    }
  }

  // FieldIds are dense starting at 0. The next id is cols.size(). A forced
  // requested_id of 0 is a legitimate value (first field of the topic), which
  // is exactly why the sentinel for "auto" is std::nullopt, not 0.
  const FieldId next_id = static_cast<FieldId>(cols.size());
  if (requested_id.has_value() && *requested_id != next_id) {
    return PJ::unexpected(
        fmt::format(
            "createTopicField: requested_id {} would create a non-dense field layout "
            "(topic {} currently has {} columns; next dense id is {})",
            *requested_id, topic_id, cols.size(), next_id));
  }

  std::vector<ColumnDescriptor> new_cols = cols;
  ColumnDescriptor desc;
  desc.field_id = next_id;
  desc.logical_type = type;
  desc.field_path = std::string(field_name);
  new_cols.push_back(std::move(desc));
  storage.setColumnDescriptors(std::move(new_cols));
  return next_id;
}

TopicStorage* DataEngine::getTopicStorage(TopicId id) {
  auto it = impl_->topics.find(id);
  if (it == impl_->topics.end()) {
    return nullptr;
  }
  return it.value().get();
}

const TopicStorage* DataEngine::getTopicStorage(TopicId id) const {
  auto it = impl_->topics.find(id);
  if (it == impl_->topics.end()) {
    return nullptr;
  }
  return it->second.get();
}

// ---------------------------------------------------------------------------
// Schema registry
// ---------------------------------------------------------------------------

TypeRegistry& DataEngine::typeRegistry() {
  return impl_->type_registry;
}

const TypeRegistry& DataEngine::typeRegistry() const {
  return impl_->type_registry;
}

// ---------------------------------------------------------------------------
// Time domains
// ---------------------------------------------------------------------------

Expected<TimeDomainId> DataEngine::createTimeDomain(std::string name, TimeDomainId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  TimeDomainId id = requested_id != 0 ? requested_id : impl_->next_time_domain_id;
  if (requested_id != 0) {
    if (impl_->time_domains.find(requested_id) != impl_->time_domains.end()) {
      return PJ::unexpected(fmt::format("Time domain {} already exists", requested_id));
    }
    if (impl_->next_time_domain_id <= id) {
      impl_->next_time_domain_id = id + 1;
    }
  } else {
    ++impl_->next_time_domain_id;
  }
  TimeDomain td;
  td.id = id;
  td.name = std::move(name);
  impl_->time_domains.emplace(id, std::move(td));
  return id;
}

const TimeDomain* DataEngine::getTimeDomain(TimeDomainId id) const {
  auto it = impl_->time_domains.find(id);
  if (it == impl_->time_domains.end()) {
    return nullptr;
  }
  return &it->second;
}

void DataEngine::setDisplayOffset(TimeDomainId id, Timestamp offset) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto it = impl_->time_domains.find(id);
  if (it != impl_->time_domains.end()) {
    it.value().display_offset = offset;
  }
}

// ---------------------------------------------------------------------------
// Commit cycle
// ---------------------------------------------------------------------------

std::vector<TopicId> DataEngine::commitChunks(
    std::vector<std::pair<TopicId, TopicChunk>> chunks) {  // NOLINT(performance-unnecessary-value-param)
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return commitChunksLocked(std::move(chunks));
}

std::vector<TopicId> DataEngine::commitChunksLocked(
    std::vector<std::pair<TopicId, TopicChunk>> chunks) {  // NOLINT(performance-unnecessary-value-param)
  std::vector<TopicId> changed;
  for (auto& [topic_id, chunk] : chunks) {
    auto* storage = getTopicStorage(topic_id);
    if (storage != nullptr) {
      auto status = storage->appendSealedChunk(std::move(chunk));
      if (!status.has_value()) {
        continue;  // chunk rejected (e.g. out-of-order); do not mark topic as changed
      }
      // A topic that receives real data is no longer "absent": un-retire it so a
      // recomputed filter output (retired by a reload's replaceDatasetFrom) reappears in
      // listTopics()/the catalog once its fresh chunks land. Mirrors the un-retire-on-
      // readopt at replaceDatasetFrom.
      impl_->retired_topic_ids.erase(topic_id);
      if (changed.empty() || changed.back() != topic_id) {
        changed.push_back(topic_id);
      }
    }
  }
  // Deduplicate (flushAll() may emit multiple chunks for one topic).
  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return changed;
}

void DataEngine::enforceRetention(Timestamp retention_window_ns) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& storage = *it.value();
    if (!storage.empty()) {
      Timestamp t_max = storage.timeMax();
      storage.evictBefore(t_max - retention_window_ns);
    }
  }
}

void DataEngine::enforceRetention(Timestamp retention_window_ns, DatasetId dataset_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& storage = *it.value();
    if (storage.descriptor().dataset_id != dataset_id || storage.empty()) {
      continue;
    }
    Timestamp t_max = storage.timeMax();
    storage.evictBefore(t_max - retention_window_ns);
  }
}

void DataEngine::retireTopic(TopicId topic_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  // Mirror replaceDatasetFrom's retire: exclude the id from listTopics (the catalog
  // drops it on the next rebuild) but keep its TopicStorage object so a cached reader
  // pointer dereferences an empty deque, never freed memory.
  auto it = impl_->topics.find(topic_id);
  if (it == impl_->topics.end()) {
    return;  // unknown id — nothing to retire
  }
  // Reclaim the materialized chunks now. A re-applied filter mints a fresh TopicId, so a
  // retired derived output's storage can never be reused — keeping its series resident
  // would leak across undo/redo + layout-load cycles. The TopicStorage object stays (only
  // the deque is emptied); adapters re-create a fresh reader per access, so no live
  // TopicChunk* survives the clear (the same precondition replaceDatasetFrom relies on).
  it.value()->clearChunks();
  impl_->retired_topic_ids.insert(topic_id);
}

void DataEngine::removeDataset(DatasetId dataset_id) {
  // Hold the engine lock for the whole erase, like every other mutator — a worker may
  // be ingesting a different dataset concurrently (GUI-thread caller; recursive_mutex).
  auto lock = lockEngine();
  auto ds_it = impl_->datasets.find(dataset_id);
  if (ds_it == impl_->datasets.end()) {
    return;  // unknown id — nothing to remove
  }
  // REAL delete (vs retireTopic's hide-but-keep): free every topic's TopicStorage.
  // PRECONDITION: the caller invalidated all readers/adapters bound to this dataset
  // first (same contract as replaceDatasetFrom) — erasing frees the memory a cached
  // reader / TopicChunk* would otherwise dereference.
  for (const TopicId tid : ds_it->second.topic_ids) {
    impl_->topics.erase(tid);
    impl_->retired_topic_ids.erase(tid);  // drop retire bookkeeping for the now-gone topic
  }
  impl_->datasets.erase(ds_it);
  // next_*_id are NOT rewound: strictly-incrementing ids guarantee a later dataset /
  // topic never aliases an erased one. time_domains are left intact (shared + cheap).
}

void DataEngine::clearDatasetChunks(DatasetId dataset_id) {
  // Like retireTopic, reclaim each topic's materialized chunks — but WITHOUT
  // adding the ids to retired_topic_ids, so they stay in listTopics() and a refill
  // writes back into them. For each live topic, clearChunks() empties the deque and
  // resets the eviction floor + cached min/max + row count, so a DataReader reports
  // an empty range immediately. Unknown dataset_id -> no-op; clearing an empty topic
  // is a no-op (idempotent).
  //
  // clearChunks() FREES the deque, so hold the engine lock across the whole sweep
  // (chunk-freeing mutation: GUI-thread-only, callers drop cached TopicChunk* first
  // — same contract as retireTopic/enforceRetention).
  auto lock = lockEngine();
  for (const TopicId topic_id : listTopicsLocked(dataset_id)) {
    if (TopicStorage* storage = getTopicStorage(topic_id)) {
      storage->clearChunks();
    }
  }
}

DataEngine::DatasetChunkSnapshot DataEngine::detachDatasetChunks(DatasetId dataset_id) {
  auto lock = lockEngine();
  DatasetChunkSnapshot snapshot;
  snapshot.dataset_id = dataset_id;
  snapshot.prior_topic_ids = listTopicsLocked(dataset_id);
  if (snapshot.prior_topic_ids.empty()) {
    return snapshot;  // unknown / empty dataset: `valid` stays false
  }
  for (const TopicId topic_id : snapshot.prior_topic_ids) {
    TopicStorage* storage = getTopicStorage(topic_id);
    if (storage == nullptr) {
      continue;
    }
    DatasetChunkSnapshot::TopicSnapshot topic_snapshot;
    // O(1): steal the chunk deque (column buffers/value arrays are pointer moves);
    // copy the small metadata the refill can mutate so reattach restores it exactly.
    topic_snapshot.chunks = std::move(storage->sealed_chunks_);
    topic_snapshot.column_descriptors = storage->column_descriptors_;
    topic_snapshot.retention_floor = storage->retention_floor_;
    topic_snapshot.max_observed_array_length = storage->max_observed_array_length_;
    topic_snapshot.truncated_sample_count = storage->truncated_sample_count_;
    topic_snapshot.array_expansion_counts = storage->array_expansion_counts_;
    // Normalize the now-empty storage exactly like clearDatasetChunks (clears the
    // moved-from deque + resets the floor); the column layout + ratchet stats stay
    // in place, matching clearDatasetChunks so a refill rebinds the same topics.
    storage->clearChunks();
    snapshot.topics.emplace(topic_id, std::move(topic_snapshot));
  }
  snapshot.valid = true;
  return snapshot;
}

void DataEngine::reattachDatasetChunks(DatasetId dataset_id, DatasetChunkSnapshot&& snapshot) {
  auto lock = lockEngine();
  if (!snapshot.valid) {
    return;
  }
  const std::unordered_set<TopicId> prior(snapshot.prior_topic_ids.begin(), snapshot.prior_topic_ids.end());
  // Reconcile the current topic set against the prior one: a topic the failed
  // refill added (not in `prior`) is retired; a prior topic's partial-refill
  // chunks are dropped before its prior data is moved back below. listTopicsLocked
  // returns a snapshot vector, so retiring mid-loop is safe.
  for (const TopicId topic_id : listTopicsLocked(dataset_id)) {
    if (prior.find(topic_id) == prior.end()) {
      retireTopic(topic_id);  // clears its chunks + excludes the id from listTopics
    } else if (TopicStorage* storage = getTopicStorage(topic_id)) {
      storage->clearChunks();
    }
  }
  // Move the prior data + refill-mutable metadata back into the stable ids. Direct
  // member assignment (not the public ratchet setters) so a smaller prior value is
  // truly restored.
  for (auto& [topic_id, topic_snapshot] : snapshot.topics) {
    TopicStorage* storage = getTopicStorage(topic_id);
    if (storage == nullptr) {
      continue;  // defensive: a prior topic vanished (should not happen)
    }
    storage->sealed_chunks_ = std::move(topic_snapshot.chunks);
    storage->column_descriptors_ = std::move(topic_snapshot.column_descriptors);
    storage->retention_floor_ = topic_snapshot.retention_floor;
    storage->max_observed_array_length_ = topic_snapshot.max_observed_array_length;
    storage->truncated_sample_count_ = topic_snapshot.truncated_sample_count;
    storage->array_expansion_counts_ = std::move(topic_snapshot.array_expansion_counts);
  }
  snapshot.valid = false;
}

Status DataEngine::flushTo(DataEngine& dst) {
  if (&dst == this) {
    return PJ::unexpected("flushTo: source and destination are the same engine");
  }
  // Lock both engines for the whole operation; std::lock's deadlock-avoidance
  // algorithm makes the acquisition order irrelevant (no ABBA).
  std::unique_lock<std::recursive_mutex> lock_this(impl_->mutex_, std::defer_lock);
  std::unique_lock<std::recursive_mutex> lock_dst(dst.impl_->mutex_, std::defer_lock);
  std::lock(lock_this, lock_dst);

  // Phase 1: validate. Walk every src topic with sealed chunks and look up
  // the matching dst topic by descriptor (dataset_id + name). Verify
  // monotonicity against dst's current time_max. No mutation yet.
  struct Step {
    TopicStorage* src;
    TopicStorage* dst;
  };
  std::vector<Step> plan;
  plan.reserve(impl_->topics.size());

  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& src_storage = *it.value();
    if (src_storage.empty()) {
      continue;
    }
    TopicStorage* dst_storage = nullptr;
    for (auto dst_it = dst.impl_->topics.begin(); dst_it != dst.impl_->topics.end(); ++dst_it) {
      auto& candidate = *dst_it.value();
      if (candidate.descriptor().dataset_id == src_storage.descriptor().dataset_id &&
          candidate.descriptor().name == src_storage.descriptor().name) {
        dst_storage = &candidate;
        break;
      }
    }
    if (dst_storage == nullptr) {
      return PJ::unexpected(
          "flushTo: destination has no topic '" + src_storage.descriptor().name + "' for dataset " +
          std::to_string(src_storage.descriptor().dataset_id));
    }
    // The source is a staging engine that is never retention-evicted, so its
    // timeMin() is the physical minimum (no retention-floor clamp in play). The
    // flushTo contract assumes the source carries no floor; if it ever did, the
    // clamped timeMin() could let physically-older source rows past this check.
    if (!dst_storage->empty() && src_storage.timeMin() < dst_storage->timeMax()) {
      return PJ::unexpected("flushTo: monotonicity violation for topic '" + src_storage.descriptor().name + "'");
    }
    plan.push_back({&src_storage, dst_storage});
  }

  // Phase 2: execute. adoptChunksFrom moves sealed_chunks_ directly between
  // TopicStorage instances (no column/value copy); the chunks' stats ride along
  // by value, so dst's time_min/time_max reflect the new state immediately. The
  // topic-id rewrite is a no-op here because flushTo's mirrored dst shares the
  // source's TopicId.
  for (auto& step : plan) {
    adoptChunksFrom(*step.dst, *step.src);
  }

  return {};
}

void DataEngine::adoptChunksFrom(TopicStorage& dst, TopicStorage& src) {
  // friend access: drain src's deque into dst, re-stamping each chunk's topic_id
  // to dst's id (a no-op when the two storages already share one, e.g. flushTo).
  // Appends — callers that need replace semantics clear dst first.
  std::deque<TopicChunk> drained = std::move(src.sealed_chunks_);
  src.sealed_chunks_.clear();  // post-move state: deque is valid but empty.
  const TopicId dst_id = dst.topicId();
  for (auto& chunk : drained) {
    chunk.topic_id = dst_id;
    dst.sealed_chunks_.push_back(std::move(chunk));
  }
}

Expected<DatasetReplaceResult> DataEngine::replaceDatasetFrom(
    DataEngine& staged, DatasetId staged_id, DatasetId primary_id) {
  if (&staged == this) {
    return PJ::unexpected("replaceDatasetFrom: staged and primary are the same engine");
  }
  // Lock BOTH engines for the whole operation in one std::lock — never hold one
  // and then plain-lock the other, or it ABBA-deadlocks against a worker write
  // host mirroring to its staging engine. Internal calls below use the *Locked
  // variants (createTopicLocked / listTopicsLocked) to avoid a redundant re-lock
  // (the recursive mutex would tolerate it; this just skips the churn).
  std::unique_lock<std::recursive_mutex> lock_this(impl_->mutex_, std::defer_lock);
  std::unique_lock<std::recursive_mutex> lock_staged(staged.impl_->mutex_, std::defer_lock);
  std::lock(lock_this, lock_staged);

  auto primary_it = impl_->datasets.find(primary_id);
  if (primary_it == impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("replaceDatasetFrom: primary dataset {} not found", primary_id));
  }
  if (staged.impl_->datasets.find(staged_id) == staged.impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("replaceDatasetFrom: staged dataset {} not found", staged_id));
  }

  // Snapshot primary topic name -> primary TopicId (includes ids retired by a
  // previous replace, so a topic that comes back re-binds to its stable id).
  std::unordered_map<std::string, TopicId> primary_by_name;
  for (const TopicId tid : primary_it->second.topic_ids) {
    if (const auto* storage = getTopicStorage(tid)) {
      primary_by_name.emplace(storage->descriptor().name, tid);
    }
  }

  DatasetReplaceResult result;
  std::unordered_set<std::string> staged_names;

  // Adopt every staged topic into the primary dataset, by name.
  for (const TopicId staged_tid : staged.listTopicsLocked(staged_id)) {
    TopicStorage* staged_storage = staged.getTopicStorage(staged_tid);
    if (staged_storage == nullptr) {
      continue;
    }
    const std::string& name = staged_storage->descriptor().name;
    staged_names.insert(name);

    const auto match = primary_by_name.find(name);
    TopicId primary_tid = 0;
    if (match == primary_by_name.end()) {
      // Schema-less by design: post-replace reads resolve columns from the moved
      // chunks' own descriptors (and the copied inline layout below), so a new
      // topic needs no registry schema (which would not exist in this engine).
      TopicDescriptor desc = staged_storage->descriptor();
      desc.dataset_id = primary_id;
      desc.schema_id = 0;
      auto created = createTopicLocked(primary_id, std::move(desc));
      if (!created.has_value()) {
        return PJ::unexpected("replaceDatasetFrom: createTopic failed for '" + name + "': " + created.error());
      }
      primary_tid = *created;
      result.added_topics.push_back(primary_tid);
    } else {
      primary_tid = match->second;
      impl_->retired_topic_ids.erase(primary_tid);  // un-retire if it had vanished
      result.replaced_topics.push_back(primary_tid);
    }

    // Fetch the primary storage now the topic is guaranteed to exist. With
    // unique_ptr<TopicStorage>, a createTopic rehash moves only pointers, so this
    // address stays valid for the rest of the function.
    TopicStorage* primary_storage = getTopicStorage(primary_tid);
    primary_storage->clearChunks();
    // Adopt the staged topic's inline layout (move, not copy — the staged engine
    // is discarded next) then re-stamp + move its chunks onto the primary id.
    primary_storage->setColumnDescriptors(std::move(staged_storage->column_descriptors_));
    adoptChunksFrom(*primary_storage, *staged_storage);
  }

  // Retire primary topics the new data no longer provides.
  for (const auto& [name, primary_tid] : primary_by_name) {
    if (staged_names.count(name) == 0 && impl_->retired_topic_ids.count(primary_tid) == 0) {
      if (auto* storage = getTopicStorage(primary_tid)) {
        storage->clearChunks();
      }
      impl_->retired_topic_ids.insert(primary_tid);
      result.retired_topics.push_back(primary_tid);
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Listing helpers
// ---------------------------------------------------------------------------

std::vector<DatasetId> DataEngine::listDatasets() const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  std::vector<DatasetId> result;
  result.reserve(impl_->datasets.size());
  for (const auto& [id, info] : impl_->datasets) {
    result.push_back(id);
  }
  return result;
}

std::vector<TopicId> DataEngine::listTopics(DatasetId dataset_id) const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return listTopicsLocked(dataset_id);
}

std::vector<TopicId> DataEngine::listTopicsLocked(DatasetId dataset_id) const {
  auto it = impl_->datasets.find(dataset_id);
  if (it == impl_->datasets.end()) {
    return {};
  }
  if (impl_->retired_topic_ids.empty()) {
    return it->second.topic_ids;
  }
  std::vector<TopicId> result;
  result.reserve(it->second.topic_ids.size());
  for (const TopicId tid : it->second.topic_ids) {
    if (impl_->retired_topic_ids.count(tid) == 0) {
      result.push_back(tid);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Writer/Reader factories
// ---------------------------------------------------------------------------

DataWriter DataEngine::createWriter() {
  return DataWriter(*this);
}

DataReader DataEngine::createReader() const {
  return DataReader(*this);
}

}  // namespace PJ
