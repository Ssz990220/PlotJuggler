// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/transform_service.h"

#include <QLoggingCategory>
#include <algorithm>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
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
};

// Decode one object entry already known to belong to a FrameTransforms topic and
// push each of its edges into the buffer. A malformed edge (reparent conflict /
// self-loop) is a recoverable data error in a real bag: count it and keep going
// rather than aborting.
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
    if (const auto result = tf_buffer.setTransform(st); result.has_value()) {
      ++stats.ingested;
    } else if (result.error() == SetTransformError::ReparentConflict) {
      ++stats.dropped_reparent;
    } else {
      ++stats.dropped_self_loop;
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
  // Keep the full history: ingestFrameTransformsForDataset() bulk-loads the
  // entire dataset's TF up front, so a rolling cache window would trim every
  // dynamic edge to its last samples and make objects in dynamic frames (e.g. a
  // local costmap in `odom`) resolve only near the end of the timeline.
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  transform_buffers_[dataset_id] = buf;
  return buf;
}

void TransformService::invalidateDataset(PJ::DatasetId dataset_id) {
  // Cursor-model equivalent of forgetting the old transforms_populated_ flag:
  // drop the per-topic ingest cursors (and not-a-TF classifications) for this
  // dataset's topics so the next ingest re-reads their TF history from scratch
  // into the cleared buffer. Best-effort by listTopics — if the dataset was
  // evicted its topics are already gone, leaving only harmless dead cursor keys.
  for (const auto& topic_id : session_.objectStore().listTopics(dataset_id)) {
    tf_cursors_.erase(topic_id.id);
    non_tf_topics_.erase(topic_id.id);
  }
  if (auto it = transform_buffers_.find(dataset_id); it != transform_buffers_.end()) {
    // Clear in place: 3D docks hold this buffer by shared_ptr, so swapping the
    // map entry would leave them rendering the stale orphan forever.
    it->second->clear();
    qCInfo(lcTransformService) << "invalidateDataset" << dataset_id << ": TF buffer cleared";
  }
}

void TransformService::invalidateAll() {
  tf_cursors_.clear();
  non_tf_topics_.clear();
  for (auto& [dataset_id, buffer] : transform_buffers_) {
    (void)dataset_id;
    buffer->clear();
  }
  if (!transform_buffers_.empty()) {
    qCInfo(lcTransformService) << "invalidateAll:" << transform_buffers_.size() << "TF buffer(s) cleared";
  }
}

void TransformService::ingestFrameTransformsForDataset(PJ::DatasetId dataset_id) {
  // Bulk path: every cursor starts at INT64_MIN, so this ingests the whole
  // history in one pass (file load); ingestNewerThanCursor creates the buffer.
  // datasetTransformsReady tells 3D docks the tree is ready to render.
  const bool changed = ingestNewerThanCursor(dataset_id);
  qCInfo(lcTransformService) << "ingestFrameTransformsForDataset" << dataset_id << ": bulk ingest, changed=" << changed;
  emit datasetTransformsReady(dataset_id);
}

bool TransformService::ingestNewTransforms(PJ::DatasetId dataset_id) {
  return ingestNewerThanCursor(dataset_id);
}

bool TransformService::ingestNewerThanCursor(PJ::DatasetId dataset_id) {
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
      if (!probe_obj.has_value() ||
          PJ::sdk::typeOf(probe_obj->object) != PJ::sdk::BuiltinObjectType::kFrameTransforms) {
        non_tf_topics_.insert(key);
        continue;
      }
      cursor_it = tf_cursors_.try_emplace(key).first;
    }

    TfCursor& cursor = cursor_it->second;
    // indexAt is at-or-before, so it lands ON the cursor timestamp (or its last
    // duplicate). Back up over the contiguous run of entries at exactly the
    // cursor timestamp: a late equal-stamp arrival appends into that run, and we
    // must re-scan it to find the ones past count_at_timestamp. On first
    // classification the cursor timestamp is INT64_MIN, so indexAt is nullopt and
    // begin == 0 — the whole history is ingested.
    const auto start = object_store.indexAt(topic_id, cursor.timestamp);
    std::size_t begin = start.has_value() ? *start + 1 : 0;
    while (begin > 0) {
      auto prev = object_store.at(topic_id, begin - 1);
      if (!prev.has_value() || prev->timestamp != cursor.timestamp) {
        break;
      }
      --begin;
    }

    PJ::Timestamp newest_seen = cursor.timestamp;
    std::size_t count_at_newest = cursor.count_at_timestamp;
    std::size_t passed_at_cursor_ts = 0;  // entries == cursor.timestamp walked this scan
    const std::size_t end = object_store.entryCount(topic_id);
    for (std::size_t i = begin; i < end; ++i) {
      auto entry = object_store.at(topic_id, i);
      if (!entry.has_value() || entry->payload.bytes.empty() || entry->timestamp < cursor.timestamp) {
        continue;
      }
      if (entry->timestamp == cursor.timestamp && passed_at_cursor_ts++ < cursor.count_at_timestamp) {
        continue;  // already ingested on an earlier tick
      }
      ingestEntry(entry->timestamp, entry->payload, parser_binding, *tf_buffer, stats);
      if (entry->timestamp > newest_seen) {
        newest_seen = entry->timestamp;
        count_at_newest = 1;
      } else {  // == newest_seen (entries are timestamp-sorted, so never <)
        ++count_at_newest;
      }
    }
    cursor.timestamp = newest_seen;
    cursor.count_at_timestamp = count_at_newest;
  }

  if (stats.ingested != 0) {
    qCDebug(lcTransformService) << "ingestNewerThanCursor" << dataset_id << ": +" << stats.ingested << "transform(s)";
  }
  if (stats.dropped_reparent != 0 || stats.dropped_self_loop != 0) {
    qCWarning(lcTransformService) << "ingestNewerThanCursor" << dataset_id << ": dropped" << stats.dropped_reparent
                                  << "reparent-conflict and" << stats.dropped_self_loop << "self-loop edge(s)";
  }
  return stats.ingested != 0;
}

}  // namespace pj::scene3d
