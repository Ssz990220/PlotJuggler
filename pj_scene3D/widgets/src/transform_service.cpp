// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/transform_service.h"

#include <QLoggingCategory>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/parse_locked.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcTransformService, "pj.scene3d.transform_service")

struct IngestStats {
  std::size_t ingested = 0;
  std::size_t dropped_reparent = 0;   // child claimed by two parents
  std::size_t dropped_self_loop = 0;  // child == parent
  std::size_t dropped_invalid = 0;    // empty name / non-unit rotation / non-finite translation
};

// Decode one object entry already known to belong to a FrameTransforms topic and
// push each of its edges into the buffer. A malformed edge (reparent conflict /
// self-loop / invalid frame data) is a recoverable data error in a real bag:
// count it and keep going rather than aborting.
void ingestEntry(
    PJ::Timestamp ts, const PJ::sdk::PayloadView& payload, const PJ::SessionManager::ParserBinding& parser_binding,
    TransformBuffer& tf_buffer, IngestStats& stats) {
  auto obj = parseLocked(parser_binding, ts, payload);
  if (!obj.has_value()) {
    return;
  }
  const auto* ft = std::any_cast<PJ::sdk::FrameTransforms>(&obj->object);
  if (ft == nullptr) {
    return;
  }
  for (const auto& t : ft->transforms) {
    StampedTransform st;
    st.stamp = TimePoint{std::chrono::nanoseconds(t.timestamp)};
    st.parent_frame = t.parent_frame_id;
    st.child_frame = t.child_frame_id;
    st.transform.t = glm::dvec3{t.translation.x, t.translation.y, t.translation.z};
    st.transform.q = glm::dquat{t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
    const auto result = tf_buffer.setTransform(st);
    if (result.has_value()) {
      ++stats.ingested;
      continue;
    }
    switch (result.error()) {
      case SetTransformError::kReparentConflict:
        ++stats.dropped_reparent;
        break;
      case SetTransformError::kSelfLoop:
        ++stats.dropped_self_loop;
        break;
      case SetTransformError::kInvalidFrameName:
      case SetTransformError::kInvalidRotation:
      case SetTransformError::kNonFiniteTranslation:
        ++stats.dropped_invalid;
        break;
    }
  }
}
}  // namespace

TransformService::TransformService(PJ::SessionManager& session, QObject* parent) : QObject(parent), session_(session) {}

TransformService::~TransformService() = default;

std::shared_ptr<TransformBuffer> TransformService::transformBuffer(PJ::DatasetId dataset_id) {
  auto it = transform_buffers_.find(dataset_id);
  if (it != transform_buffers_.end()) {
    return it->second;
  }
  // Default to keeping the full history: ingestFrameTransformsForDataset()
  // bulk-loads the entire dataset's TF up front, so a rolling cache window would
  // trim every dynamic edge to its last samples and make objects in dynamic
  // frames (e.g. a local costmap in `odom`) resolve only near the end of the
  // timeline. Live-streaming datasets override this with setLiveCacheWindow() to
  // bound memory in step with the ObjectStore retention budget.
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  transform_buffers_[dataset_id] = buf;
  return buf;
}

void TransformService::invalidateDataset(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  PJ::ObjectStore& object_store = session_.objectStore();
  // Cursor-model equivalent of forgetting the old transforms_populated_ flag:
  // drop the per-topic ingest cursors (and not-a-TF classifications) for this
  // dataset's topics so the next ingest re-reads their TF history from scratch
  // into the cleared buffer.
  for (const auto& topic_id : object_store.listTopics(dataset_id)) {
    tf_cursors_.erase(topic_id.id);
    non_tf_topics_.erase(topic_id.id);
  }
  // listTopics only resolves topics the dataset still has, so the loop above
  // misses keys for topics this reload/eviction already removed (on the reload
  // path replaceDataset drops removed_topics before we run; on the tombstone
  // path the caller must invalidate BEFORE eviction). Sweep the maps and erase
  // any key whose store descriptor is now empty so dead cursors do not leak
  // across reloads.
  std::erase_if(tf_cursors_, [&object_store](const auto& entry) {
    return object_store.descriptor(PJ::ObjectTopicId{entry.first}).topic_name.empty();
  });
  std::erase_if(non_tf_topics_, [&object_store](uint32_t key) {
    return object_store.descriptor(PJ::ObjectTopicId{key}).topic_name.empty();
  });
  if (auto it = transform_buffers_.find(dataset_id); it != transform_buffers_.end()) {
    // Clear in place: 3D docks hold this buffer by shared_ptr, so swapping the
    // map entry would leave them rendering the stale orphan forever.
    it->second->clear();
  }
}

