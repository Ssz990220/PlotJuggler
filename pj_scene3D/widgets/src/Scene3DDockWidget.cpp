// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/Scene3DDockWidget.h"

#include <QAbstractItemView>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QLoggingCategory>
#include <QPoint>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/SvgUtil.h"
#include "urdf_package_resolver.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene3DDock, "pj.scene3d.dock")

using pj::scene3d::DepthCloudLayer;
using pj::scene3d::FrameRow;
using pj::scene3d::OccupancyGridLayer;
using pj::scene3d::PointCloudLayer;
using pj::scene3d::PosesInFrameLayer;
using pj::scene3d::RobotModelLayer;
using pj::scene3d::Scene3DLayer;
using pj::scene3d::Scene3DLayerContext;
using pj::scene3d::SceneEntitiesLayer;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::VoxelGridLayer;

// Stable enum <-> on-disk-name table for the camera model, persisted in the
// layout. The enum value == combo index, so the on-disk name stays independent of
// the combo's display order; one table feeds both directions so they can't drift.
struct CameraModelName {
  SceneViewWidget::CameraModel model;
  const char* id;
};
constexpr CameraModelName kCameraModelNames[] = {
    {SceneViewWidget::CameraModel::kOrbit, "orbit"},
    {SceneViewWidget::CameraModel::kXyOrbit, "xy_orbit"},
    {SceneViewWidget::CameraModel::kFly, "fly"},
    {SceneViewWidget::CameraModel::kTopDownOrtho, "top_down_ortho"},
};

QString cameraModelToString(int combo_index) {
  const auto model = static_cast<SceneViewWidget::CameraModel>(combo_index);
  for (const auto& entry : kCameraModelNames) {
    if (entry.model == model) {
      return QString::fromLatin1(entry.id);
    }
  }
  return QStringLiteral("orbit");
}

// Combo index for a persisted model name, or -1 when missing / unknown (→ keep
// the default).
int cameraModelFromString(const QString& name) {
  for (const auto& entry : kCameraModelNames) {
    if (name == QLatin1String(entry.id)) {
      return static_cast<int>(entry.model);
    }
  }
  return -1;
}

[[nodiscard]] bool framesContain(const QList<FrameRow>& frames, const QString& name) {
  const auto needle = name.toStdString();
  return std::any_of(frames.begin(), frames.end(), [&](const FrameRow& r) { return r.name == needle; });
}

[[nodiscard]] QString pickFixedFrame(const QList<FrameRow>& frames) {
  for (const auto* name : {"map", "world", "odom", "base_link", "base_footprint"}) {
    if (framesContain(frames, QString::fromLatin1(name))) {
      return QString::fromLatin1(name);
    }
  }
  return frames.isEmpty() ? QString() : QString::fromStdString(frames.first().name);
}

// True when topic_id's first stored sample is an sdk::Image with a DEPTH encoding.
// The kImage type alone can't distinguish depth from color, so the dock peeks the
// first sample (the parser can't — classify_schema sees no payload) to offer only
// depth images as DepthClouds. False when there is no sample yet or it can't decode.
[[nodiscard]] bool firstSampleIsDepthEncoded(PJ::SessionManager& session, ObjectTopicId topic_id) {
  PJ::ObjectStore& store = session.objectStore();
  auto first = store.at(topic_id, static_cast<size_t>(0));
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = session.parserBindingForObjectTopic(topic_id);
  if (!binding) {
    return false;
  }
  auto obj = pj::scene3d::parseLocked(binding, first->timestamp, first->payload);
  if (!obj.has_value()) {
    return false;
  }
  const auto* image = std::any_cast<PJ::sdk::Image>(&obj->object);
  return image != nullptr && pj::scene3d::isDepthEncoding(image->encoding);
}

}  // namespace

Scene3DDockWidget::Scene3DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("3D View"));
  package_resolver_ = std::make_unique<pj::scene3d::UrdfPackageResolver>();

  // One dual-mode layer renders both raw and compressed clouds: PointCloudLayer
  // detects a CompressedPointCloud per-sample and decodes it (off the UI thread)
  // into the same render path, so both object types share this factory.
  auto pointcloud_factory = [this](
                                ObjectTopicId topic_id, sdk::BuiltinObjectType object_type,
                                const QString& display_name) -> std::unique_ptr<ISceneLayer> {
    prepareTransformBufferForTopic(topic_id);
    auto layer = std::make_unique<PointCloudLayer>(topic_id, display_name, object_type, this);
    wireScene3DLayer(layer.get());
    return layer;
  };
  layerFactory().registerType(sdk::BuiltinObjectType::kPointCloud, pointcloud_factory);
  layerFactory().registerType(sdk::BuiltinObjectType::kCompressedPointCloud, pointcloud_factory);
  // Depth image -> back-projected point cloud ("DepthCloud"). Depth arrives as
  // sdk::Image with a depth encoding (no kDepthImage producer exists); addTopic()
  // gates kImage topics so only depth-encoded ones become DepthClouds. Intrinsics
  // come from a CameraInfo topic matched by frame_id; see DepthCloudLayer.
  layerFactory().registerType(
      sdk::BuiltinObjectType::kImage,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<DepthCloudLayer>(topic_id, display_name, object_type, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kRobotDescription,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        const bool local_layer = isLocalRobotLayerId(topic_id);
        if (!local_layer) {
          prepareTransformBufferForTopic(topic_id);
        }
        auto layer = std::make_unique<RobotModelLayer>(topic_id, display_name, this);
        layer->setPackageResolver(package_resolver_.get());
        if (local_layer) {
          layer->setSourceFile(QString());
        }
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kOccupancyGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<OccupancyGridLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kSceneEntities,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<SceneEntitiesLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kPosesInFrame,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<PosesInFrameLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kVoxelGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<VoxelGridLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });

  frame_overlay_combo_ = new ComboBox(this);
  frame_overlay_combo_->setFocusPolicy(Qt::ClickFocus);
  frame_overlay_combo_->raise();
  refreshFrameOverlayCombo();
  connect(frame_overlay_combo_, &QComboBox::currentIndexChanged, this, &Scene3DDockWidget::onOverlayFramePicked);

  // Camera-model selector + Home button — same styled overlay control as the
  // fixed-frame combo (a pj_widgets ComboBox), anchored top-right just left of
  // the orientation gizmo. Children of `this` (NOT view_) so they layer above
  // the QOpenGLWidget without fighting ADS's native-window flags. addItems()
  // before connect() avoids a spurious callback into a not-yet-constructed view
  // during the ctor.
  camera_model_combo_ = new ComboBox(this);
  camera_model_combo_->setObjectName(QStringLiteral("cameraModelCombo"));
  camera_model_combo_->setFocusPolicy(Qt::ClickFocus);
  camera_model_combo_->addItems({tr("Orbit"), tr("XYOrbit"), tr("Fly"), tr("Top-down ortho")});
  camera_model_combo_->raise();
  connect(camera_model_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (view_ != nullptr && index >= 0) {
      view_->setCameraModel(static_cast<pj::scene3d::SceneViewWidget::CameraModel>(index));
      view_->update();
    }
  });

  home_button_ = new QToolButton(this);
  home_button_->setObjectName(QStringLiteral("cameraHomeButton"));
  home_button_->setFocusPolicy(Qt::ClickFocus);
  home_button_->setToolTip(tr("Reset view to default (Home)"));
  // Bundled Material "home" glyph (resources.qrc). Pinned to the light-theme
  // ink so it stays dark on this always-light overlay button, even when the
  // app is in dark mode (theme-following ink would render near-invisible here).
  home_button_->setIcon(PJ::loadSvg(QStringLiteral(":/resources/svg/home.svg")));
  home_button_->setStyleSheet(QStringLiteral(
      "QToolButton { background-color: rgba(255, 255, 255, 200); border: 1px solid rgba(60, 60, 60, 180); "
      "padding: 0px; border-radius: 3px; }"));  // no padding: let the glyph fill the button (icon sized below)
  home_button_->raise();
  connect(home_button_, &QToolButton::clicked, this, [this]() {
    if (view_ != nullptr) {
      view_->camera().reset();
      view_->update();
    }
  });

  // Forget a removed topic's cached orphan/warning state. pj_app drives its UI
  // off the base SceneDockWidget layer* signals directly, so no relay is needed.
  connect(this, &SceneDockWidget::layerRemoved, this, [this](ObjectTopicId topic_id) {
    orphan_states_.erase(topicKey(topic_id));
    local_robot_layer_ids_.erase(topic_id.id);
    scene_topic_datasets_.erase(topic_id.id);
    // A local robot layer carries no store dataset, so its removal never affects
    // the TF binding; a store-backed layer's removal might be the last topic of
    // the bound dataset, which frees the buffer for a rebind (M.17).
    resetTransformBindingIfDatasetGone();
  });
}

