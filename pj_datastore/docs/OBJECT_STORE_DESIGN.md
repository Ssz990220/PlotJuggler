# ObjectStore Design

`ObjectStore` stores timestamped opaque byte payloads alongside the columnar
`DataEngine`. It is for data that should be selected by time but should not be
expanded into scalar columns at ingest time.

## Responsibilities

`ObjectStore` owns:

- object topic registration scoped by `DatasetId`
- monotonically non-decreasing timestamped entries per topic
- eager payload storage through `pushOwned()`
- lazy payload storage through `pushLazy()`
- at-or-before timestamp lookup
- entry-index lookup and timestamp views
- per-topic retention budgets
- explicit topic eviction, removal, and clear operations

It does not decode payloads, interpret metadata, choose renderers, or own UI
policy. Topic metadata is opaque JSON retained verbatim for callers that need to
interpret object bytes.

## Data Model

```cpp
struct ObjectTopicDescriptor {
  DatasetId dataset_id;
  std::string topic_name;
  std::string metadata_json;
};

// Eager payload: store-owned bytes, counted against the retention budget.
using SharedBuffer = std::shared_ptr<const std::vector<uint8_t>>;
// Lazy payload: idempotent fetcher returning a view + ownership anchor.
using LazyCallback = std::function<sdk::PayloadView()>;

struct ObjectEntry {
  Timestamp timestamp;
  SequentialUID sequential_uid;
  std::variant<SharedBuffer, LazyCallback> payload;
};

struct RetentionBudget {
  int64_t time_window_ns;
  size_t max_memory_bytes;
};
```

Topic names must be unique within one dataset. The same topic name may appear in
different datasets.

Entries in a topic must be pushed in monotonically non-decreasing timestamp
order. Equal timestamps are allowed. Out-of-order writes fail.

## Entry Identity (SequentialUID)

Every entry carries a `SequentialUID` (`pj_datastore/sequential_uid.hpp`): a
stable identity assigned at insert from a process-wide atomic counter (under the
series write lock, so UIDs are strictly increasing within a topic). Value 0 is
the invalid/default sentinel.

Properties consumers can rely on:

- **Stable across eviction.** Unlike a deque index, a UID is never renumbered or
  reused when retention drops front entries. Replay cursors and decoded-payload
  caches key on it safely.
- **Sparse per topic.** Allocation is global across all topics, so consecutive
  entries of one topic are *not* consecutive integers. Never iterate a topic by
  incrementing UID values; step with `nextUIDAfter()`.
- **A generation marker.** `flushTo()` and `replaceDatasetFrom()` assign fresh
  UIDs to the entries they move into the destination topic. A cursor whose UID
  falls below `firstSequentialUID()` therefore detects both "evicted past me"
  and "dataset replaced" with one comparison (eviction is front-only, so the
  first retained UID passing the cursor is exactly the missed-entry condition).

## Write Paths

`pushOwned(id, timestamp, payload)` moves the caller-provided vector into a
shared buffer owned by the store. Owned entries contribute to `memoryUsage()`.

`pushLazy(id, timestamp, fetch)` stores a callable instead of bytes. The callable
is invoked on resolve and returns a `sdk::PayloadView` — a `Span<const uint8_t>`
paired with a type-erased `BufferAnchor` that keeps those bytes alive for as long
as the resolved view is held. The producer anchors on whatever already owns the
bytes (a decompressed chunk, an mmap, or a fresh allocation via
`sdk::makePayloadView`), so the store never copies on resolve. Lazy entries do not
contribute to `memoryUsage()` because the store retains the callable, not the
fetched bytes. The callable runs on every `at()` read; `latestAt()` repeats of the
same sample are served from a warm cache without re-invoking it (see Read Paths).

Both write paths apply the topic retention budget after the new entry is
inserted.

## Dataset Merge

`ObjectStore::mergeDatasets(anchor, sources)` is the object-side companion to the
scalar dataset merge. It destructively folds source datasets into an anchor using
caller-supplied raw timestamp shifts.

