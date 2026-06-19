// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/object_store.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace PJ {

// --- Registration ---

Expected<ObjectTopicId> ObjectStore::registerTopic(
    const ObjectTopicDescriptor& descriptor, ObjectTopicId requested_id) {
  std::unique_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.topic_name == descriptor.topic_name &&
        series->descriptor.dataset_id == descriptor.dataset_id) {
      return unexpected("topic already registered: " + descriptor.topic_name);
    }
    if (requested_id.id != 0 && tid.id == requested_id.id) {
      return unexpected("object topic id already in use: " + std::to_string(requested_id.id));
    }
  }
  ObjectTopicId id;
  if (requested_id.id != 0) {
    id = requested_id;
    if (next_id_ <= id.id) {
      next_id_ = id.id + 1;
    }
  } else {
    id = ObjectTopicId{next_id_++};
  }
  auto series = std::make_unique<ObjectSeries>();
  series->descriptor = descriptor;
  topics_.emplace_back(id, std::move(series));
  series_index_.emplace(id.id, topics_.back().second.get());
  return id;
}

std::optional<ObjectTopicId> ObjectStore::findTopic(DatasetId dataset_id, std::string_view topic_name) const {
  std::shared_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id && series->descriptor.topic_name == topic_name) {
      return tid;
    }
  }
  return std::nullopt;
}

ObjectTopicDescriptor ObjectStore::descriptor(ObjectTopicId id) const {
  std::shared_lock lock(store_mutex_);
  const auto* s = findSeries(id);
  if (s == nullptr) {
    return {};
  }
  return s->descriptor;  // copy under the lock — no reference escapes into the caller
}

std::vector<ObjectTopicId> ObjectStore::listTopics() const {
  std::shared_lock lock(store_mutex_);
  std::vector<ObjectTopicId> result;
  result.reserve(topics_.size());
  for (const auto& [tid, _] : topics_) {
    result.push_back(tid);
  }
  return result;
}

std::vector<ObjectTopicId> ObjectStore::listTopics(DatasetId dataset_id) const {
  std::shared_lock lock(store_mutex_);
  std::vector<ObjectTopicId> result;
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      result.push_back(tid);
    }
  }
  return result;
}

// --- Write ---

Status ObjectStore::pushOwned(ObjectTopicId id, Timestamp timestamp, std::vector<uint8_t> payload) {
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return unexpected("unknown topic");
  }

  std::unique_lock lock(series->mutex);
  if (!series->entry_timestamps.empty() && timestamp < series->entry_timestamps.back()) {
    return unexpected("timestamp not monotonically non-decreasing");
  }

  const size_t payload_size = payload.size();

  auto shared_data = std::make_shared<const std::vector<uint8_t>>(std::move(payload));

  ObjectEntry entry;
  entry.timestamp = timestamp;
  entry.sequential_uid = SequentialUID::getNext();
  entry.payload = std::move(shared_data);
  series->entries.push_back(std::move(entry));
  series->entry_timestamps.push_back(timestamp);
  series->memory_bytes += payload_size;

  applyRetention(*series, timestamp);
  return {};
}

Status ObjectStore::pushLazy(ObjectTopicId id, Timestamp timestamp, LazyCallback fetch) {
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return unexpected("unknown topic");
  }

  std::unique_lock lock(series->mutex);
  if (!series->entry_timestamps.empty() && timestamp < series->entry_timestamps.back()) {
    return unexpected("timestamp not monotonically non-decreasing");
  }

  ObjectEntry entry;
  entry.timestamp = timestamp;
  entry.sequential_uid = SequentialUID::getNext();
  entry.payload = std::move(fetch);
  series->entries.push_back(std::move(entry));
  series->entry_timestamps.push_back(timestamp);

  applyRetention(*series, timestamp);
  return {};
}

// --- Read ---