Scene3DDockWidget::~Scene3DDockWidget() {
  // Release the view's layer references — and their GL resources — while this
  // concrete class is still alive: clearLayers() reconciles the view through
  // syncViewLayers(), which the base destructor can no longer dispatch to us.
  // Without this, SceneViewWidget::layers_ would dangle over the destroyed
  // layers and releaseGlResources()/setLayers() would dereference freed
  // memory (and the layers' GL objects would die without a current context).
  clearLayers();
}

void Scene3DDockWidget::setTransformService(pj::scene3d::TransformService* service) {
  if (tf_ready_conn_) {
    QObject::disconnect(tf_ready_conn_);
    tf_ready_conn_ = {};
  }
  transform_service_ = service;
  if (view_ != nullptr && tf_buffer_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  if (transform_service_ != nullptr) {
    // A progressive file load folds FrameTransforms into the buffer incrementally
    // and emits this per flush. Re-resolve our layers against the new transforms at
    // the instant we are showing so the scene fills in as the file loads instead of
    // only at the end. Filter to our dataset; before any layer binds
    // representativeDatasetId() is 0 (no match, nothing to draw yet) and the dock
    // self-heals on the next signal once a layer resolves.
    tf_ready_conn_ = connect(
        transform_service_, &pj::scene3d::TransformService::datasetTransformsReady, this,
        [this](PJ::DatasetId dataset_id) {
          if (dataset_id == representativeDatasetId()) {
            onTrackerTime(last_tracker_display_);
          }
        });
  }
}

void Scene3DDockWidget::setSessionManager(SessionManager* session) {
  SceneDockWidget::setSessionManager(session);
  reconnectLiveSamples(session);
}

void Scene3DDockWidget::reconnectLiveSamples(SessionManager* session) {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
    live_samples_conn_ = {};
  }
  if (session == nullptr) {
    return;
  }
  // Streamed FrameTransform messages must be folded into the TF buffer as they
  // arrive: a file load is driven by FileLoader (ingestFrameTransformsForDataset
  // per flush -> datasetTransformsReady, handled in setTransformService), but a
  // live stream has no such driver, so without this slot the buffer stays empty
  // and every sensor frame is orphan (red). samplesIngested fires on the UI thread
  // after the retention trim, with live=true only while following a live stream —
  // file load emits live=false and is served by the FileLoader path instead.
  live_samples_conn_ =
      connect(session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (!live || transform_service_ == nullptr || tf_buffer_ == nullptr) {
          return;
        }
        transform_service_->ingestNewTransforms(dataset_id_);
        // Recompute orphan states only when the TF buffer actually changed
        // (force=false gates on revision()+fixed-frame): a sibling 3D dock sharing
        // this dataset may have advanced the shared ingest cursor, so we cannot
        // gate on our own ingest call's return value — but the buffer revision
        // catches any writer. driveVisibleLayersToLiveEdge() then advances the
        // object layers to the newest store data and repaints (itself a no-op when
        // the live edge hasn't moved).
        recomputeOrphanStates(/*force=*/false);
        driveVisibleLayersToLiveEdge();
      });
}

void Scene3DDockWidget::driveVisibleLayersToLiveEdge() {
  if (sessionManager() == nullptr) {
    return;
  }
  bool any = false;
  int64_t latest = std::numeric_limits<int64_t>::lowest();
  for (const SceneLayerInfo& info : layers()) {
    ISceneLayer* layer = layerFor(info.topic_id);
    if (!info.visible || layer == nullptr) {
      continue;
    }
    // Consult the layer's own timeRange() rather than store.timeRange(topic_id):
    // a multi-topic layer (OccupancyGrid + its _updates sibling) reports a live
    // edge that the base topic alone would miss, freezing it at the last
    // keyframe. Empty layers report an inverted range and are skipped.
    const PJ::Range<PJ::Timepoint> range = layer->timeRange();
    if (range.max < range.min) {
      continue;
    }
    latest = std::max(latest, PJ::toRaw(range.max));
    any = true;
  }
  // Fold config (TF) topics into the live edge so a layer-less TF-only dock still
  // advances + repaints every live tick (H.3). Without this, layers() being empty
  // bailed before any view update and the TF axes stayed frozen on the seed time.
  ObjectStore& store = sessionManager()->objectStore();
  for (const uint32_t topic_raw : config_topics_) {
    const ObjectTopicId topic_id{topic_raw};
    if (store.entryCount(topic_id) == 0) {
      continue;
    }
    latest = std::max(latest, store.timeRange(topic_id).second);
    any = true;
  }
  if (!any) {
    return;
  }
  // Skip when the edge hasn't moved: MainWindow's playhead path repaints per tick
  // regardless, so re-pushing the same time here is wasted work (L.45/L.46).
  if (const auto last_ns = lastTrackerNs(); last_ns.has_value() && *last_ns == latest) {
    return;
  }
  noteTrackerTime(latest);
  pushTrackerTimeToView(latest);
}

void Scene3DDockWidget::pushTrackerTimeToView(int64_t time_ns) {
  for (const SceneLayerInfo& info : layers()) {
    if (ISceneLayer* layer = layerFor(info.topic_id); layer != nullptr && info.visible) {
      layer->setTrackerTime(PJ::fromRaw(time_ns));
    }
  }
  if (view_ != nullptr) {
    view_->setTrackerTime(PJ::fromRaw(time_ns));
  }
  refreshView();  // refreshView() already unions scene bounds before repainting
  // This live-edge push painted OUTSIDE the per-tick gate, so the gate's
  // last-painted key now lags the pixels on screen. Force the next onTrackerTime()
  // to re-evaluate rather than trust a stale match (e.g. a tracker lagging the live
  // edge could otherwise coalesce away a needed correction).
  invalidateTrackerRenderKey();
}

