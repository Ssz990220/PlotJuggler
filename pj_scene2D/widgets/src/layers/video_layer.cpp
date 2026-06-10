// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/video_layer.h"

#include <QLoggingCategory>
#include <memory>
#include <utility>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/streaming_video_source.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene2DVideoLayer, "pj.scene2d.layer.video")
}  // namespace

VideoLayer::VideoLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, QStringLiteral("Video"), parent) {}

std::unique_ptr<MediaSource> VideoLayer::createMediaSource(const SceneLayerContext& ctx) {
  auto* session = ctx.session;
  auto* store = objectStore();
  if (session == nullptr || store == nullptr) {
    return nullptr;
  }

  const auto parser_binding = session->parserBindingForObjectTopic(topicId());
  if (!parser_binding) {
    qCWarning(lcScene2DVideoLayer) << "kVideoFrame topic_id=" << topicId().id
                                   << "has no live parser registered — cannot unwrap VideoFrame messages";
    return nullptr;
  }

  auto video_src = std::make_unique<StreamingVideoSource>(
      store, topicId(), parser_binding.parser, parser_binding.mutex, parser_binding.keepalive);

  video_src->setFrameReadyCallback(makeQueuedRepaintCallback());
  return video_src;
}

}  // namespace PJ