std::optional<ResolvedObjectEntry> ObjectStore::latestAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  if (series->entry_timestamps.empty()) {
    return std::nullopt;
  }

  auto it = std::upper_bound(series->entry_timestamps.begin(), series->entry_timestamps.end(), timestamp);
  if (it == series->entry_timestamps.begin()) {
    return std::nullopt;
  }
  --it;
  auto idx = static_cast<size_t>(it - series->entry_timestamps.begin());
  const ObjectEntry& entry = series->entries[idx];

  // Warm cache: a ~60 Hz reader landing on the same sample as last time is served
  // without re-invoking the (possibly decompressing/file-backed) lazy fetcher.
  // Keyed by sequential_uid, which is never reused, so a hit is always the exact
  // same entry. The lock is released before resolveEntry() so a slow decode on a
  // miss never blocks concurrent readers of this series.
  {
    std::lock_guard cache_guard(series->cache_mutex);
    if (series->cached_latest && series->cached_latest->sequential_uid == entry.sequential_uid) {
      return *series->cached_latest;
    }
  }
  ResolvedObjectEntry resolved = resolveEntry(entry);
  // Don't memoize a failed/empty resolve — let the next read retry instead of
  // latching the failure.
  if (!resolved.payload.bytes.empty()) {
    std::lock_guard cache_guard(series->cache_mutex);
    series->cached_latest = resolved;
  }
  return resolved;
}

std::optional<ResolvedObjectEntry> ObjectStore::at(ObjectTopicId id, size_t index) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  if (index >= series->entries.size()) {
    return std::nullopt;
  }
  return resolveEntry(series->entries[index]);
}

std::optional<ResolvedObjectEntry> ObjectStore::at(ObjectTopicId id, SequentialUID sequential_uid) const {
  if (!sequential_uid.valid()) {
    return std::nullopt;
  }

  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  if (series->entries.empty()) {
    return std::nullopt;
  }

  const SequentialUID first = series->entries.front().sequential_uid;
  const SequentialUID last = series->entries.back().sequential_uid;
  if (sequential_uid < first || sequential_uid > last) {
    return std::nullopt;
  }

  const auto it = std::lower_bound(
      series->entries.begin(), series->entries.end(), sequential_uid,
      [](const ObjectEntry& entry, SequentialUID uid) { return entry.sequential_uid < uid; });
  if (it == series->entries.end() || it->sequential_uid != sequential_uid) {
    return std::nullopt;
  }
  return resolveEntry(*it);
}

std::optional<size_t> ObjectStore::indexAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  if (series->entry_timestamps.empty()) {
    return std::nullopt;
  }

  auto it = std::upper_bound(series->entry_timestamps.begin(), series->entry_timestamps.end(), timestamp);
  if (it == series->entry_timestamps.begin()) {
    return std::nullopt;
  }
  --it;
  return static_cast<size_t>(it - series->entry_timestamps.begin());
}

SequentialUID ObjectStore::firstSequentialUID(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  if (series->entries.empty()) {
    return {};
  }
  return series->entries.front().sequential_uid;
}

SequentialUID ObjectStore::nextUIDAfter(ObjectTopicId id, SequentialUID after) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  const auto it = std::upper_bound(
      series->entries.begin(), series->entries.end(), after,
      [](SequentialUID uid, const ObjectEntry& entry) { return uid < entry.sequential_uid; });
  if (it == series->entries.end()) {
    return {};
  }
  return it->sequential_uid;
}

size_t ObjectStore::entryCount(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return 0;
  }

  std::shared_lock lock(series->mutex);
  return series->entries.size();
}

std::pair<Timestamp, Timestamp> ObjectStore::timeRange(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {0, 0};
  }

  std::shared_lock lock(series->mutex);
  if (series->entry_timestamps.empty()) {
    return {0, 0};
  }
  return {series->entry_timestamps.front(), series->entry_timestamps.back()};
}

EntryTimestampsView ObjectStore::entryTimestamps(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  return {std::move(lock), &series->entry_timestamps};
}

// --- Retention ---

void ObjectStore::setRetentionBudget(ObjectTopicId id, RetentionBudget budget) {
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return;
  }
  std::unique_lock lock(series->mutex);
  series->budget = budget;
}