void Scene3DDockWidget::setSettings(QSettings* settings) {
  settings_ = settings;
  if (package_resolver_ != nullptr) {
    package_resolver_->setSettings(settings_);
  }
}

void Scene3DDockWidget::setEmbeddedAssets(QMap<QString, QByteArray> assets) {
  embedded_assets_ = std::move(assets);
  if (package_resolver_ != nullptr) {
    package_resolver_->setEmbeddedAssets(embedded_assets_);
  }
}

void Scene3DDockWidget::setSourcePath(const QString& path) {
  source_path_ = path;
  if (package_resolver_ != nullptr) {
    package_resolver_->setSourcePath(source_path_);
  }
}

bool Scene3DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  return object_type == sdk::BuiltinObjectType::kPointCloud ||
         object_type == sdk::BuiltinObjectType::kCompressedPointCloud ||
         object_type == sdk::BuiltinObjectType::kFrameTransforms ||
         object_type == sdk::BuiltinObjectType::kOccupancyGrid ||
         object_type == sdk::BuiltinObjectType::kRobotDescription ||
         object_type == sdk::BuiltinObjectType::kSceneEntities ||
         object_type == sdk::BuiltinObjectType::kPosesInFrame || object_type == sdk::BuiltinObjectType::kVoxelGrid ||
         // kImage is accepted only for DEPTH-encoded images; addTopic() peeks the
         // first sample's encoding and rejects color images (which share kImage).
         object_type == sdk::BuiltinObjectType::kImage;
}

bool Scene3DDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // Interactive add (drop / family switch): enforce the kImage depth-encoding gate.
  return addTopicImpl(topic_id, object_type, title, /*enforce_image_gate=*/true);
}

bool Scene3DDockWidget::addTopicImpl(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title, bool enforce_image_gate) {
  if (sessionManager() == nullptr) {
    qCWarning(lcScene3DDock) << "addTopic: session is null";
    return false;
  }
  if (layerFor(topic_id) != nullptr) {
    return true;
  }
  if (!handlesObjectType(object_type)) {
    qCWarning(lcScene3DDock) << "addTopic: unsupported object_type" << static_cast<int>(object_type);
    return false;
  }
  // Encoding gate: a kImage topic is a DepthCloud only when its samples are depth
  // pixels. Color images share kImage and must not become 3D layers. Skipped on the
  // restore path (enforce_image_gate=false): a saved DepthCloud layer was already
  // validated as depth when created, and its first sample may not be loaded yet at
  // restore time — gating on an absent sample would silently drop the layer (C1).
  if (enforce_image_gate && object_type == sdk::BuiltinObjectType::kImage &&
      !firstSampleIsDepthEncoded(*sessionManager(), topic_id)) {
    return false;
  }

  const bool accepted = SceneDockWidget::addTopic(topic_id, object_type, title);
  if (accepted) {
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
    recomputeOrphanStates();
  }
  return accepted;
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayer(const QString& urdf_path) {
  const ObjectTopicId topic_id = allocateLocalRobotLayerId();
  if (topic_id.id == 0) {
    qCWarning(lcScene3DDock) << "addRobotModelLayer: exhausted local topic ids";
    return ObjectTopicId{0};
  }
  const QString title = urdf_path.isEmpty() ? tr("Robot model") : QFileInfo(urdf_path).fileName();
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_robot_layer_ids_.erase(topic_id.id);
    return ObjectTopicId{0};
  }
  // One-click flow: point the freshly created File-source layer at the chosen
  // URDF immediately (the creator already forced kFile per the PUNCH-4 rule).
  if (!urdf_path.isEmpty()) {
    if (auto* layer = dynamic_cast<RobotModelLayer*>(layerFor(topic_id)); layer != nullptr) {
      layer->setSourceFile(urdf_path);
    }
  }
  return topic_id;
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayerFromUrl(const QString& url) {
  const ObjectTopicId topic_id = allocateLocalRobotLayerId();
  if (topic_id.id == 0) {
    qCWarning(lcScene3DDock) << "addRobotModelLayerFromUrl: exhausted local topic ids";
    return ObjectTopicId{0};
  }
  const QString file_name = QUrl(url).fileName();
  const QString title = file_name.isEmpty() ? url : file_name;
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_robot_layer_ids_.erase(topic_id.id);
    return ObjectTopicId{0};
  }
  if (auto* layer = dynamic_cast<RobotModelLayer*>(layerFor(topic_id)); layer != nullptr) {
    layer->setSourceUrl(url);
  }
  return topic_id;
}

QList<Scene3DDockWidget::RobotDescriptionTopic> Scene3DDockWidget::robotDescriptionTopics() const {
  QList<RobotDescriptionTopic> result;
  if (sessionManager() == nullptr) {
    return result;
  }
  ObjectStore& store = sessionManager()->objectStore();
  for (const ObjectTopicId topic_id : store.listTopics()) {
    const ObjectTopicDescriptor& desc = store.descriptor(topic_id);
    if (pj::scene3d::builtinObjectTypeFor(desc) == sdk::BuiltinObjectType::kRobotDescription) {
      result.append({topic_id, QString::fromStdString(desc.topic_name)});
    }
  }
  return result;
}

