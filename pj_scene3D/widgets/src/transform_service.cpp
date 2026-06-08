// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/transform_service.h"

#include <QLoggingCategory>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/parse_locked.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcTransformService, "pj.scene3d.transform_service")
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

void TransformService::ingestFrameTransformsForDataset(PJ::DatasetId dataset_id) {
  if (transforms_populated_.contains(dataset_id)) {
    qCInfo(lcTransformService) << "ingestFrameTransformsForDataset" << dataset_id << ": already populated, skipping";
    return;
  }
  PJ::ObjectStore& object_store = session_.objectStore();
  auto tf_buffer = transformBuffer(dataset_id);  // creates if missing
  std::size_t ingested = 0;
  std::size_t tf_topics = 0;
  std::size_t dropped_reparent = 0;   // child claimed by two parents
  std::size_t dropped_self_loop = 0;  // child == parent

  for (const auto& topic_id : object_store.listTopics(dataset_id)) {
    auto* parser = session_.parserForObjectTopic(topic_id);
    if (parser == nullptr) {
      continue;
    }
    auto parser_mutex = session_.parserMutexForObjectTopic(topic_id);
    const auto count = object_store.entryCount(topic_id);
    if (count == 0) {
      continue;
    }
    // Probe the first entry: if parseObject returns FrameTransforms,
    // this topic is TF and we ingest every entry.
    auto first = object_store.at(topic_id, 0);
    if (!first.has_value() || first->payload.bytes.empty()) {
      continue;
    }
    auto probe_obj = parseLocked(parser, parser_mutex, first->timestamp, first->payload);
    if (!probe_obj.has_value() || PJ::sdk::typeOf(probe_obj->object) != PJ::sdk::BuiltinObjectType::kFrameTransforms) {
      continue;
    }
    ++tf_topics;

    // No static/dynamic inference: the TransformBuffer stores every edge as a
    // nearest-previous history, so a once-published transform (/tf_static, or any
    // namespaced *_static topic) resolves at all later times on its own. This
    // drops the old `desc.topic_name == "/tf_static"` exact-string match, which
    // silently mislabelled namespaced static topics (e.g. /robot1/tf_static) as
    // dynamic and orphaned their frames before the first timestamp.
    for (std::size_t i = 0; i < count; ++i) {
      auto entry = object_store.at(topic_id, i);
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;
      }
      auto obj = parseLocked(parser, parser_mutex, entry->timestamp, entry->payload);
      if (!obj.has_value()) {
        continue;
      }
      const auto* ft = std::any_cast<PJ::sdk::FrameTransforms>(&obj->object);
      if (ft == nullptr) {
        continue;
      }
      for (const auto& t : ft->transforms) {
        StampedTransform st;
        st.stamp = TimePoint{std::chrono::nanoseconds(t.timestamp)};
        st.parent_frame = t.parent_frame_id;
        st.child_frame = t.child_frame_id;
        st.transform.t = glm::dvec3{t.translation.x, t.translation.y, t.translation.z};
        st.transform.q = glm::dquat{t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
        // A malformed edge (reparent conflict / self-loop) is a recoverable data
        // error in a real bag: count it and keep ingesting the rest rather than
        // aborting the whole dataset load.
        if (const auto result = tf_buffer->setTransform(st); result.has_value()) {
          ++ingested;
        } else if (result.error() == SetTransformError::ReparentConflict) {
          ++dropped_reparent;
        } else {
          ++dropped_self_loop;
        }
      }
    }
  }
  transforms_populated_.insert(dataset_id);
  qCInfo(lcTransformService) << "ingestFrameTransformsForDataset" << dataset_id << ": ingested" << ingested
                             << "transforms from" << tf_topics << "TF topic(s)";
  if (dropped_reparent != 0 || dropped_self_loop != 0) {
    qCWarning(lcTransformService) << "ingestFrameTransformsForDataset" << dataset_id << ": dropped" << dropped_reparent
                                  << "reparent-conflict and" << dropped_self_loop << "self-loop edge(s)";
  }
  emit datasetTransformsReady(dataset_id);
}

}  // namespace pj::scene3d
