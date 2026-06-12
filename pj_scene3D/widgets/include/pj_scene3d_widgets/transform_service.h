#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <cstddef>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "pj_base/types.hpp"

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
  // FrameTransforms schemas, then runs the shared cursor ingest with every
  // cursor at its INT64_MIN start, so one pass covers the whole history. The
  // per-topic cursors guard against double-ingest, so a redundant call ingests
  // nothing new — but it still re-emits datasetTransformsReady (it is NOT a
  // silent no-op). Emits datasetTransformsReady when done.
  //
  // Synchronous; blocks the calling thread. For large MCAPs (100K+ TF
  // messages) callers may run this on a worker thread.
  void ingestFrameTransformsForDataset(PJ::DatasetId dataset_id);

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

  // Incremental, timestamp-keyed ingest of TF entries that arrived since the
  // last call for this dataset. Cheap no-op when nothing is new (the common
  // streaming tick). Safe to call repeatedly; never double-ingests an entry
  // (a per-topic timestamp cursor guards it). Returns true if the buffer
  // changed, so the caller can recompute orphan states + repaint only then.
  bool ingestNewTransforms(PJ::DatasetId dataset_id);

 signals:
  // Emitted after ingestFrameTransformsForDataset finishes populating a
  // dataset's TransformBuffer, so 3D docks can render once TF is ready.
  void datasetTransformsReady(PJ::DatasetId dataset_id);

 private:
  // Shared core: for every TF topic in the dataset, ingest entries whose
  // timestamp is newer than the per-topic cursor. Both the bulk file path and
  // the incremental streaming path go through here. Returns true if any
  // transform was applied.
  bool ingestNewerThanCursor(PJ::DatasetId dataset_id);

  // Per-topic ingest cursor: the newest timestamp already pushed into the buffer
  // plus how many entries at exactly that timestamp were ingested. The count is
  // load-bearing — ObjectStore allows equal-timestamp appends, and the streaming
  // worker pushes concurrently with the ingest tick, so a second entry sharing
  // the newest timestamp can land after a tick's scan. A bare-timestamp cursor
  // skips it forever (indexAt is at-or-before, so it walks past the late arrival
  // every later pass); the count lets the next tick re-scan that timestamp's run
  // and ingest only the entries beyond the ones already taken.
  struct TfCursor {
    PJ::Timestamp timestamp = std::numeric_limits<PJ::Timestamp>::min();
    std::size_t count_at_timestamp = 0;
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