bool Scene3DDockWidget::pruneEvictedObjects() {
  // Family-specific prune only; the base revalidateObjects() adds the keep-if-
  // never-populated rule. Reports whether any live render layer or config topic
  // remains (a TF-only dock stays alive via config_topics_).
  if (sessionManager() == nullptr) {
    return !layers().empty() || !config_topics_.empty();
  }
  ObjectStore& store = sessionManager()->objectStore();
  std::vector<ObjectTopicId> dead;
  for (const SceneLayerInfo& info : layers()) {
    if (isLocalRobotLayerId(info.topic_id)) {
      continue;
    }
    if (store.descriptor(info.topic_id).topic_name.empty()) {
      dead.push_back(info.topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead) {
    removeTopic(topic_id);
  }
  // Prune evicted config (TF) topics. Unlike render-layer topics they have no
  // layer to remove; an empty descriptor means the dataset was unloaded. Keeping
  // them tracked is what lets a layer-less TF dock survive catalog churn (H.4).
  bool config_changed = false;
  for (auto it = config_topics_.begin(); it != config_topics_.end();) {
    if (store.descriptor(ObjectTopicId{*it}).topic_name.empty()) {
      scene_topic_datasets_.erase(*it);
      it = config_topics_.erase(it);
      config_changed = true;
    } else {
      ++it;
    }
  }
  if (config_changed) {
    resetTransformBindingIfDatasetGone();
  }
  return !layers().empty() || !config_topics_.empty();
}

bool Scene3DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // addTopic (this class's shadow, with title/orphan bookkeeping) already
  // guards on handlesObjectType() and consumes config topics via the base.
  return addTopic(topic_id, object_type, title);
}

void Scene3DDockWidget::onTrackerTime(double time) {
  // Remember the instant we are showing so a mid-load TF update (datasetTransformsReady)
  // can re-render here without a new playback push. Guard finiteness so a NaN tick
  // never poisons the replayed value.
  if (std::isfinite(time)) {
    last_tracker_display_ = time;
  }
  // The base converts (NaN/inf-safe), clamps with the latched-layer rule, drives
  // the layers, and — only when the visible scene would actually differ at this
  // time — calls refreshView() (below), which pushes the render time into the view.
  // When nothing moved the base returns without painting, so a 60 Hz playhead over
  // <10 Hz data (or a static pose) no longer repaints the GL view every tick. The
  // view's setTrackerTime/bounds therefore live in refreshView(), not here, so they
  // are gated together with the repaint.
  SceneDockWidget::onTrackerTime(time);
}

DatasetId Scene3DDockWidget::representativeDatasetId() const {
  // A TF-only dock has no render layer for the base to find, so fall back to the
  // bound TF dataset; a dock with render layers keeps the base's layer-derived id.
  const DatasetId from_layers = SceneDockWidget::representativeDatasetId();
  return (from_layers != 0) ? from_layers : dataset_id_;
}

QWidget* Scene3DDockWidget::createSceneView() {
  auto* view = new pj::scene3d::SceneViewWidget();
  view_ = view;
  view_->setContentsMargins(0, 0, 0, 0);
  view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  connect(view_, &pj::scene3d::SceneViewWidget::framesChanged, this, &Scene3DDockWidget::onAvailableFrames);
  if (tf_buffer_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  // refreshFrameOverlayCombo() already ends with layoutFrameOverlayCombo(),
  // which itself calls raise() — no second pass needed (L.82).
  refreshFrameOverlayCombo();
  // view_ is non-null from here: let the host apply view-only state (scene
  // controls) that could not be pushed while the lazy view did not yet exist.
  emit sceneViewReady();
  return view_;
}

std::unique_ptr<SceneLayerContext> Scene3DDockWidget::makeContext() {
  auto ctx = std::make_unique<Scene3DLayerContext>();
  ctx->session = sessionManager();
  ctx->tf_buffer = tf_buffer_;
  return ctx;
}

bool Scene3DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  // The factory registrations in the constructor are the single source of truth
  // for which render-layer types this dock can host.
  return layerFactory().supports(object_type);
}

bool Scene3DDockWidget::handleSceneConfigTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (object_type != sdk::BuiltinObjectType::kFrameTransforms) {
    return false;
  }
  if (sessionManager() == nullptr) {
    qCWarning(lcScene3DDock) << "addTopic: session is null";
    return false;
  }
  prepareTransformBufferForTopic(topic_id);
  // Remember this as a config (TF) topic so a layer-less dock survives catalog
  // churn (revalidateObjects) and folds into the live-edge drive (H.3 / H.4).
  const DatasetId dataset = sessionManager()->objectStore().descriptor(topic_id).dataset_id;
  scene_topic_datasets_[topic_id.id] = dataset;
  config_topics_.insert(topic_id.id);
  setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  return true;
}

void Scene3DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  std::vector<Scene3DLayer*> ordered;
  ordered.reserve(ordered_layers.size());
  const QString fixed = currentFixedFrame();
  for (ISceneLayer* layer : ordered_layers) {
    auto* scene3d_layer = dynamic_cast<Scene3DLayer*>(layer);
    if (scene3d_layer == nullptr) {
      continue;
    }
    absorbFallbackFrames(scene3d_layer);
    if (!fixed.isEmpty()) {
      scene3d_layer->setFixedFrame(fixed);
    }
    ordered.push_back(scene3d_layer);
  }
  if (view_ != nullptr) {
    view_->setLayers(ordered);
    updateSceneBounds();
  }
  recomputeOrphanStates();
}

void Scene3DDockWidget::updateSceneBounds() {
  if (view_ == nullptr) {
    return;
  }
  pj::scene3d::AABB scene;
  for (Scene3DLayer* layer : view_->layers()) {
    if (layer == nullptr) {
      continue;
    }
    if (const auto bounds = layer->worldBounds(); bounds.has_value()) {
      scene = pj::scene3d::unionAABB(scene, *bounds);
    }
  }
  view_->setSceneBounds(scene);
}

void Scene3DDockWidget::refreshView() {
  if (view_ == nullptr) {
    return;
  }
  // Push the current render time into the view HERE (not in onTrackerTime) so the
  // base's per-tick repaint gate also gates this work: refreshView() runs only on a
  // tracker tick that actually changed the scene, or on an async layer push
  // (compressed-cloud decode / settings) arriving via repaintRequested — both must
  // re-render at the current playhead. setTrackerTime() self-guards on render_time_
  // equality, so a settings-only refresh skips the frame-list rebuild.
  if (const auto ns = lastTrackerNs(); ns.has_value()) {
    view_->setTrackerTime(PJ::fromRaw(*ns));
  }
  // Async pushes can arrive with no tracker tick (paused), so the camera's scene
  // bounds must refresh too. O(layers), cheap.
  updateSceneBounds();
  view_->update();
}

uint64_t Scene3DDockWidget::viewRenderKey(PJ::Timepoint time) const {
  // The TF axis triads + parent-connection lines are drawn by the view from the
  // whole frame forest, owned by no layer; without folding them into the gate a
  // moving TF tree freezes whenever every visible layer's renderKey is unchanged
  // (a static/fixed-frame cloud, or a TF-only dock). Returns 0 when no TF overlay
  // is drawn (the view decides), so coalescing is full when it should be.
  return view_ != nullptr ? view_->tfRenderKey(time) : 0;
}

QString Scene3DDockWidget::xmlTag() const {
  return QStringLiteral("scene3d");
}

void Scene3DDockWidget::prepareTransformBufferForTopic(ObjectTopicId topic_id) {
  if (transform_service_ == nullptr || sessionManager() == nullptr) {
    return;
  }
  ObjectStore& store = sessionManager()->objectStore();
  const auto dataset_id = store.descriptor(topic_id).dataset_id;
  // Record the topic→dataset mapping for both layer and config topics so the
  // removal path can detect when the TF-bound dataset has no remaining topics
  // and rebind (M.17). handleSceneConfigTopic also records config topics in
  // config_topics_; this covers render-layer topics.
  scene_topic_datasets_[topic_id.id] = dataset_id;

  if (tf_buffer_ != nullptr) {
    // Already bound. Mixing a second dataset's topics into one 3D dock would
    // resolve B's frames against A's TF tree — silently wrong. Warn loudly and
    // keep the existing binding rather than rebinding underneath live layers.
    // The reset-on-removal path lets a genuine rebind happen once dataset A's
    // topics are gone (tf_buffer_ goes back to null first).
    if (dataset_id != dataset_id_) {
      qCWarning(lcScene3DDock) << "prepareTransformBufferForTopic: topic from dataset" << dataset_id
                               << "added to a 3D dock already bound to dataset" << dataset_id_
                               << "- its frames will resolve against the wrong TF tree";
    }
    return;
  }
  dataset_id_ = dataset_id;  // cached for the live-samples TF ingest slot
  tf_buffer_ = transform_service_->transformBuffer(dataset_id);
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
}

