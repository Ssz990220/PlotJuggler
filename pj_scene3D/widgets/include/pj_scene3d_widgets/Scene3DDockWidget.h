#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"

class QResizeEvent;
class QSettings;
class QToolButton;
class QWidget;

namespace pj::scene3d {
class SceneViewWidget;
class TransformService;
class UrdfPackageResolver;
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

  // Shadows the base setter to also (re)connect the live-samples slot that feeds
  // streamed FrameTransform messages into the TF buffer (see reconnectLiveSamples).
  void setSessionManager(SessionManager* session);

  void setTransformService(pj::scene3d::TransformService* service);

  // The owned GL view, for the app's scene-controls panel (grid/gizmo/look
  // setters live on SceneViewWidget). Null until the dock realizes its view.
  [[nodiscard]] pj::scene3d::SceneViewWidget* sceneView() {
    return view_;
  }

  /// Single source of truth for the canonical object types the 3D scene family
  /// handles: render layers (kPointCloud, kCompressedPointCloud, kOccupancyGrid,
  /// kRobotDescription, kSceneEntities — see acceptsObjectType) plus scene-wide config topics
  /// (kFrameTransforms via handleSceneConfigTopic). Host-side drop routing reads this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  bool revalidateObjects() override;

  void setSettings(QSettings* settings);
  void setMcapAttachments(QMap<QString, QByteArray> attachments);

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

  // Store topics carrying a robot description (metadata builtin_object_type ==
  // kRobotDescription), for the scene-controls panel's Model/URDF topic combo.
  struct RobotDescriptionTopic {
    ObjectTopicId topic_id;
    QString name;
  };
  [[nodiscard]] QList<RobotDescriptionTopic> robotDescriptionTopics() const;

 public slots:
  // Create a local File/URL robot layer (synthetic registry id, never passed to
  // the ObjectStore). With a path, the layer loads that URDF immediately — the
  // panel's one-click "Load URDF…" flow; empty = blank layer, configure later.
  // Returns the layer's registry id ({0} on failure) so the caller can later
  // removeTopic() it — the panel's per-row remove flow.
  ObjectTopicId addRobotModelLayer(const QString& urdf_path = {});
  // Same local-layer flow, loading the URDF over http(s) instead of from disk.
  ObjectTopicId addRobotModelLayerFromUrl(const QString& url);
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
  // Connect to SessionManager::samplesIngested so streamed FrameTransform
  // messages are accumulated into the dataset's TF buffer live (file load uses
  // the bulk ingest instead). Re-callable: drops any prior connection first.
  void reconnectLiveSamples(SessionManager* session);
  // Advance every visible object layer (pointcloud / markers / occupancy) to the
  // newest timestamp now present in the ObjectStore. Streaming's samplesIngested
  // carries only scalar TopicIds, so — like the 2D dock — we ignore that list and
  // query the store directly; without this the object layers stay frozen on their
  // last decoded frame while only the TF buffer advances.
  void driveVisibleLayersToLiveEdge();
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
  [[nodiscard]] bool isLocalRobotLayerId(ObjectTopicId topic_id) const;
  ObjectTopicId allocateLocalRobotLayerId();

  pj::scene3d::TransformService* transform_service_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_buffer_;
  // Dataset that owns tf_buffer_, cached when the buffer is first bound so the
  // live-samples slot can drive incremental TF ingest without re-deriving it.
  DatasetId dataset_id_ = 0;
  // Live streamed-TF ingest hookup (SessionManager::samplesIngested).
  QMetaObject::Connection live_samples_conn_;
  std::unique_ptr<pj::scene3d::UrdfPackageResolver> package_resolver_;
  QSettings* settings_ = nullptr;
  QMap<QString, QByteArray> mcap_attachments_;
  uint32_t next_local_robot_topic_id_ = std::numeric_limits<uint32_t>::max();
  std::unordered_set<uint32_t> local_robot_layer_ids_;

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
