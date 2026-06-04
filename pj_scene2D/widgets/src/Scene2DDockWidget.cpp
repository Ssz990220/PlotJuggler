// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

#include <QBoxLayout>
#include <QMetaObject>
#include <QPointer>
#include <QSizePolicy>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/borrowed_media_source.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/composite_media_source.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_widgets/layers/depth_image_layer.h"
#include "pj_scene2d_widgets/layers/image_layer.h"
#include "pj_scene2d_widgets/layers/scene2d_layer.h"
#include "pj_scene2d_widgets/layers/scene_decoder_layer.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"

namespace PJ {

namespace {}  // namespace

Scene2DDockWidget::Scene2DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("2D View"));

  layerFactory().registerType(
      sdk::BuiltinObjectType::kImage,
      [](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<ImageLayer>(topic_id, object_type, display_name);
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kAssetVideo,
      [](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<ImageLayer>(topic_id, object_type, display_name);
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kDepthImage,
      [](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<DepthImageLayer>(topic_id, object_type, display_name);
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kImageAnnotations,
      [](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<SceneDecoderLayer>(
            topic_id, object_type, display_name, QStringLiteral("Annotations"), kSchemaImageAnnotations);
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kSceneEntities,
      [](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<SceneDecoderLayer>(
            topic_id, object_type, display_name, QStringLiteral("Markers"), kSchemaSceneEntities);
      });
}

Scene2DDockWidget::~Scene2DDockWidget() {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
  }
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(nullptr);
  }
  composite_.reset();
}

void Scene2DDockWidget::setSessionManager(SessionManager* session) {
  SceneDockWidget::setSessionManager(session);
  reconnectLiveSamples(session);
}

bool Scene2DDockWidget::setImageTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  const bool accepted = addTopic(topic_id, object_type, title);
  if (accepted) {
    setWindowTitle(title.isEmpty() ? tr("2D View") : tr("2D View - %1").arg(title));
  }
  return accepted;
}

void Scene2DDockWidget::setPointInspectorEnabled(bool enabled) {
  if (viewer_ != nullptr) {
    viewer_->setPointInspectorEnabled(enabled);
  }
}

bool Scene2DDockWidget::pointInspectorEnabled() const noexcept {
  return viewer_ != nullptr && viewer_->pointInspectorEnabled();
}

std::unique_ptr<CodecPipeline> Scene2DDockWidget::makePipelineFor(sdk::BuiltinObjectType object_type) {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
      return makeJpegPipeline();
    case sdk::BuiltinObjectType::kDepthImage:
      return nullptr;
    case sdk::BuiltinObjectType::kNone:
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kFrameTransforms:
    case sdk::BuiltinObjectType::kOccupancyGrid:
    case sdk::BuiltinObjectType::kCompressedPointCloud:
    case sdk::BuiltinObjectType::kMesh3D:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kSceneEntities:
    case sdk::BuiltinObjectType::kAssetVideo:
    case sdk::BuiltinObjectType::kRobotDescription:
    case sdk::BuiltinObjectType::kCameraInfo:
    case sdk::BuiltinObjectType::kOccupancyGridUpdate:
    case sdk::BuiltinObjectType::kLog:
      return nullptr;
  }
  return nullptr;
}

size_t Scene2DDockWidget::compositeLayerCountForTesting() const noexcept {
  return composite_ != nullptr ? composite_->layerCount() : 0U;
}

std::vector<ObjectTopicId> Scene2DDockWidget::compositeTopicOrderForTesting() const {
  return composite_topic_order_;
}

QString Scene2DDockWidget::xmlTag() const {
  return QStringLiteral("scene2d");
}

QWidget* Scene2DDockWidget::createSceneView() {
  auto* container = new QWidget(this);
  container->setContentsMargins(0, 0, 0, 0);

  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  bootstrap_ = new MediaViewerWidget(container);
  bootstrap_->setMaximumSize(0, 0);
  layout->addWidget(bootstrap_);

  viewer_ = new MediaViewerWidget(container);
  viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(viewer_);
  if (composite_ != nullptr) {
    viewer_->setMediaSource(composite_.get());
  }

  return container;
}