RetentionBudget ObjectStore::retentionBudget(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return series->budget;
}

size_t ObjectStore::memoryUsage(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return 0;
  }
  std::shared_lock lock(series->mutex);
  return series->memory_bytes;
}

// --- Explicit eviction ---

void ObjectStore::evictBefore(ObjectTopicId id, Timestamp threshold) {
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return;
  }
  std::unique_lock lock(series->mutex);
  while (!series->entries.empty() && series->entry_timestamps.front() < threshold) {
    evictFront(*series);
  }
}

void ObjectStore::evictAllBefore(Timestamp threshold) {
  std::shared_lock store_lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    std::unique_lock lock(series->mutex);
    while (!series->entries.empty() && series->entry_timestamps.front() < threshold) {
      evictFront(*series);
    }
  }
}

// --- Cross-store flush ---

Status ObjectStore::flushTo(ObjectStore& dst) {
  if (&dst == this) {
    return unexpected("flushTo: source and destination are the same store");
  }

  // Deterministic lock order by address to avoid deadlock with concurrent flushTo calls.
  ObjectStore* first = this < &dst ? this : &dst;
  ObjectStore* second = first == this ? &dst : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  // Phase 1: validate every source series can be matched to a destination
  // topic by descriptor and that the move respects monotonicity. No mutation.
  struct Step {
    ObjectSeries* src;
    ObjectSeries* dst;
  };
  std::vector<Step> plan;
  plan.reserve(topics_.size());

  for (auto& [src_id, src_series] : topics_) {
    if (src_series->entry_timestamps.empty()) {
      continue;
    }
    ObjectSeries* dst_series = nullptr;
    for (auto& [dst_id, dst_series_ptr] : dst.topics_) {
      if (dst_series_ptr->descriptor.dataset_id == src_series->descriptor.dataset_id &&
          dst_series_ptr->descriptor.topic_name == src_series->descriptor.topic_name) {
        dst_series = dst_series_ptr.get();
        break;
      }
    }
    if (dst_series == nullptr) {
      return unexpected(
          "flushTo: destination has no topic '" + src_series->descriptor.topic_name + "' for dataset " +
          std::to_string(src_series->descriptor.dataset_id));
    }
    if (!dst_series->entry_timestamps.empty() &&
        src_series->entry_timestamps.front() < dst_series->entry_timestamps.back()) {
      return unexpected("flushTo: monotonicity violation for topic '" + src_series->descriptor.topic_name + "'");
    }
    plan.push_back({src_series.get(), dst_series});
  }

  // Phase 2: execute the moves. Holding both store_mutex_ unique blocks new
  // readers/writers, but a reader that acquired an EntryTimestampsView BEFORE this
  // call still holds only the series lock — drain those before reallocating the
  // timestamp vectors we are about to move/insert into.
  for (auto& step : plan) {
    drainSeriesReaders(*step.src);
    drainSeriesReaders(*step.dst);
    for (auto& entry : step.src->entries) {
      entry.sequential_uid = SequentialUID::getNext();
      step.dst->entries.push_back(std::move(entry));
    }
    step.dst->entry_timestamps.insert(
        step.dst->entry_timestamps.end(), step.src->entry_timestamps.begin(), step.src->entry_timestamps.end());
    step.dst->memory_bytes += step.src->memory_bytes;

    step.src->entries.clear();
    step.src->entry_timestamps.clear();
    step.src->memory_bytes = 0;
    // src's entries were moved out (and re-UIDed into dst); its warm cache now
    // refers to entries it no longer owns. dst keeps its cache: its pre-existing
    // entries are untouched, and any newly-appended entry has a fresh UID that
    // simply misses.
    {
      std::lock_guard src_cache(step.src->cache_mutex);
      step.src->cached_latest.reset();
    }

    const Timestamp newest = step.dst->entry_timestamps.empty() ? 0 : step.dst->entry_timestamps.back();
    applyRetention(*step.dst, newest);
  }

  return {};
}

