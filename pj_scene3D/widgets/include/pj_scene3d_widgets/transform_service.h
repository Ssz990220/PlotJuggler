#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
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
// (objectStore() + parserForObjectTopic()).
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

  // Walks every object topic in `dataset_id`, probes each via parseObject to
  // detect FrameTransforms schemas, and pushes every transform into the
  // dataset's TransformBuffer. Idempotent: re-ingest of an already-populated
  // dataset is a no-op (re-ingest would duplicate samples — TransformBuffer
  // appends without dedup).
  //
  // Synchronous; blocks the calling thread. For large MCAPs (100K+ TF
  // messages) callers may run this on a worker thread.
  void ingestFrameTransformsForDataset(PJ::DatasetId dataset_id);

  // Forgets a dataset's TF state so the next ingest re-reads the store: clears
  // the ingest-done flag and empties the existing TransformBuffer IN PLACE
  // (3D docks share that buffer by pointer, so they see the reset rather than
  // holding a stale orphan). Call when the dataset's object topics no longer
  // hold the data the buffer was built from: an in-place dataset replace
  // (same-file reload) or the dataset's removal/eviction.
  void invalidateDataset(PJ::DatasetId dataset_id);

  // invalidateDataset() over every known dataset — the clear-all counterpart,
  // paired with SessionManager::clearAllObjects() at the shell's wipe sites.
  void invalidateAll();

 signals:
  // Emitted after ingestFrameTransformsForDataset finishes populating a
  // dataset's TransformBuffer, so 3D docks can render once TF is ready.
  void datasetTransformsReady(PJ::DatasetId dataset_id);

 private:
  PJ::SessionManager& session_;
  std::unordered_map<PJ::DatasetId, std::shared_ptr<TransformBuffer>> transform_buffers_;
  std::unordered_set<PJ::DatasetId> transforms_populated_;
};

}  // namespace pj::scene3d