void Scene3DDockWidget::resetTransformBindingIfDatasetGone() {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const bool dataset_still_present = std::any_of(
      scene_topic_datasets_.begin(), scene_topic_datasets_.end(),
      [this](const auto& entry) { return entry.second == dataset_id_; });
  if (dataset_still_present) {
    return;
  }
  // No tracked topic belongs to the bound dataset anymore: drop the binding so a
  // later topic from a different dataset can rebind via prepareTransformBufferForTopic.
  tf_buffer_.reset();
  dataset_id_ = 0;
  if (view_ != nullptr) {
    view_->setTransformBuffer(nullptr);  // setTransformBuffer tolerates a null buffer
  }
}

void Scene3DDockWidget::wireScene3DLayer(Scene3DLayer* layer) {
  if (layer == nullptr) {
    return;
  }
  connect(layer, &Scene3DLayer::fallbackFramesChanged, this, [this, layer](const QStringList&) {
    absorbFallbackFrames(layer);
  });
  connect(layer, &Scene3DLayer::sourceFrameChanged, this, [this](const QString&) { recomputeOrphanStates(); });
}

void Scene3DDockWidget::absorbFallbackFrames(Scene3DLayer* layer) {
  if (layer == nullptr) {
    return;
  }
  bool changed = false;
  for (const QString& frame : layer->fallbackFrames()) {
    const std::string frame_std = frame.toStdString();
    if (std::find(fallback_frames_.begin(), fallback_frames_.end(), frame_std) == fallback_frames_.end()) {
      fallback_frames_.push_back(frame_std);
      changed = true;
    }
  }
  if (changed) {
    onAvailableFrames(available_frames_);
  }
}

void Scene3DDockWidget::onAvailableFrames(const QList<FrameRow>& frames) {
  std::vector<QList<FrameRow>> clusters;
  for (const auto& row : frames) {
    if (row.depth == 0 || clusters.empty()) {
      clusters.emplace_back();
    }
    clusters.back().append(row);
  }
  for (const auto& fallback : fallback_frames_) {
    if (!framesContain(frames, QString::fromStdString(fallback))) {
      clusters.push_back({FrameRow{fallback, 0}});
    }
  }
  std::sort(clusters.begin(), clusters.end(), [](const QList<FrameRow>& a, const QList<FrameRow>& b) {
    return pj::scene3d::frameNameLess(a.first().name, b.first().name);
  });

  QList<FrameRow> effective;
  for (auto& cluster : clusters) {
    for (auto& row : cluster) {
      effective.append(std::move(row));
    }
  }

  if (effective.isEmpty()) {
    return;
  }
  available_frames_ = effective;
  emit availableFramesChanged(effective);
  refreshFrameOverlayCombo();

  if (fixed_frame_mode_ == FixedFrameMode::kAutoRoot || currentFixedFrame().isEmpty()) {
    applyResolvedFixedFrame(resolveAutoFixedFrame(effective));
  }
  recomputeOrphanStates();
}

QString Scene3DDockWidget::resolveAutoFixedFrame(const QList<FrameRow>& frames) const {
  // Prefer the frame the user last picked by hand for this dataset's TransformBuffer
  // (shared across docks, persisted across restarts), but only when it still exists
  // in this dock's tree — a different recording of the same source may lack it.
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    const QString remembered = transform_service_->rememberedFixedFrame(dataset_id_);
    if (!remembered.isEmpty() && framesContain(frames, remembered)) {
      return remembered;
    }
  }
  return pickFixedFrame(frames);
}

QString Scene3DDockWidget::currentFixedFrame() const {
  if (view_ == nullptr) {
    return {};
  }
  return QString::fromStdString(view_->fixedFrame());
}

bool Scene3DDockWidget::layerVisible(ObjectTopicId topic_id) const {
  const ISceneLayer* layer = layerFor(topic_id);
  return layer != nullptr && layer->info().visible;
}

void Scene3DDockWidget::setFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kAutoRoot);
  fixed_frame_mode_ = FixedFrameMode::kExplicit;
  if (mode_flipped) {
    emit fixedFrameModeChanged(false);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(frame);
  // Remember this explicit pick so a NEWLY-created dock on the same dataset's
  // TransformBuffer defaults to it (resolveAutoFixedFrame). dataset_id_ is 0 for a
  // not-yet-bound or local-only dock — nothing to key the memory by, so skip.
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    transform_service_->rememberFixedFrame(dataset_id_, frame);
  }
}

void Scene3DDockWidget::setFixedFrameAutoRoot() {
  if (view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kExplicit);
  fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  if (mode_flipped) {
    emit fixedFrameModeChanged(true);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(resolveAutoFixedFrame(available_frames_));
}

void Scene3DDockWidget::applyResolvedFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  if (currentFixedFrame() == frame) {
    return;
  }
  view_->setFixedFrame(frame.toStdString());
  // Single fan-out: reconcileViewLayers() re-runs syncViewLayers(), whose 3D
  // override pushes the (now-changed) currentFixedFrame() to every layer, updates
  // bounds, AND recomputes orphans — so we don't re-iterate layers or recompute
  // here (L.83). currentFixedFrame() must already read the new frame before this:
  // view_->setFixedFrame above made it so.
  reconcileViewLayers();
  view_->update();
  emit currentFixedFrameChanged(frame);
  refreshFrameOverlayCombo();
}

void Scene3DDockWidget::refreshFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  QSignalBlocker block(frame_overlay_combo_);
  frame_overlay_combo_->clear();
  for (const auto& row : available_frames_) {
    const QString name = QString::fromStdString(row.name);
    const QString display = QString(row.depth * 2, QChar(' ')) + name;
    frame_overlay_combo_->addItem(display, name);
  }
  const QString current = currentFixedFrame();
  const int idx = frame_overlay_combo_->findData(current);
  if (idx >= 0) {
    frame_overlay_combo_->setCurrentIndex(idx);
  }
  layoutFrameOverlayCombo();
}

void Scene3DDockWidget::onOverlayFramePicked(int /*index*/) {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  const QString name = frame_overlay_combo_->currentData().toString();
  if (!name.isEmpty()) {
    setFixedFrame(name);
  }
}