Expected<ObjectDatasetReplaceResult> ObjectStore::replaceDatasetFrom(
    ObjectStore& staged, DatasetId staged_id, DatasetId primary_id) {
  if (&staged == this) {
    return unexpected("replaceDatasetFrom: staged and primary are the same store");
  }

  // Deterministic lock order by address (same discipline as flushTo).
  ObjectStore* first = this < &staged ? this : &staged;
  ObjectStore* second = first == this ? &staged : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  ObjectDatasetReplaceResult result;

  // Index primary (this) series under primary_id, by name -> (series, id).
  // Pointers stay valid across topics_.emplace_back below because the
  // ObjectSeries are heap-owned by unique_ptr — a vector realloc moves the
  // pointers, not the pointees.
  std::unordered_map<std::string, std::pair<ObjectSeries*, ObjectTopicId>> primary_by_name;
  for (auto& [pid, series] : topics_) {
    if (series->descriptor.dataset_id == primary_id) {
      primary_by_name.emplace(series->descriptor.topic_name, std::pair{series.get(), pid});
    }
  }

  std::unordered_set<std::string> staged_names;

  for (auto& [sid, staged_series] : staged.topics_) {
    if (staged_series->descriptor.dataset_id != staged_id) {
      continue;
    }
    const std::string& name = staged_series->descriptor.topic_name;
    staged_names.insert(name);

    // Resolve target series+id once (matched keeps its ObjectTopicId + adopts metadata; added mints a fresh one),
    // then run the identical entry move below a single time.
    ObjectSeries* primary_series = nullptr;
    ObjectTopicId primary_tid;
    if (auto match = primary_by_name.find(name); match != primary_by_name.end()) {
      primary_series = match->second.first;
      primary_tid = match->second.second;
      primary_series->descriptor.metadata_json = staged_series->descriptor.metadata_json;  // adopt reloaded metadata
    } else {
      primary_tid = ObjectTopicId{next_id_++};
      auto series = std::make_unique<ObjectSeries>();
      series->descriptor = staged_series->descriptor;
      series->descriptor.dataset_id = primary_id;
      primary_series = series.get();  // heap-owned: stays valid after emplace_back reallocs topics_
      topics_.emplace_back(primary_tid, std::move(series));
      series_index_.emplace(primary_tid.id, primary_series);
    }
    // A matched primary series may have outstanding views (the staged one may too);
    // drain both before replacing/moving their timestamp vectors. A freshly added
    // primary_series has no readers yet, so its drain is uncontended.
    drainSeriesReaders(*primary_series);
    drainSeriesReaders(*staged_series);
    primary_series->entries = std::move(staged_series->entries);
    for (auto& entry : primary_series->entries) {
      entry.sequential_uid = SequentialUID::getNext();
    }
    primary_series->entry_timestamps = std::move(staged_series->entry_timestamps);
    primary_series->memory_bytes = staged_series->memory_bytes;
    result.remapped.emplace_back(sid, primary_tid);
    staged_series->memory_bytes = 0;  // entries/timestamps already moved-from
    // Both series' warm caches now refer to replaced/moved-from entries (the
    // primary's entries are wholly new, with fresh UIDs); drop them.
    {
      std::lock_guard primary_cache(primary_series->cache_mutex);
      primary_series->cached_latest.reset();
    }
    {
      std::lock_guard staged_cache(staged_series->cache_mutex);
      staged_series->cached_latest.reset();
    }
  }

  // Remove primary topics the reloaded dataset no longer provides.
  for (const auto& [name, slot] : primary_by_name) {
    if (staged_names.count(name) == 0) {
      result.removed_topics.push_back(slot.second);
      eraseTopicLocked(slot.second);
    }
  }

  return result;
}

// --- Lifecycle ---

void ObjectStore::removeTopic(ObjectTopicId id) {
  std::unique_lock lock(store_mutex_);
  eraseTopicLocked(id);
}

