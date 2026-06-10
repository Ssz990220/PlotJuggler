// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/scene_decoder_layer.h"

#include <memory>
#include <utility>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_core/scene_pipeline_source.h"

namespace PJ {

SceneDecoderLayer::SceneDecoderLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, const QString& kind_label,
    std::string_view schema, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, kind_label, parent), schema_(schema) {}

std::unique_ptr<MediaSource> SceneDecoderLayer::createMediaSource(const SceneLayerContext& /*ctx*/) {
  auto* store = objectStore();
  if (store == nullptr) {
    return nullptr;
  }
  auto decoder = makeSceneDecoder(schema_);
  if (decoder == nullptr) {
    return nullptr;
  }
  // Topics produced by a message parser (e.g. yolo_msgs/DetectionArray, markers)
  // store the RAW source message under pure-lazy ingest; hand the parser to the
  // source so it converts raw -> canonical before decoding. Topics whose loader
  // writes canonical bytes directly have no parser and decode them as-is.
  if (auto* session = sessionManager(); session != nullptr) {
    if (auto* parser = session->parserForObjectTopic(topicId()); parser != nullptr) {
      return std::make_unique<ScenePipelineSource>(
          store, topicId(), parser, session->parserMutexForObjectTopic(topicId()), std::move(decoder));
    }
  }
  return std::make_unique<ScenePipelineSource>(store, topicId(), std::move(decoder));
}

}  // namespace PJ
