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

#include "pj_base/builtin/asset_video.hpp"
#include "pj_base/builtin/asset_video_codec.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/file_video_source.h"
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

  if (objectType() == sdk::BuiltinObjectType::kAssetVideo) {
    if (store->entryCount(topicId()) == 0) {
      qCWarning(lcScene2DImageLayer) << "kAssetVideo topic_id=" << topicId().id << "has no entries";
      return nullptr;
    }
    const auto entry = store->at(topicId(), 0);
    if (!entry.has_value() || entry->payload.anchor == nullptr) {
      qCWarning(lcScene2DImageLayer) << "kAssetVideo topic_id=" << topicId().id << "first entry has no payload";
      return nullptr;
    }
    auto asset = deserializeAssetVideo(entry->payload.bytes.data(), entry->payload.bytes.size());
    if (!asset.has_value()) {
      qCWarning(lcScene2DImageLayer) << "deserializeAssetVideo failed for topic_id=" << topicId().id << ":"
                                     << QString::fromStdString(asset.error());
      return nullptr;
    }
    auto src = FileVideoSource::open(asset->file_path);
    if (!src.has_value()) {
      qCWarning(lcScene2DImageLayer) << "FileVideoSource::open failed for" << QString::fromStdString(asset->file_path)
                                     << ":" << QString::fromStdString(src.error());
      return nullptr;
    }
    if (asset->time_origin_ns.has_value()) {
      (*src)->setEpochAnchorNs(*asset->time_origin_ns);
    }
    (*src)->setClipWindowNs(asset->start_ns, asset->end_ns);
    return std::move(*src);
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
    image_src =
        std::make_unique<ImagePipelineSource>(store, topicId(), parser, session->parserMutexForObjectTopic(topicId()));
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
