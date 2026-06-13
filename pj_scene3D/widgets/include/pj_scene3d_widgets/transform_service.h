#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "pj_base/types.hpp"
#include "pj_datastore/sequential_uid.hpp"

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

class TransformBuffer;

// Owns the per-dataset 3D transform buffers and the load-time TF ingest.
//
// This deliberately lives in the 3D widget family rather than in
// pj_runtime: SessionManager (the shared, domain-neutral runtime) must not
// depend on 3D-specific types like TransformBuffer. The service reaches the
// data it needs through SessionManager's neutral surface only
// (objectStore() + parserBindingForObjectTopic()).
//
// One TransformBuffer per dataset is created lazily and shared by every 3D
// dock attached to that dataset, so dropping a second pointcloud is instant
// (no re-walk of every TF entry). Per pj_scene3D REQUIREMENTS §9.
//
// THREADING: every method must run on the GUI thread (the QObject's own
// thread). The internal cursor / classification containers are unsynchronized,
// and the streaming samplesIngested slot already drives ingestNewTransforms on
// every tick from the GUI thread — so there is no safe worker-thread entry point.
// The individual ObjectStore / TransformBuffer reads are themselves
// thread-safe, but the service's bookkeeping around them is not.
class TransformService : public QObject {
  Q_OBJECT
 public:
  explicit TransformService(PJ::SessionManager& session, QObject* parent = nullptr);
  ~TransformService() override;

  TransformService(const TransformService&) = delete;
  TransformService& operator=(const TransformService&) = delete;

  // Returns the dataset's TransformBuffer, lazily creating it on first access.
  // The buffer is thread-safe for concurrent ingest writes + render reads.
  [[nodiscard]] std::shared_ptr<TransformBuffer> transformBuffer(PJ::DatasetId dataset_id);

  // Bulk path: ingests a dataset's full TF history into its TransformBuffer at
  // file load. Probes every object topic via parseObject to detect
  // FrameTransforms schemas, then runs the shared UID cursor ingest with every
  // cursor at its invalid (begin-of-history) start, so one pass covers the whole
  // history. The per-topic cursors guard against double-ingest, so a redundant
  // call ingests nothing new — but it still re-emits datasetTransformsReady (it
  // is NOT a silent no-op). Emits datasetTransformsReady when done.
  //
  // Threading: GUI-thread only, like every TransformService method (see the
  // class comment). Synchronous; blocks the calling thread for the whole TF
  // history.
  void ingestFrameTransformsForDataset(PJ::DatasetId dataset_id);

  // Bound the dataset's TransformBuffer to a rolling cache window so a
  // live-streaming session does not retain every TF sample forever. Trims
  // per-edge to (newest stamp - `window`) but always keeps the last sample of
  // each edge, so static / once-published frames stay resolvable. File loads
  // never call this and keep the buffer's kKeepAll default (see transformBuffer).
  // Reconfiguring is cheap and can be re-issued whenever the retention budget
  // changes; GUI-thread only.
  void setLiveCacheWindow(PJ::DatasetId dataset_id, std::chrono::nanoseconds window);

  // Forgets a dataset's TF state so the next ingest re-reads the store: drops the
  // per-topic ingest cursors and non-TF classifications, and empties the existing
  // TransformBuffer IN PLACE (3D docks share that buffer by pointer, so they see
  // the reset rather than holding a stale orphan). Call when the dataset's object
  // topics no longer hold the data the buffer was built from: an in-place dataset
  // replace (same-file reload) or the dataset's removal/eviction.
  void invalidateDataset(PJ::DatasetId dataset_id);

  // invalidateDataset() over every known dataset — the clear-all counterpart,
  // paired with SessionManager::clearAllObjects() at the shell's wipe sites.
  void invalidateAll();

  // Incremental, UID-keyed ingest of TF entries that arrived since the last call
  // for this dataset. Cheap no-op when nothing is new (the common streaming
  // tick). Safe to call repeatedly; never double-ingests an entry (a per-topic
  // SequentialUID cursor guards it). GUI-thread only.
  //
  // The bool reports whether THIS call applied any transforms — NOT a reliable
  // "something changed" signal for repaint gating: the per-topic cursor is
  // shared across sibling docks, so a sibling tick may have already advanced it
  // and this call returns false while the buffer did change. Live followers
  // should gate repaint on TransformBuffer::revision() instead. The return is
  // kept because ingestFrameTransformsForDataset still logs it.
  bool ingestNewTransforms(PJ::DatasetId dataset_id);

 signals:
  // Emitted after ingestFrameTransformsForDataset finishes populating a
  // dataset's TransformBuffer, so 3D docks can render once TF is ready.
  void datasetTransformsReady(PJ::DatasetId dataset_id);

 private:
  // Shared core: for every TF topic in the dataset, step the per-topic UID
  // cursor forward (ObjectStore::nextUIDAfter) and ingest each newly retained
  // entry. Both the bulk file path and the incremental streaming path go through
  // here. GUI-thread only. Returns true if any transform was applied.
  bool ingestNewerThanCursor(PJ::DatasetId dataset_id);

  // Per-topic ingest cursor: the SequentialUID of the last entry pushed into the
  // buffer. A UID is stable across front-eviction and per-topic SPARSE (UID
  // allocation is process-global), so the next tick steps strictly forward with
  // ObjectStore::nextUIDAfter — never by incrementing the value. This fixes both
  // the index-shift TOCTOU (a concurrent front-eviction can never move a
  // not-yet-ingested entry below the cursor) and the equal-timestamp skip (UIDs
  // are unique even when timestamps tie) that a timestamp cursor suffered.
  // Default-constructed (invalid) UID means "ingest from the first retained
  // entry" — the bulk file-load start. An entry evicted before its UID is
  // reached is unrecoverable by design: the store no longer holds it.
  struct TfCursor {
    PJ::SequentialUID last_ingested;
  };

  PJ::SessionManager& session_;
  std::unordered_map<PJ::DatasetId, std::shared_ptr<TransformBuffer>> transform_buffers_;
  // Per-topic ingest cursors (keyed by ObjectTopicId::id). A topic present here
  // is a known FrameTransforms topic. Topics proven NOT to be TF go into
  // non_tf_topics_ so they are classified (parsed) at most once instead of every
  // streaming tick.
  std::unordered_map<uint32_t, TfCursor> tf_cursors_;
  std::unordered_set<uint32_t> non_tf_topics_;
};

}  // namespace pj::scene3d