void ObjectStore::eraseTopicLocked(ObjectTopicId id) {
  auto it = std::find_if(topics_.begin(), topics_.end(), [&](const auto& pair) { return pair.first == id; });
  if (it != topics_.end()) {
    drainSeriesReaders(*it->second);  // let outstanding views release before the series dies
    series_index_.erase(id.id);
    topics_.erase(it);
  }
}

void ObjectStore::drainSeriesReaders(ObjectSeries& series) {
  // Acquire-and-release: blocks until in-flight shared readers drain. With
  // store_mutex_ held exclusively by the caller, nothing can re-acquire the series
  // lock after this returns, so the series is safe to destroy or mutate.
  std::unique_lock<std::shared_mutex> drain(series.mutex);
}

void ObjectStore::clearDataset(DatasetId dataset_id) {
  // Exclusive store lock blocks new readers/writers; for each series in the
  // dataset, drain any reader holding only the series lock (an EntryTimestampsView)
  // before emptying its timestamp vector, then clear it in place — keeping the
  // ObjectTopicId registered (never removeTopic+re-register). Unknown dataset_id
  // matches no series -> no-op; clearing an empty series is a no-op (idempotent).
  std::unique_lock lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id != dataset_id) {
      continue;
    }
    drainSeriesReaders(*series);
    clearEntriesLocked(*series);
  }
}

ObjectStore::ObjectDatasetSnapshot ObjectStore::detachDataset(DatasetId dataset_id) {
  // Same discipline as clearDataset, but MOVE each series' entries aside instead of
  // dropping them. drainSeriesReaders before touching a series so no EntryTimestampsView
  // dangles into the timestamp vector we move out.
  std::unique_lock lock(store_mutex_);
  ObjectDatasetSnapshot snapshot;
  snapshot.dataset_id = dataset_id;
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id != dataset_id) {
      continue;
    }
    snapshot.prior_object_topic_ids.push_back(tid);
    drainSeriesReaders(*series);
    ObjectDatasetSnapshot::SeriesSnapshot series_snapshot;
    series_snapshot.entries = std::move(series->entries);
    series_snapshot.entry_timestamps = std::move(series->entry_timestamps);
    series_snapshot.budget = series->budget;
    series_snapshot.memory_bytes = series->memory_bytes;
    // Normalize the now-empty series in place (memory accounting + warm cache),
    // keeping the ObjectTopicId registered — exactly like clearDataset.
    clearEntriesLocked(*series);
    snapshot.series.emplace(tid.id, std::move(series_snapshot));
  }
  if (snapshot.prior_object_topic_ids.empty()) {
    return snapshot;  // unknown / empty dataset: `valid` stays false
  }
  snapshot.valid = true;
  return snapshot;
}

void ObjectStore::reattachDataset(DatasetId dataset_id, ObjectDatasetSnapshot&& snapshot) {
  std::unique_lock lock(store_mutex_);
  if (!snapshot.valid) {
    return;
  }
  const std::unordered_set<uint32_t> prior([&snapshot] {
    std::unordered_set<uint32_t> set;
    set.reserve(snapshot.prior_object_topic_ids.size());
    for (const ObjectTopicId id : snapshot.prior_object_topic_ids) {
      set.insert(id.id);
    }
    return set;
  }());
  // Reconcile the current series set against the prior one. Snapshot the current
  // ids first since eraseTopicLocked mutates topics_. A series the failed refill
  // added (not in `prior`) is erased; a prior series is cleared before its entries
  // are moved back. (When the caller already evicted the added topics, `current`
  // simply has none to erase — tolerated.)
  std::vector<ObjectTopicId> current;
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      current.push_back(tid);
    }
  }
  for (const ObjectTopicId tid : current) {
    if (prior.find(tid.id) == prior.end()) {
      eraseTopicLocked(tid);  // drains + erases internally
    } else if (ObjectSeries* series = findSeries(tid)) {
      drainSeriesReaders(*series);
      clearEntriesLocked(*series);
    }
  }
  // Move the prior entries + budget + memory back into the (still-registered)
  // series. clearEntriesLocked above already reset each prior series' warm cache.
  for (auto& [raw_id, series_snapshot] : snapshot.series) {
    ObjectSeries* series = findSeries(ObjectTopicId{raw_id});
    if (series == nullptr) {
      continue;  // defensive: a prior series vanished (should not happen)
    }
    series->entries = std::move(series_snapshot.entries);
    series->entry_timestamps = std::move(series_snapshot.entry_timestamps);
    series->budget = series_snapshot.budget;
    series->memory_bytes = series_snapshot.memory_bytes;
  }
  snapshot.valid = false;
}