void Scene3DDockWidget::layoutFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr || view_ == nullptr) {
    return;
  }
  constexpr int kMargin = 8;
  // Combo width tracks the selected item only (so the overlay stays compact even
  // when one frame name is very long). The chrome is added by the active combo
  // style rather than a hardcoded slack, which avoids clipping themed combos.
  const QFontMetrics fm(frame_overlay_combo_->font());
  const QString current_text = frame_overlay_combo_->currentText();
  const auto combo_width_for = [&](const QString& text) {
    QStyleOptionComboBox opt;
    opt.initFrom(frame_overlay_combo_);
    const QSize content(fm.horizontalAdvance(text), fm.height());
    return frame_overlay_combo_->style()
        ->sizeFromContents(QStyle::CT_ComboBox, &opt, content, frame_overlay_combo_)
        .width();
  };
  const int natural_w = combo_width_for(current_text);
  const int avail = width() - 2 * kMargin;
  const int w = (avail > 0) ? std::min(natural_w, avail) : natural_w;
  const int h = frame_overlay_combo_->sizeHint().height();
  const QPoint view_origin = view_->pos();
  frame_overlay_combo_->setGeometry(view_origin.x() + kMargin, view_origin.y() + kMargin, w, h);

  if (auto* popup_view = frame_overlay_combo_->view()) {
    int popup_w = natural_w;
    for (int i = 0; i < frame_overlay_combo_->count(); ++i) {
      popup_w = std::max(popup_w, combo_width_for(frame_overlay_combo_->itemText(i)));
    }
    popup_view->setMinimumWidth(popup_w);
  }
  frame_overlay_combo_->raise();

  // Camera-model combo + Home button, anchored flush to the top-right edge. The
  // orientation gizmo now lives in the bottom-right corner (set in the
  // SceneViewWidget ctor), so nothing is reserved up here. The fixed-frame combo
  // stays top-left.
  constexpr int kGap = 6;
  if (camera_model_combo_ != nullptr) {
    // Size via the combo's own style chrome (chevron + padding), matching the
    // fixed-frame combo's style-driven sizing above — a hardcoded slack would
    // clip a themed ComboBox.
    const QFontMetrics cm_fm(camera_model_combo_->font());
    QStyleOptionComboBox cam_opt;
    cam_opt.initFrom(camera_model_combo_);
    int cam_w = 0;
    for (int i = 0; i < camera_model_combo_->count(); ++i) {
      const QSize content(cm_fm.horizontalAdvance(camera_model_combo_->itemText(i)), cm_fm.height());
      cam_w = std::max(
          cam_w, camera_model_combo_->style()
                     ->sizeFromContents(QStyle::CT_ComboBox, &cam_opt, content, camera_model_combo_)
                     .width());
    }
    const int cam_h = camera_model_combo_->sizeHint().height();
    const int home_w = (home_button_ != nullptr) ? cam_h : 0;  // square button matching combo height
    const int total_w = cam_w + (home_button_ != nullptr ? kGap + home_w : 0);
    const int top_y = view_origin.y() + kMargin;
    // Right edge of the controls sits a margin in from the view's right edge,
    // mirroring the fixed-frame combo's left margin. Home button is the rightmost,
    // combo to its left.
    const int controls_right = view_origin.x() + view_->width() - kMargin;
    const int left_x = controls_right - total_w;
    camera_model_combo_->setGeometry(left_x, top_y, cam_w, cam_h);
    camera_model_combo_->raise();
    if (home_button_ != nullptr) {
      home_button_->setGeometry(left_x + cam_w + kGap, top_y, home_w, cam_h);
      // Icon-only square button: fill it minus the 1px border (top+bottom) so the
      // home glyph reads at the same size as the 20px config-panel icon buttons,
      // not the tiny default QToolButton icon metric.
      const int home_icon_dim = std::max(home_w - 2, 1);
      home_button_->setIconSize(QSize(home_icon_dim, home_icon_dim));
      home_button_->raise();
    }
  }
}

void Scene3DDockWidget::resizeEvent(QResizeEvent* event) {
  SceneDockWidget::resizeEvent(event);
  layoutFrameOverlayCombo();
}

Scene3DDockWidget::OrphanSnapshot Scene3DDockWidget::orphanState(ObjectTopicId topic_id) const {
  auto it = orphan_states_.find(topicKey(topic_id));
  if (it == orphan_states_.end()) {
    return {};
  }
  return it->second;
}

void Scene3DDockWidget::recomputeOrphanStates(bool force) {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const QString fixed = currentFixedFrame();
  // Hot live-ingest path: skip the per-layer frame walk when neither the buffer
  // nor the fixed frame moved since the last full recompute (L.45/L.47).
  if (!force && tf_buffer_->revision() == last_orphan_revision_ && fixed == last_orphan_fixed_frame_) {
    return;
  }
  const std::string fixed_std = fixed.toStdString();

  std::unordered_set<std::string> known_frames;
  for (auto&& frame : tf_buffer_->getAllFrames()) {
    known_frames.insert(std::move(frame));
  }

  for (const SceneLayerInfo& info : layers()) {
    auto* layer = dynamic_cast<Scene3DLayer*>(layerFor(info.topic_id));
    if (layer == nullptr) {
      continue;
    }
    const QString src = layer->sourceFrame();
    bool is_orphan = false;
    QString reason;
    if (src.isEmpty() || fixed.isEmpty()) {
      // Pending data.
    } else if (src == fixed) {
      // Identity transform.
    } else {
      const std::string src_std = src.toStdString();
      if (known_frames.count(src_std) == 0) {
        is_orphan = true;
        reason = tr("Frame '%1' can't be resolved").arg(src);
      } else if (!tf_buffer_->areConnected(fixed_std, src_std)) {
        is_orphan = true;
        reason = tr("Frame '%1' is not connected to fixed frame '%2'").arg(src, fixed);
      }
    }

    auto& state = orphan_states_[topicKey(info.topic_id)];
    if (state.is_orphan != is_orphan || state.reason != reason) {
      state.is_orphan = is_orphan;
      state.reason = reason;
      emit layerWarningChanged(info.topic_id, is_orphan, reason);
    }
  }

  // Record what this full recompute observed so the force=false fast path can
  // detect "nothing changed" on the next live tick.
  last_orphan_revision_ = tf_buffer_->revision();
  last_orphan_fixed_frame_ = fixed;
}

bool Scene3DDockWidget::isLocalRobotLayerId(ObjectTopicId topic_id) const {
  return local_robot_layer_ids_.find(topic_id.id) != local_robot_layer_ids_.end();
}

ObjectTopicId Scene3DDockWidget::allocateLocalRobotLayerId() {
  while (next_local_robot_topic_id_ > 0) {
    ObjectTopicId topic_id;
    topic_id.id = next_local_robot_topic_id_--;
    if (layerFor(topic_id) == nullptr && local_robot_layer_ids_.insert(topic_id.id).second) {
      return topic_id;
    }
  }
  return {};
}

bool Scene3DDockWidget::restoreLayerElement(const QDomElement& layer_el) {
  if (sessionManager() == nullptr) {
    return false;
  }

  const QString object_type_str = layer_el.attribute(QStringLiteral("object_type"));
  const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
  if (!object_type_opt.has_value()) {
    return true;
  }
  const QString display_name = layer_el.attribute(QStringLiteral("display_name"));
  const bool visible = layer_el.attribute(QStringLiteral("visible"), QStringLiteral("true")) == QStringLiteral("true");

  ObjectTopicId topic_id;
  const bool local_layer = layer_el.attribute(QStringLiteral("local")) == QStringLiteral("true");
  if (local_layer) {
    if (*object_type_opt != sdk::BuiltinObjectType::kRobotDescription) {
      return true;
    }
    topic_id = allocateLocalRobotLayerId();
    if (topic_id.id == 0) {
      return true;
    }
  } else {
    bool dataset_ok = false;
    const auto dataset_value = layer_el.attribute(QStringLiteral("dataset_id")).toULongLong(&dataset_ok);
    if (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max()) {
      return true;
    }
    const auto saved_id = static_cast<DatasetId>(dataset_value);
    const QString saved_source = layer_el.attribute(QStringLiteral("dataset_source"));
    const QString topic_name = layer_el.attribute(QStringLiteral("topic_name"));
    // Re-resolve by stable source name first; the raw id is load-order (M.55).
    const auto dataset_id_opt = resolveDatasetId(sessionManager(), saved_id, saved_source);
    if (!dataset_id_opt.has_value()) {
      return false;
    }
    const auto topic_id_opt = sessionManager()->objectStore().findTopic(*dataset_id_opt, topic_name.toStdString());
    if (!topic_id_opt.has_value()) {
      return false;
    }
    topic_id = *topic_id_opt;
  }

  // Restore trusts the saved layer type: a persisted kImage layer was a
  // DepthCloud when saved, and its first sample may not be loaded yet here, so
  // the interactive depth-encoding gate must not run (it would silently drop the
  // layer — C1). enforce_image_gate=false.
  if (!addTopicImpl(topic_id, *object_type_opt, display_name, /*enforce_image_gate=*/false)) {
    if (local_layer) {
      local_robot_layer_ids_.erase(topic_id.id);
    }
    return true;
  }
  if (ISceneLayer* layer = layerFor(topic_id); layer != nullptr) {
    const QDomElement payload = layer_el.firstChildElement();
    if (!payload.isNull()) {
      layer->xmlLoadState(payload);
    }
  }
  if (!visible) {
    setLayerVisible(topic_id, false);
  }
  return true;
}

