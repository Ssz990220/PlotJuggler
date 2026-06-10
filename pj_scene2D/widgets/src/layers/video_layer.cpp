// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/video_layer.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
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

  // Per-frame VideoFrame entries are wrapped canonical messages; only the
  // topic's parser can unwrap them to the Annex-B NAL span. Fetch the parser AND
  // its keepalive together: the keepalive pins the parser instance + plugin DSO
  // for the decode worker's whole lifetime (the worker calls parseObject off the
  // GUI thread). A null keepalive paired with a non-null parser means the topic
  // was unregistered between the two lookups — refuse rather than hand the worker
  // a parser that could be dlclosed underneath an in-flight call.
  auto* parser = session->parserForObjectTopic(topicId());
  auto parser_keepalive = session->parserKeepaliveForObjectTopic(topicId());
  if (parser == nullptr || parser_keepalive == nullptr) {
    qCWarning(lcScene2DVideoLayer) << "kVideoFrame topic_id=" << topicId().id
                                   << "has no live parser registered — cannot unwrap VideoFrame messages";
    return nullptr;
  }

  auto video_src = std::make_unique<StreamingVideoSource>(
      store, topicId(), parser, session->parserMutexForObjectTopic(topicId()), std::move(parser_keepalive));

  // Decode is asynchronous on the source's worker thread, so a single takeFrame()
  // right after a scrub races the worker. Re-poll on the GUI thread when the
  // worker signals a fresh frame (mirrors ImageLayer). The callback fires FROM
  // the worker thread, so hop via QueuedConnection; QPointer guards teardown.
  video_src->setFrameReadyCallback([qp = QPointer<VideoLayer>(this)]() {
    if (!qp) {
      return;
    }
    QMetaObject::invokeMethod(
        qp.data(),
        [qp]() {
          if (qp) {
            emit qp->repaintRequested();
          }
        },
        Qt::QueuedConnection);
  });
  return video_src;
}

}  // namespace PJ