void ObjectStore::clearEntriesLocked(ObjectSeries& series) {
  series.entries.clear();
  series.entry_timestamps.clear();
  series.memory_bytes = 0;
  // The warm cache now refers to dropped entries; reset it under its own lock
  // (mirrors the matched-series clear in flushTo / replaceDatasetFrom).
  std::lock_guard cache_guard(series.cache_mutex);
  series.cached_latest.reset();
}

void ObjectStore::clear() {
  std::unique_lock lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    drainSeriesReaders(*series);
  }
  topics_.clear();
  series_index_.clear();
  next_id_ = 1;
}

// --- Private helpers ---

ObjectStore::ObjectSeries* ObjectStore::findSeries(ObjectTopicId id) {
  auto it = series_index_.find(id.id);
  return it != series_index_.end() ? it->second : nullptr;
}

const ObjectStore::ObjectSeries* ObjectStore::findSeries(ObjectTopicId id) const {
  auto it = series_index_.find(id.id);
  return it != series_index_.end() ? it->second : nullptr;
}

ResolvedObjectEntry ObjectStore::resolveEntry(const ObjectEntry& entry) {
  ResolvedObjectEntry resolved;
  resolved.timestamp = entry.timestamp;
  resolved.sequential_uid = entry.sequential_uid;

  if (const auto* owned = std::get_if<SharedBuffer>(&entry.payload)) {
    // Span the vector, anchor on the same shared_ptr — refcount bump, no copy.
    // A default-constructed entry holds a null SharedBuffer, so guard it.
    if (*owned) {
      resolved.payload = sdk::PayloadView{
          Span<const uint8_t>{(*owned)->data(), (*owned)->size()},
          sdk::BufferAnchor{*owned},
      };
    }
  } else if (const auto* lazy = std::get_if<LazyCallback>(&entry.payload)) {
    // Forward the closure's PayloadView verbatim. The anchor stays opaque (no
    // cast), so producers can back it with arrow::Buffer, mmap, or a C-ABI anchor.
    resolved.payload = (*lazy)();
  }

  return resolved;
}

void ObjectStore::evictFront(ObjectSeries& series) {
  if (series.entries.empty()) {
    return;
  }

  const auto& front = series.entries.front();
  // Drop the warm cache if it holds the entry being evicted, so its bytes are
  // released with the entry rather than pinned past its lifetime.
  {
    std::lock_guard cache_guard(series.cache_mutex);
    if (series.cached_latest && series.cached_latest->sequential_uid == front.sequential_uid) {
      series.cached_latest.reset();
    }
  }
  if (const auto* owned = std::get_if<SharedBuffer>(&front.payload); owned != nullptr && *owned) {
    series.memory_bytes -= (*owned)->size();
  }

  series.entries.pop_front();
  series.entry_timestamps.erase(series.entry_timestamps.begin());
}

void ObjectStore::applyRetention(ObjectSeries& series, Timestamp newest_ts) {
  if (series.budget.time_window_ns > 0) {
    Timestamp threshold = newest_ts - series.budget.time_window_ns;
    while (!series.entries.empty() && series.entry_timestamps.front() < threshold) {
      evictFront(series);
    }
  }
  if (series.budget.max_memory_bytes > 0) {
    while (!series.entries.empty() && series.memory_bytes > series.budget.max_memory_bytes) {
      evictFront(series);
    }
  }
}

}  // namespace PJ
