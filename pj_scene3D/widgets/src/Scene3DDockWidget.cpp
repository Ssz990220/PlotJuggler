// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/Scene3DDockWidget.h"

#include <QAbstractItemView>
#include <QDomDocument>
#include <QDomElement>
#include <QFontMetrics>
#include <QIcon>
#include <QLoggingCategory>
#include <QPoint>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene3DDock, "pj.scene3d.dock")

using pj::scene3d::FrameRow;
using pj::scene3d::OccupancyGridLayer;
using pj::scene3d::PointCloudLayer;
using pj::scene3d::Scene3DLayer;
using pj::scene3d::Scene3DLayerContext;
using pj::scene3d::SceneViewWidget;

// Stable enum <-> on-disk-name table for the camera model, persisted in the
// layout. The enum value == combo index, so the on-disk name stays independent of
// the combo's display order; one table feeds both directions so they can't drift.
struct CameraModelName {
  SceneViewWidget::CameraModel model;
  const char* id;
};
constexpr CameraModelName kCameraModelNames[] = {
    {SceneViewWidget::CameraModel::Orbit, "orbit"},
    {SceneViewWidget::CameraModel::XYOrbit, "xy_orbit"},
    {SceneViewWidget::CameraModel::Fly, "fly"},
    {SceneViewWidget::CameraModel::TopDownOrtho, "top_down_ortho"},
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

[[nodiscard]] int64_t topicKey(ObjectTopicId topic_id) {
  return static_cast<int64_t>(topic_id.id);
}

}  // namespace

Scene3DDockWidget::Scene3DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("3D View"));

  layerFactory().registerType(
      sdk::BuiltinObjectType::kPointCloud,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<PointCloudLayer>(topic_id, display_name, this);
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
  home_button_->setIcon(PJ::LoadSvg(QStringLiteral(":/resources/svg/home.svg")));
  home_button_->setStyleSheet(QStringLiteral(
      "QToolButton { background-color: rgba(255, 255, 255, 200); border: 1px solid rgba(60, 60, 60, 180); "
      "padding: 2px; border-radius: 3px; }"));  // symmetric padding: this is a square icon-only button
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
  transform_service_ = service;
  if (view_ != nullptr && tf_buffer_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
}

bool Scene3DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  return object_type == sdk::BuiltinObjectType::kPointCloud ||
         object_type == sdk::BuiltinObjectType::kFrameTransforms ||
         object_type == sdk::BuiltinObjectType::kOccupancyGrid;
}

bool Scene3DDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
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

  const bool accepted = SceneDockWidget::addTopic(topic_id, object_type, title);
  if (accepted) {
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
    recomputeOrphanStates();
  }
  return accepted;
}

bool Scene3DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // addTopic (this class's shadow, with title/orphan bookkeeping) already
  // guards on handlesObjectType() and consumes config topics via the base.
  return addTopic(topic_id, object_type, title);
}

void Scene3DDockWidget::onTrackerTime(double time) {
  // The base converts (NaN/inf-safe), clamps with the latched-layer rule, and
  // drives the layers; reuse its result for the view's render time instead of
  // re-deriving and re-clamping it here.
  SceneDockWidget::onTrackerTime(time);
  if (const auto ns = lastTrackerNs(); view_ != nullptr && ns.has_value()) {
    view_->setTrackerTime(std::chrono::nanoseconds{*ns});
    updateSceneBounds();  // cloud / grid geometry changes with tracker time
  }
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
  refreshFrameOverlayCombo();
  layoutFrameOverlayCombo();
  if (frame_overlay_combo_ != nullptr) {
    frame_overlay_combo_->raise();
  }
  return view_;
}

std::unique_ptr<SceneLayerContext> Scene3DDockWidget::makeContext() {
  auto ctx = std::make_unique<Scene3DLayerContext>();
  ctx->session = sessionManager();
  ctx->tf_buffer = tf_buffer_;
  return ctx;
}

bool Scene3DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  return object_type == sdk::BuiltinObjectType::kPointCloud || object_type == sdk::BuiltinObjectType::kOccupancyGrid;
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
  if (view_ != nullptr) {
    view_->update();
  }
}

QString Scene3DDockWidget::xmlTag() const {
  return QStringLiteral("scene3d");
}

void Scene3DDockWidget::prepareTransformBufferForTopic(ObjectTopicId topic_id) {
  if (tf_buffer_ != nullptr || transform_service_ == nullptr || sessionManager() == nullptr) {
    return;
  }
  ObjectStore& store = sessionManager()->objectStore();
  const auto dataset_id = store.descriptor(topic_id).dataset_id;
  tf_buffer_ = transform_service_->transformBuffer(dataset_id);
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
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
    applyResolvedFixedFrame(pickFixedFrame(effective));
  }
  recomputeOrphanStates();
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
  applyResolvedFixedFrame(pickFixedFrame(available_frames_));
}

void Scene3DDockWidget::applyResolvedFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  if (currentFixedFrame() == frame) {
    return;
  }
  view_->setFixedFrame(frame.toStdString());
  for (const SceneLayerInfo& info : layers()) {
    if (ISceneLayer* layer = layerFor(info.topic_id); layer != nullptr) {
      layer->setFixedFrame(frame);
    }
  }
  view_->update();
  emit currentFixedFrameChanged(frame);
  refreshFrameOverlayCombo();
  recomputeOrphanStates();
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

  // Camera-model combo + Home button, anchored top-right but to the LEFT of the
  // orientation gizmo (which occupies the very corner). The fixed-frame combo
  // stays top-left.
  constexpr int kGap = 6;
  // Reserve the gizmo's default footprint — read from AxisOverlayPass so the two
  // can't silently desync — plus a gap before the controls.
  constexpr int kGizmoReserveW =
      pj::scene3d::AxisOverlayPass::kDefaultSizePx + pj::scene3d::AxisOverlayPass::kDefaultMarginPx + kGap;
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
    // Right edge of the controls sits just left of the gizmo's reserved region;
    // Home button is the rightmost (nearest the gizmo), combo to its left.
    const int controls_right = view_origin.x() + view_->width() - kGizmoReserveW;
    const int left_x = controls_right - total_w;
    camera_model_combo_->setGeometry(left_x, top_y, cam_w, cam_h);
    camera_model_combo_->raise();
    if (home_button_ != nullptr) {
      home_button_->setGeometry(left_x + cam_w + kGap, top_y, home_w, cam_h);
      // Icon-only square button: size the glyph to nearly fill it (minus border +
      // padding) so it doesn't render at the tiny default QToolButton icon size.
      const int home_icon_dim = std::max(home_w - 8, 1);
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
  return {it->second.is_orphan, it->second.reason};
}

void Scene3DDockWidget::setLayerVisible(ObjectTopicId topic_id, bool visible) {
  SceneDockWidget::setLayerVisible(topic_id, visible);
}

void Scene3DDockWidget::recomputeOrphanStates() {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const QString fixed = currentFixedFrame();
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
      } else if (!tf_buffer_->latestCommonTime(fixed_std, src_std).has_value()) {
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
}

QDomElement Scene3DDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = SceneDockWidget::xmlSaveState(doc);
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
  return root;
}

bool Scene3DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("scene3d")) {
    return false;
  }

  orphan_states_.clear();
  fallback_frames_.clear();

  const QString saved_mode = element.attribute(QStringLiteral("fixed_frame_mode"), QStringLiteral("auto_root"));
  const QString saved_frame = element.attribute(QStringLiteral("fixed_frame"));

  if (!SceneDockWidget::xmlLoadState(element)) {
    return false;
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
