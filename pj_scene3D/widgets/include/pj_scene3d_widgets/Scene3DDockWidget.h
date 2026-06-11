#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"

class QResizeEvent;
class QToolButton;
class QWidget;

namespace pj::scene3d {
class SceneViewWidget;
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class ComboBox;

// Dock content for 3D scene topics. Generic layer lifetime, ordering,
// visibility, tracker-time dispatch, and XML layer persistence live in
// SceneDockWidget; this subclass owns 3D-only state: the OpenGL view, the
// per-dataset TF buffer, the fixed-frame overlay, fallback frames, and orphan
// detection.
class Scene3DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene3DDockWidget(QWidget* parent = nullptr);
  ~Scene3DDockWidget() override;

  using SceneDockWidget::setSessionManager;

  void setTransformService(pj::scene3d::TransformService* service);

  /// Single source of truth for the canonical object types the 3D scene family
  /// handles: render layers (kPointCloud, kCompressedPointCloud, kOccupancyGrid,
  /// kSceneEntities — see acceptsObjectType) plus scene-wide config topics
  /// (kFrameTransforms via handleSceneConfigTopic). Host-side drop routing reads this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  bool setSceneTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
    return addTopic(topic_id, object_type, title);
  }

  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  void onTrackerTime(double time) override;

  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  enum class FixedFrameMode { kAutoRoot, kExplicit };

  [[nodiscard]] QList<pj::scene3d::FrameRow> availableFrames() const {
    return available_frames_;
  }
  [[nodiscard]] QString currentFixedFrame() const;
  [[nodiscard]] bool isAutoRootMode() const {
    return fixed_frame_mode_ == FixedFrameMode::kAutoRoot;
  }

  /// True when the topic has a live layer that is currently visible.
  [[nodiscard]] bool layerVisible(ObjectTopicId topic_id) const;

  struct OrphanSnapshot {
    bool is_orphan = false;
    QString reason;
  };
  [[nodiscard]] OrphanSnapshot orphanState(ObjectTopicId topic_id) const;

 public slots:
  void setFixedFrame(const QString& frame);
  void setFixedFrameAutoRoot();
  void setLayerVisible(ObjectTopicId topic_id, bool visible);

 signals:
  void availableFramesChanged(const QList<pj::scene3d::FrameRow>& frames);
  void currentFixedFrameChanged(const QString& frame);
  void fixedFrameModeChanged(bool is_auto_root);

 protected:
  QWidget* createSceneView() override;
  std::unique_ptr<SceneLayerContext> makeContext() override;
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  bool handleSceneConfigTopic(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  void refreshView() override;
  [[nodiscard]] QString xmlTag() const override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onAvailableFrames(const QList<pj::scene3d::FrameRow>& frames);

 private:
  void prepareTransformBufferForTopic(ObjectTopicId topic_id);
  void wireScene3DLayer(pj::scene3d::Scene3DLayer* layer);
  void absorbFallbackFrames(pj::scene3d::Scene3DLayer* layer);
  void applyResolvedFixedFrame(const QString& frame);
  void refreshFrameOverlayCombo();
  void onOverlayFramePicked(int index);
  void layoutFrameOverlayCombo();
  // Union worldBounds() over the current view entities and push the result to the
  // camera (drives adaptive near/far + framing). Cheap; called on layer changes
  // and on tracker-time changes (cloud/grid geometry moves over time).
  void updateSceneBounds();
  void recomputeOrphanStates();

  pj::scene3d::TransformService* transform_service_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_buffer_;

  QList<pj::scene3d::FrameRow> available_frames_;
  std::vector<std::string> fallback_frames_;
  FixedFrameMode fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  ComboBox* frame_overlay_combo_ = nullptr;
  // Top-right overlay controls (children of `this`, not the QOpenGLWidget — z-order):
  // the camera-model selector and a Home (reset-to-default-view) button, anchored
  // to the left of the orientation gizmo.
  ComboBox* camera_model_combo_ = nullptr;
  QToolButton* home_button_ = nullptr;

  /// Cached per-topic orphan/warning state, so recomputeOrphanStates() only
  /// emits layerWarningChanged when a layer's frame-resolvability actually flips.
  struct LayerOrphanState {
    bool is_orphan = false;
    QString reason;
  };
  std::unordered_map<int64_t, LayerOrphanState> orphan_states_;
};

}  // namespace PJ