bool Scene3DDockWidget::restoreConfigTopicElement(const QDomElement& config_el) {
  if (sessionManager() == nullptr) {
    return false;
  }

  bool dataset_ok = false;
  const auto dataset_value = config_el.attribute(QStringLiteral("dataset_id")).toULongLong(&dataset_ok);
  if (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max()) {
    return true;
  }
  const auto saved_id = static_cast<DatasetId>(dataset_value);
  const QString saved_source = config_el.attribute(QStringLiteral("dataset_source"));
  const QString topic_name = config_el.attribute(QStringLiteral("topic_name"));
  const QString object_type_str = config_el.attribute(QStringLiteral("object_type"));
  const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
  if (!object_type_opt.has_value()) {
    return true;
  }
  const auto dataset_id_opt = resolveDatasetId(sessionManager(), saved_id, saved_source);
  if (!dataset_id_opt.has_value()) {
    return false;
  }
  const auto topic_id_opt = sessionManager()->objectStore().findTopic(*dataset_id_opt, topic_name.toStdString());
  if (!topic_id_opt.has_value()) {
    return false;
  }
  addTopic(*topic_id_opt, *object_type_opt, topic_name);
  return true;
}

bool Scene3DDockWidget::restoreOnePending(const QDomElement& element) {
  // 3D defers two element kinds into the base's shared pending queue: scene-config
  // topics (e.g. TF) and render layers. Dispatch on the tag; the base SceneDockWidget
  // owns the queue + the retry/unresolved/clear bookkeeping.
  return element.tagName() == QStringLiteral("config_topic") ? restoreConfigTopicElement(element)
                                                             : restoreLayerElement(element);
}

QDomElement Scene3DDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(xmlTag());
  root.setAttribute(QStringLiteral("version"), QStringLiteral("1"));

  ObjectStore* store = sessionManager() != nullptr ? &sessionManager()->objectStore() : nullptr;
  for (const SceneLayerInfo& info : layers()) {
    const bool local_layer = isLocalRobotLayerId(info.topic_id);
    if (!local_layer && store == nullptr) {
      continue;
    }

    QDomElement layer_el = doc.createElement(QStringLiteral("layer"));
    if (local_layer) {
      layer_el.setAttribute(QStringLiteral("local"), QStringLiteral("true"));
      layer_el.setAttribute(QStringLiteral("dataset_id"), QStringLiteral("0"));
      layer_el.setAttribute(QStringLiteral("topic_name"), QString());
    } else {
      const auto& desc = store->descriptor(info.topic_id);
      layer_el.setAttribute(QStringLiteral("dataset_id"), QString::number(desc.dataset_id));
      // Source name is stable across sessions; the raw id is a load-order counter
      // (see resolveDatasetId). Persist both so restore can re-resolve (M.55).
      layer_el.setAttribute(QStringLiteral("dataset_source"), datasetSourceName(sessionManager(), desc.dataset_id));
      layer_el.setAttribute(QStringLiteral("topic_name"), QString::fromStdString(desc.topic_name));
    }
    const auto object_type_name = sdk::name(info.object_type);
    layer_el.setAttribute(
        QStringLiteral("object_type"),
        QString::fromLatin1(object_type_name.data(), static_cast<qsizetype>(object_type_name.size())));
    layer_el.setAttribute(QStringLiteral("display_name"), info.display_name);
    layer_el.setAttribute(QStringLiteral("visible"), info.visible ? QStringLiteral("true") : QStringLiteral("false"));

    if (ISceneLayer* layer = layerFor(info.topic_id); layer != nullptr) {
      QDomElement payload = layer->xmlSaveState(doc);
      if (!payload.isNull()) {
        layer_el.appendChild(payload);
      }
    }
    root.appendChild(layer_el);
  }

  // FrameTransforms config topics create no layer (handleSceneConfigTopic just
  // binds tf_buffer_/dataset_id_), so the layer loop above leaves no trace of a
  // TF-only / URDF+TF dock's TF source. Persist them separately so restore can
  // re-bind the TF buffer (M.18).
  if (store != nullptr) {
    for (const uint32_t topic_raw : config_topics_) {
      const ObjectTopicId topic_id{topic_raw};
      const ObjectTopicDescriptor& desc = store->descriptor(topic_id);
      if (desc.topic_name.empty()) {
        continue;  // evicted; nothing to restore
      }
      QDomElement config_el = doc.createElement(QStringLiteral("config_topic"));
      config_el.setAttribute(QStringLiteral("dataset_id"), QString::number(desc.dataset_id));
      config_el.setAttribute(QStringLiteral("dataset_source"), datasetSourceName(sessionManager(), desc.dataset_id));
      config_el.setAttribute(QStringLiteral("topic_name"), QString::fromStdString(desc.topic_name));
      const auto frame_transforms_name = sdk::name(sdk::BuiltinObjectType::kFrameTransforms);
      config_el.setAttribute(
          QStringLiteral("object_type"),
          QString::fromLatin1(frame_transforms_name.data(), static_cast<qsizetype>(frame_transforms_name.size())));
      root.appendChild(config_el);
    }
  }

  root.setAttribute(
      QStringLiteral("fixed_frame_mode"),
      fixed_frame_mode_ == FixedFrameMode::kAutoRoot ? QStringLiteral("auto_root") : QStringLiteral("explicit"));
  root.setAttribute(QStringLiteral("fixed_frame"), currentFixedFrame());
  if (view_ != nullptr && camera_model_combo_ != nullptr) {
    root.setAttribute(QStringLiteral("camera_model"), cameraModelToString(camera_model_combo_->currentIndex()));
    root.setAttribute(
        QStringLiteral("camera_state"),
        QString::fromStdString(pj::scene3d::cameraStateToJson(view_->camera().state())));
  }
  if (view_ != nullptr) {
    // Per-dock scene-look controls travel WITH the layout (export/import), so a
    // restored layout gives each view its own grid/frame/mesh look instead of a
    // shared global one. The QSettings group is only the seed for brand-new docks.
    // Field set mirrors Scene3DConfigPanel::applySceneControlsTo (keep in sync; 4 sites).
    const auto bool_attr = [](bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); };
    QDomElement sc = doc.createElement(QStringLiteral("scene_controls"));
    sc.setAttribute(QStringLiteral("grid_visible"), bool_attr(view_->gridVisible()));
    sc.setAttribute(
        QStringLiteral("grid_style"), view_->gridStyle() == pj::scene3d::GridRenderPass::Style::kFilledCells ? 1 : 0);
    sc.setAttribute(QStringLiteral("grid_extent_m"), view_->gridExtentMetres());
    sc.setAttribute(QStringLiteral("grid_divisions"), view_->gridDivisions());
    sc.setAttribute(QStringLiteral("axes_visible"), bool_attr(view_->axesVisible()));
    sc.setAttribute(QStringLiteral("gizmo_size_m"), view_->gizmoSize());
    sc.setAttribute(QStringLiteral("gizmo_opacity"), view_->gizmoOpacity());
    sc.setAttribute(QStringLiteral("tf_parent_lines"), bool_attr(view_->tfConnectionsVisible()));
    const auto& shading = view_->meshShadingParams();
    sc.setAttribute(QStringLiteral("meshes_visible"), bool_attr(shading.meshes_visible));
    sc.setAttribute(QStringLiteral("mesh_opacity"), shading.mesh_opacity);
    sc.setAttribute(QStringLiteral("collisions_visible"), bool_attr(shading.collisions_visible));
    sc.setAttribute(QStringLiteral("collision_opacity"), shading.collision_opacity);
    root.appendChild(sc);
  }
  return root;
}

