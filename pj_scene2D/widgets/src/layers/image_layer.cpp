// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/image_layer.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <memory>
#include <string>
#include <utility>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/image_pipeline_source.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

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

  auto pipeline = Scene2DDockWidget::makePipelineFor(objectType());
  auto* parser = session->parserForObjectTopic(topicId());
  const bool canonical_blob =
      parser == nullptr && topicUsesCanonicalImageCodec(store->descriptor(topicId()).metadata_json);
  if (parser == nullptr && !canonical_blob && pipeline == nullptr) {
    qCWarning(lcScene2DImageLayer) << "no parser and no built-in image pipeline for topic_id=" << topicId().id;
    return nullptr;
  }

  std::unique_ptr<ImagePipelineSource> image_src;
  if (parser != nullptr) {
    // Parser-mode: the decode worker calls parser->parseObject off the GUI
    // thread, so pin the parser instance + plugin DSO via the keepalive for the
    // worker's whole lifetime (mirrors VideoLayer). A null keepalive paired with
    // a non-null parser means the topic was unregistered between the two lookups
    // — refuse rather than hand the worker a parser that could be dlclosed under
    // an in-flight call.
    auto parser_keepalive = session->parserKeepaliveForObjectTopic(topicId());
    if (parser_keepalive == nullptr) {
      qCWarning(lcScene2DImageLayer) << "kImage topic_id=" << topicId().id
                                     << "has no live parser keepalive — cannot safely decode image messages";
      return nullptr;
    }
    image_src = std::make_unique<ImagePipelineSource>(
        store, topicId(), parser, session->parserMutexForObjectTopic(topicId()), std::move(parser_keepalive));
  } else if (canonical_blob) {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), ImagePipelineSource::CanonicalImageCodec{});
  } else {
    image_src = std::make_unique<ImagePipelineSource>(store, topicId(), std::move(pipeline));
  }

  image_src->setFrameReadyCallback([qp = QPointer<ImageLayer>(this)]() {
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
  return image_src;
}

}  // namespace PJ
