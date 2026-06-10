// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

#include <QBoxLayout>
#include <QSizePolicy>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/borrowed_media_source.h"
#include "pj_scene2d_core/composite_media_source.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_widgets/layers/depth_image_layer.h"
#include "pj_scene2d_widgets/layers/image_layer.h"
#include "pj_scene2d_widgets/layers/scene2d_layer.h"
#include "pj_scene2d_widgets/layers/scene_decoder_layer.h"
#ifdef PJ_HAS_FFMPEG
#include "pj_scene2d_widgets/layers/video_layer.h"
#endif
#include "pj_scene2d_widgets/media_viewer_widget.h"

namespace PJ {

namespace {

using LayerCreator = std::unique_ptr<ISceneLayer> (*)(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name);

struct LayerRegistration {
  sdk::BuiltinObjectType object_type;
  LayerCreator creator;
};

std::unique_ptr<ISceneLayer> createImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<ImageLayer>(topic_id, object_type, display_name);
}

#ifdef PJ_HAS_FFMPEG
std::unique_ptr<ISceneLayer> createVideoLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<VideoLayer>(topic_id, object_type, display_name);
}
#endif

std::unique_ptr<ISceneLayer> createDepthImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<DepthImageLayer>(topic_id, object_type, display_name);
}

std::unique_ptr<ISceneLayer> createImageAnnotationsLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<SceneDecoderLayer>(
      topic_id, object_type, display_name, QStringLiteral("Annotations"), kSchemaImageAnnotations);
}

std::unique_ptr<ISceneLayer> createSceneEntitiesLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<SceneDecoderLayer>(
      topic_id, object_type, display_name, QStringLiteral("Markers"), kSchemaSceneEntities);
}

#ifdef PJ_HAS_FFMPEG
constexpr std::array<LayerRegistration, 5> kLayerRegistrations {
  {
#else
constexpr std::array<LayerRegistration, 4> kLayerRegistrations{{
#endif
    {sdk::BuiltinObjectType::kImage, &createImageLayer},
#ifdef PJ_HAS_FFMPEG
        {sdk::BuiltinObjectType::kVideoFrame, &createVideoLayer},
#endif
        {sdk::BuiltinObjectType::kDepthImage, &createDepthImageLayer},
        {sdk::BuiltinObjectType::kImageAnnotations, &createImageAnnotationsLayer},
        {sdk::BuiltinObjectType::kSceneEntities, &createSceneEntitiesLayer},
  }
};

}  // namespace

Scene2DDockWidget::Scene2DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("2D View"));

  for (const LayerRegistration& registration : kLayerRegistrations) {
    layerFactory().registerType(registration.object_type, registration.creator);
  }
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

  // Forces Qt 6.8 to create an RHI-backed window backing store before first show();
  // dynamically added QRhiWidgets otherwise never get a QRhi. See TECHNICAL_NOTES.md.
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
  return std::any_of(kLayerRegistrations.begin(), kLayerRegistrations.end(), [object_type](const auto& registration) {
    return registration.object_type == object_type;
  });
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
    // Tag the layer with its underlying source pointer so the rebuilt composite
    // can inherit this layer's last frame from the outgoing one (carry-over
    // below), instead of starting blank and forcing a re-decode.
    MediaSource* underlying = scene2d_layer->mediaSource();
    next->addLayer(std::make_unique<BorrowedMediaSource>(underlying), 1.0f, underlying);
    composite_topic_order_.push_back(info.topic_id);
  }

  // Carry the persisting layers' current frame into the rebuilt composite so the
  // viewer keeps showing them across the swap instead of going black until each
  // source re-decodes (the "black until play" symptom on add/remove/hide).
  if (composite_ != nullptr) {
    next->adoptContributions(*composite_);
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
        if (live) {
          driveVisibleLayersToLiveEdge();
        }
      });
}

void Scene2DDockWidget::driveVisibleLayersToLiveEdge() {
  if (sessionManager() == nullptr) {
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
      layer->setTrackerTime(PJ::fromRaw(latest));
    }
  }
  if (composite_ != nullptr) {
    composite_->setTimestamp(latest);
  }
  refreshView();
}

void Scene2DDockWidget::syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers) {
  if (composite_ == nullptr) {
    return;
  }
  // A rebuild rewraps the same underlying sources, which still hold their
  // per-timestamp dedup; invalidate first so the re-seed below re-decodes at the
  // (usually unchanged) current time instead of leaving a stale/empty frame
  // until the tracker next moves (the "black until play" symptom).
  composite_->invalidate();
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
