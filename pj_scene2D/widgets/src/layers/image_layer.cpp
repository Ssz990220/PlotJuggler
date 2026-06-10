// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/image_layer.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>
#include <any>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "pj_base/builtin/asset_video.hpp"
#include "pj_base/builtin/asset_video_codec.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/image_pipeline_source.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_widgets/scene2d_pipelines.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene2DImageLayer, "pj.scene2d.layer.image")

bool topicUsesCanonicalImageCodec(const std::string& metadata_json) {
  const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(metadata_json));
  if (!doc.isObject()) {
    if (!metadata_json.empty()) {
      qCWarning(lcScene2DImageLayer) << "topic metadata is not a valid JSON object; cannot detect image_codec";
    }
    return false;
  }
  return doc.object().value(QStringLiteral("image_codec")).toString() == QStringLiteral("pj_image_v1");
}

// Build a frame_id -> CameraInfo map by running each "<ns>/camera_info" topic's
// MessageParser on its latest sample. The ObjectStore keeps raw message bytes
// (e.g. foxglove.CameraCalibration) and decodes lazily through the topic's
// parser — so calibration must be obtained the same way an image is, not by
// deserializing the raw bytes as a canonical CameraInfo. The decode worker then
// uses this map (matched by sdk::Image.frame_id) to rectify the camera frames.
std::unordered_map<std::string, sdk::CameraInfo> collectCameraInfoByFrameId(
    SessionManager* session, ObjectStore* store) {
  std::unordered_map<std::string, sdk::CameraInfo> by_frame;
  constexpr std::string_view kSuffix = "/camera_info";
  for (const ObjectTopicId id : store->listTopics()) {
    const std::string& name = store->descriptor(id).topic_name;
    if (name.size() < kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      continue;
    }
    if (store->entryCount(id) == 0) {
      continue;
    }
    auto* ci_parser = session->parserForObjectTopic(id);
    if (ci_parser == nullptr) {
      continue;
    }
    const auto entry = store->latestAt(id, store->timeRange(id).second);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;
    }
    const sdk::PayloadView payload = entry->payload;
    // MessageParser plugins aren't thread-safe; serialize via the shared mutex,
    // exactly as ImagePipelineSource does for the image parser.
    const auto mutex = session->parserMutexForObjectTopic(id);
    auto record = [&] {
      if (mutex) {
        const std::lock_guard<std::mutex> lock(*mutex);
        return ci_parser->parseObject(entry->timestamp, payload);
      }
      return ci_parser->parseObject(entry->timestamp, payload);
    }();
    if (!record.has_value() || sdk::typeOf(record->object) != sdk::BuiltinObjectType::kCameraInfo) {
      continue;
    }
    const auto* ci = std::any_cast<sdk::CameraInfo>(&record->object);
    if (ci != nullptr && !ci->frame_id.empty()) {
      by_frame.insert_or_assign(ci->frame_id, *ci);
    }
  }
  return by_frame;
}
}  // namespace

ImageLayer::ImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, QStringLiteral("Image"), parent) {}

std::unique_ptr<MediaSource> ImageLayer::createMediaSource(const SceneLayerContext& ctx) {
  auto* session = ctx.session;
  auto* store = objectStore();
  if (session == nullptr || store == nullptr) {
    return nullptr;
  }

  if (objectType() != sdk::BuiltinObjectType::kImage) {
    return nullptr;
  }

  auto pipeline = makeScene2DPipelineFor(objectType());
  const auto parser_binding = session->parserBindingForObjectTopic(topicId());
  auto* parser = parser_binding.parser;
  const bool canonical_blob =
      parser == nullptr && topicUsesCanonicalImageCodec(store->descriptor(topicId()).metadata_json);
  if (parser == nullptr && !canonical_blob && pipeline == nullptr) {
    qCWarning(lcScene2DImageLayer) << "no parser and no built-in image pipeline for topic_id=" << topicId().id;
    return nullptr;
  }

  std::unique_ptr<ImagePipelineSource> image_src;
  if (parser != nullptr) {
    image_src =
        std::make_unique<ImagePipelineSource>(store, topicId(), parser, parser_binding.mutex, parser_binding.keepalive);
  } else if (canonical_blob) {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), ImagePipelineSource::CanonicalImageCodec{});
  } else {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), std::move(pipeline));
  }

  // Supply camera calibration (parsed via each camera_info topic's parser) so the
  // decode worker can rectify frames to native resolution and 2D annotation
  // overlays line up. Built once here, before the first setTimestamp() request:
  // sufficient for file load (all camera_info is ingested before the layer
  // attaches); a camera_info published after attach is not retro-applied.
  image_src->setCameraInfoMap(collectCameraInfoByFrameId(session, store));

  image_src->setFrameReadyCallback(makeQueuedRepaintCallback());
  return image_src;
}

}  // namespace PJ