- Shared object topic names are fused into the anchor topic: entries are
  interleaved by shifted timestamp, equal timestamps keep anchor entries before
  source entries, and the fused series receives fresh ascending
  `SequentialUID`s so `at(uid)` remains a valid binary-search path.
- Source-only object topics are reparented to the anchor dataset and keep their
  `ObjectTopicId`; source datasets are emptied.
- Duplicate or self sources are rejected before mutation, so that validation is
  atomic. A source with no object topics is accepted as a no-op.
- The fold is purely mechanical. It does not detect canonical object type
  conflicts; pj_runtime owns that preflight policy.
- Topic retention is re-applied after each fold. Lazy entries move by value, and
  their closures keep their original backing data alive.
- **Payload-embedded stamps follow the shift.** The merge moves each shifted
  entry's STORE `timestamp` but never rewrites the payload bytes, so timestamps
  encoded INSIDE a payload (a serialized canonical object, or a wire message a
  parser later decodes) would otherwise stay on the source's original clock. To
  reconcile them without touching the bytes, each shifted entry records the same
  delta in `ObjectEntry::payload_stamp_shift` (carried through `resolveEntry` to
  `ResolvedObjectEntry`). A consumer that keys off payload-embedded stamps adds
  it; one that keys off the store `timestamp` ignores it. It is 0 for unmerged
  data and accumulates across chained merges. The one consumer today is the 3D TF
  buffer, which indexes history by each transform's own inner stamp.

## Read Paths

`latestAt(id, timestamp)` returns the newest entry whose timestamp is less than
or equal to the query timestamp. It returns `std::nullopt` if the topic is
unknown, empty, or has no entry at or before that time.

`at(id, index)` resolves an entry by sequence index.

`at(id, sequential_uid)` resolves an entry by stable UID (binary search; nullopt
when evicted, invalid, or from another topic/generation).

`indexAt(id, timestamp)` returns the index that `latestAt()` would resolve.

`firstSequentialUID(id)` returns the first retained entry's UID;
`nextUIDAfter(id, after)` returns the next retained UID strictly greater than
`after` (an invalid `after` starts from the front). Together they let a replay
cursor walk a topic's sparse UID sequence at one binary search per entry.

`entryTimestamps(id)` returns an `EntryTimestampsView` that holds the series read
lock while the timestamp span is inspected.

Resolved entries contain:

```cpp
struct ResolvedObjectEntry {
  Timestamp timestamp;
  SequentialUID sequential_uid;
  sdk::PayloadView payload;  // { Span<const uint8_t> bytes; BufferAnchor anchor; }
};
```

`payload.bytes` is the resolved view; `payload.anchor` keeps those bytes alive
independently of later store mutation (eviction, removal, or `clear()`). For an
owned entry the anchor is the store's `SharedBuffer`; for a lazy entry it is
whatever the fetcher anchored on. An empty `anchor` means "no bytes".

### Warm cache (latestAt)

A ~60 Hz scene renderer calls `latestAt(topic, t)` every frame, but object topics
publish far slower, so consecutive calls usually resolve the *same* sample. To
avoid re-invoking a lazy fetcher (which, for the MCAP source, re-decompresses a
chunk and re-copies the payload) on every frame, each series keeps a one-slot warm
cache: the most-recently-resolved `ResolvedObjectEntry`, keyed by its
`sequential_uid`.

- **Scope.** Only `latestAt()` consults and populates the cache. `at(index)` and
  `at(uid)` always resolve fresh, preserving their per-read semantics (a prefetch
  or replay walk through `at()` therefore cannot evict the renderer's current
  sample).
- **Hit/miss.** A hit (cached `sequential_uid` matches the looked-up entry)
  returns the cached entry without invoking the fetcher. A miss resolves once and
  caches the result — but only if the payload is non-empty, so a failed/empty
  resolve is retried on the next read rather than latched.
- **Identity is exact.** `SequentialUID` is never reused, so a hit is always the
  same entry; no validation against the underlying bytes is needed.