bool Scene3DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("scene3d")) {
    return false;
  }

  // Force the lazily-created view now. PlotDocker calls xmlLoadState
  // synchronously during layout restore, before the deferred singleShot fires;
  // without this view_ is null and the explicit fixed frame, fixed-frame mode,
  // and camera state below are silently dropped when zero layers restore (M.19).
  ensureSceneViewCreated();

  orphan_states_.clear();
  fallback_frames_.clear();
  local_robot_layer_ids_.clear();
  config_topics_.clear();
  scene_topic_datasets_.clear();
  clearPendingRestores();

  const QString saved_mode = element.attribute(QStringLiteral("fixed_frame_mode"), QStringLiteral("auto_root"));
  const QString saved_frame = element.attribute(QStringLiteral("fixed_frame"));

  clearLayers();
  int unresolved_topics = 0;
  if (sessionManager() != nullptr) {
    for (QDomElement layer_el = element.firstChildElement(QStringLiteral("layer")); !layer_el.isNull();
         layer_el = layer_el.nextSiblingElement(QStringLiteral("layer"))) {
      if (!restoreLayerElement(layer_el)) {
        ++unresolved_topics;
        rememberPendingRestore(layer_el);
      }
    }

    // Re-add persisted FrameTransforms config topics so handleSceneConfigTopic
    // re-binds tf_buffer_/dataset_id_ (M.18). These create no layer; a TF-only
    // dock has nothing in the layer loop above and depends entirely on this.
    for (QDomElement config_el = element.firstChildElement(QStringLiteral("config_topic")); !config_el.isNull();
         config_el = config_el.nextSiblingElement(QStringLiteral("config_topic"))) {
      if (!restoreConfigTopicElement(config_el)) {
        ++unresolved_topics;
        rememberPendingRestore(config_el);
      }
    }
  }
  if (unresolved_topics > 0) {
    qCWarning(lcScene3DDock) << unresolved_topics << "saved layer(s) could not be restored (dataset not loaded)";
  }

  if (saved_mode == QStringLiteral("explicit") && !saved_frame.isEmpty()) {
    setFixedFrame(saved_frame);
  } else {
    setFixedFrameAutoRoot();
  }

  // Restore the camera model + pose (tolerant of older layouts without them).
  // Setting the combo index switches the active controller via its signal; the
  // adoptState then applies the saved pose on top.
  if (view_ != nullptr) {
    if (camera_model_combo_ != nullptr) {
      if (const int idx = cameraModelFromString(element.attribute(QStringLiteral("camera_model"))); idx >= 0) {
        camera_model_combo_->setCurrentIndex(idx);
      }
    }
    const QString camera_state = element.attribute(QStringLiteral("camera_state"));
    if (!camera_state.isEmpty()) {
      view_->camera().adoptState(pj::scene3d::cameraStateFromJson(camera_state.toStdString(), view_->camera().state()));
    }
    // Per-dock scene controls: override the global seed applied at view creation
    // (sceneViewReady -> applySceneControlsTo) with this dock's saved look. Older
    // layouts have no <scene_controls> child -> keep the seed. Each attribute
    // falls back to the current value so a partial element never zeroes a control.
    // Field set mirrors Scene3DConfigPanel::applySceneControlsTo (keep in sync; 4 sites).
    if (const QDomElement sc = element.firstChildElement(QStringLiteral("scene_controls")); !sc.isNull()) {
      const auto bool_attr = [&sc](const QString& key, bool fallback) {
        return sc.attribute(key, fallback ? QStringLiteral("true") : QStringLiteral("false")) == QStringLiteral("true");
      };
      view_->setGridVisible(bool_attr(QStringLiteral("grid_visible"), view_->gridVisible()));
      view_->setGridStyle(
          sc.attribute(QStringLiteral("grid_style"), QStringLiteral("0")).toInt() == 1
              ? pj::scene3d::GridRenderPass::Style::kFilledCells
              : pj::scene3d::GridRenderPass::Style::kLines);
      view_->setGridExtentMetres(
          sc.attribute(QStringLiteral("grid_extent_m"), QString::number(view_->gridExtentMetres())).toFloat());
      view_->setGridDivisions(
          sc.attribute(QStringLiteral("grid_divisions"), QString::number(view_->gridDivisions())).toInt());
      view_->setAxesVisible(bool_attr(QStringLiteral("axes_visible"), view_->axesVisible()));
      view_->setGizmoSize(sc.attribute(QStringLiteral("gizmo_size_m"), QString::number(view_->gizmoSize())).toFloat());
      view_->setGizmoOpacity(
          sc.attribute(QStringLiteral("gizmo_opacity"), QString::number(view_->gizmoOpacity())).toFloat());
      view_->setTfConnectionsVisible(bool_attr(QStringLiteral("tf_parent_lines"), view_->tfConnectionsVisible()));
      auto& shading = view_->meshShadingParams();
      shading.meshes_visible = bool_attr(QStringLiteral("meshes_visible"), shading.meshes_visible);
      shading.mesh_opacity =
          sc.attribute(QStringLiteral("mesh_opacity"), QString::number(shading.mesh_opacity)).toFloat();
      shading.collisions_visible = bool_attr(QStringLiteral("collisions_visible"), shading.collisions_visible);
      shading.collision_opacity =
          sc.attribute(QStringLiteral("collision_opacity"), QString::number(shading.collision_opacity)).toFloat();
    }
    view_->update();
  }

  const auto infos = layers();
  if (infos.empty()) {
    setWindowTitle(tr("3D View"));
  } else {
    const QString& title = infos.back().display_name;
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  }
  recomputeOrphanStates();
  return true;
}

}  // namespace PJ