void TransformService::setLiveCacheWindow(PJ::DatasetId dataset_id, std::chrono::nanoseconds window) {
  Q_ASSERT(QThread::currentThread() == thread());
  transformBuffer(dataset_id)->setCacheWindow(window);
}

void TransformService::invalidateAll() {
  tf_cursors_.clear();
  non_tf_topics_.clear();
  for (auto& [dataset_id, buffer] : transform_buffers_) {
    (void)dataset_id;
    buffer->clear();
  }
}

void TransformService::ingestFrameTransformsForDataset(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  // Bulk path: every cursor starts at the invalid (begin-of-history) UID, so
  // this ingests the whole history in one pass (file load); ingestNewerThanCursor
  // creates the buffer. datasetTransformsReady tells 3D docks the tree is ready.
  ingestNewerThanCursor(dataset_id);
  emit datasetTransformsReady(dataset_id);
}

bool TransformService::ingestNewTransforms(PJ::DatasetId dataset_id) {
  return ingestNewerThanCursor(dataset_id);
}

bool TransformService::ingestNewerThanCursor(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  PJ::ObjectStore& object_store = session_.objectStore();
  auto tf_buffer = transformBuffer(dataset_id);
  IngestStats stats;

  for (const auto& topic_id : object_store.listTopics(dataset_id)) {
    const uint32_t key = topic_id.id;
    if (non_tf_topics_.contains(key)) {
      continue;  // already classified as not-a-TF topic; never re-probe it
    }
    const auto count = object_store.entryCount(topic_id);
    if (count == 0) {
      continue;  // nothing to classify/ingest yet; retry on a later tick
    }
    const auto parser_binding = session_.parserBindingForObjectTopic(topic_id);
    if (!parser_binding) {
      continue;
    }

    auto cursor_it = tf_cursors_.find(key);
    if (cursor_it == tf_cursors_.end()) {
      // Unclassified topic: probe its newest entry's type exactly once. Probing
      // the newest (not index 0) stays valid after streaming evicts the front,
      // and classifying once keeps a big non-TF topic (e.g. a point cloud) from
      // being decoded on every tick.
      auto probe = object_store.at(topic_id, count - 1);
      if (!probe.has_value() || probe->payload.bytes.empty()) {
        continue;  // can't classify yet; retry next tick
      }
      auto probe_obj = parseLocked(parser_binding, probe->timestamp, probe->payload);
      if (!probe_obj.has_value()) {
        // Parse FAILED (transient corruption / a parser that failed this tick) —
        // do NOT blacklist the topic. A single bad newest message must not
        // permanently suppress TF ingest; retry classification next tick.
        continue;
      }
      if (PJ::sdk::typeOf(probe_obj->object) != PJ::sdk::BuiltinObjectType::kFrameTransforms) {
        non_tf_topics_.insert(key);  // parsed cleanly as a different type: settled, never re-probe
        continue;
      }
      cursor_it = tf_cursors_.try_emplace(key).first;
    }

    // Step the topic's sparse UID sequence forward from the cursor. UIDs are
    // stable across front-eviction and per-topic SPARSE (process-global
    // allocation), so this can never skip an un-ingested entry the way a raw
    // index window did under concurrent eviction, and equal-timestamp entries
    // get distinct UIDs so a late same-stamp arrival is still reached. Each step
    // re-resolves under a fresh series lock (mirrors PR #179's entities path); an
    // entry evicted between the UID step and the at() resolve is simply gone.
    TfCursor& cursor = cursor_it->second;
    for (PJ::SequentialUID uid = object_store.nextUIDAfter(topic_id, cursor.last_ingested); uid.valid();
         uid = object_store.nextUIDAfter(topic_id, uid)) {
      cursor.last_ingested = uid;
      auto entry = object_store.at(topic_id, uid);
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;  // evicted between the UID step and the resolve, or empty payload
      }
      ingestEntry(entry->timestamp, entry->payload, parser_binding, *tf_buffer, stats);
    }
  }

  if (stats.dropped_reparent != 0 || stats.dropped_self_loop != 0 || stats.dropped_invalid != 0) {
    qCWarning(lcTransformService) << "ingestNewerThanCursor" << dataset_id << ": dropped" << stats.dropped_reparent
                                  << "reparent-conflict," << stats.dropped_self_loop << "self-loop, and"
                                  << stats.dropped_invalid << "invalid edge(s)";
  }
  return stats.ingested != 0;
}

}  // namespace pj::scene3d