std::unique_ptr<SceneLayerContext> Scene2DDockWidget::makeContext() {
  auto context = std::make_unique<SceneLayerContext>();
  context->session = sessionManager();
  return context;
}

bool Scene2DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
    case sdk::BuiltinObjectType::kAssetVideo:
    case sdk::BuiltinObjectType::kDepthImage:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kSceneEntities:
      return true;
    default:
      return false;
  }
}

bool Scene2DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  return handlesObjectType(object_type);
}

void Scene2DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  auto next = std::make_unique<CompositeMediaSource>();
  composite_topic_order_.clear();

  for (ISceneLayer* layer : ordered_layers) {
    if (layer == nullptr) {
      continue;
    }
    const auto info = layer->info();
    if (!info.visible) {
      continue;
    }
    auto* scene2d_layer = dynamic_cast<Scene2DLayer*>(layer);
    if (scene2d_layer == nullptr || scene2d_layer->mediaSource() == nullptr) {
      continue;
    }
    next->addLayer(std::make_unique<BorrowedMediaSource>(scene2d_layer->mediaSource()));
    composite_topic_order_.push_back(info.topic_id);
  }

  // Repoint the viewer at the new composite BEFORE the previous one is freed,
  // so the viewer never holds a dangling MediaSource — not even transiently
  // between the swap and the setMediaSource call.
  auto previous = std::move(composite_);
  composite_ = std::move(next);
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(composite_.get());
  }
  // `previous` is destroyed here, after the viewer no longer references it.
  syncCompositeTimestamp(ordered_layers);
  refreshView();
}

void Scene2DDockWidget::refreshView() {
  if (viewer_ != nullptr) {
    viewer_->update();
  }
}

void Scene2DDockWidget::reconnectLiveSamples(SessionManager* session) {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
    live_samples_conn_ = {};
  }
  if (session == nullptr) {
    return;
  }
  live_samples_conn_ =
      connect(session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (!live || sessionManager() == nullptr) {
          return;
        }

        ObjectStore& store = sessionManager()->objectStore();
        bool any = false;
        int64_t latest = std::numeric_limits<int64_t>::lowest();
        for (const SceneLayerInfo& info : layers()) {
          if (!info.visible || store.entryCount(info.topic_id) == 0) {
            continue;
          }
          latest = std::max(latest, store.timeRange(info.topic_id).second);
          any = true;
        }
        if (!any) {
          return;
        }

        noteTrackerTime(latest);
        for (const SceneLayerInfo& info : layers()) {
          ISceneLayer* layer = layerFor(info.topic_id);
          if (layer != nullptr && info.visible) {
            layer->setTrackerTime(std::chrono::nanoseconds{latest});
          }
        }
        if (composite_ != nullptr) {
          composite_->setTimestamp(latest);
        }
        refreshView();
      });
}

void Scene2DDockWidget::syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers) {
  if (composite_ == nullptr) {
    return;
  }
  const auto seed = seedTimestampNs(ordered_layers);
  if (seed.has_value()) {
    composite_->setTimestamp(*seed);
  }
}

std::optional<int64_t> Scene2DDockWidget::seedTimestampNs(const std::vector<ISceneLayer*>& ordered_layers) const {
  if (const auto ns = lastTrackerNs(); ns.has_value()) {
    return ns;
  }
  for (ISceneLayer* layer : ordered_layers) {
    auto* scene2d_layer = dynamic_cast<Scene2DLayer*>(layer);
    if (scene2d_layer == nullptr || !scene2d_layer->info().visible) {
      continue;
    }
    const auto last = scene2d_layer->lastTrackerTimeNs();
    if (last.has_value()) {
      return last;
    }
  }
  return std::nullopt;
}

}  // namespace PJ