- **Invalidation.** The slot is dropped when its entry is evicted (`evictFront`
  matching the cached UID) and when the series is replaced or flushed
  (`replaceDatasetFrom`, `flushTo`). A plain `push` never needs to invalidate: a
  new entry has a new UID, so a query mapping to it simply misses.
- **Residency.** For a lazy topic this keeps at most one materialized payload
  resident per topic (the current sample) — a deliberate relaxation of "lazy
  entries are never resident". It never holds more than the current entry, so the
  whole series (e.g. a full video) is never brought into memory. The warm payload
  is not counted by `memoryUsage()` (which tracks owned buffers only).

## Retention

Retention is configured per topic:

- `time_window_ns > 0`: drop entries older than `newest_push_ts -
  time_window_ns`.
- `max_memory_bytes > 0`: drop oldest entries until owned-payload memory is at
  or below the cap.

Either axis can be zero to disable that axis. Both zero disables automatic
retention.

Automatic retention runs only during `pushOwned()` and `pushLazy()`. Explicit
eviction is available through `evictBefore(id, threshold)` and
`evictAllBefore(threshold)`.

Memory accounting includes only owned payloads. Lazy entries are counted as zero
bytes because the store retains a fetch callable, not the fetched payload.

## Threading

The store has one global shared mutex for topic lookup and one shared mutex per
topic series. Reads can proceed concurrently with reads on the same or different
topics. Writes take the target topic's exclusive lock. Topic registration,
removal, and `clear()` take the global exclusive lock.

The warm cache (above) has its own small per-series mutex, distinct from the
series shared mutex. `latestAt` holds the series mutex only in shared mode and
never holds the cache mutex across `resolveEntry()` (a lazy fetch), so a slow
decode on a miss cannot block other readers of the series. A lazy fetcher is
always invoked outside every store lock.

### Deferred: off-thread prefetch

A background worker that warms upcoming samples ahead of the playhead was
considered, to move genuine new-frame decompression off the GUI thread entirely.
It is **deferred**: for the MCAP source, all cold fetches of a source serialize on
a single per-source mutex (`DataSourceRuntimeHost::lazy_fetch_mutex_`) plus
`ColdChunkStore`'s own mutex, because the cold reader is a single-cursor
`FileReader` that is not concurrent-safe. A background decompress would hold that
mutex and block a concurrent GUI fetch — even for an already-warm chunk — for the
decompress duration, relocating the stall rather than removing it. Making prefetch
genuinely off-load the work requires concurrent-izing that single-cursor reader
(finer `ColdChunkStore` locking, or a second reader sharing the chunk cache),
which is the real cost of this feature; the prefetch controller itself is the easy
part.

## Plugin ABI Bridge

Plugin access to `ObjectStore` is provided by three optional v4 services:

| Service | Host implementation | Purpose |
|---|---|---|
| `pj.source_object_write.v1` | `DatastoreSourceObjectWriteHost` | DataSource plugins register object topics and push owned or lazy entries. |
| `pj.parser_object_write.v1` | `DatastoreParserObjectWriteHost` | MessageParser plugins push entries to a host-bound object topic. |
| `pj.toolbox_object_read.v1` | `DatastoreToolboxObjectReadHost` | Toolbox plugins look up topics and read entries as owning byte handles. |

The raw ABI lives in `plotjuggler_sdk/pj_base/include/pj_base/plugin_data_api.h`;
the C++ SDK views live in
`plotjuggler_sdk/pj_base/include/pj_base/sdk/plugin_data_api.hpp` (from the
`plotjuggler_sdk` submodule; the in-code `#include` paths are
`pj_base/plugin_data_api.h` and `pj_base/sdk/plugin_data_api.hpp`).

The toolbox read ABI allocates one owning handle per successful read. The handle
keeps bytes alive until the plugin releases it, even if the store evicts or
removes the underlying topic.

## Tests

Core behavior is covered by:

- `pj_datastore/tests/object_store_test.cpp`
- `pj_datastore/tests/plugin_data_host_object_test.cpp`
- `pj_datastore/tests/plugin_data_host_object_read_test.cpp`
- `pj_datastore/tests/plugin_parser_object_write_test.cpp`
