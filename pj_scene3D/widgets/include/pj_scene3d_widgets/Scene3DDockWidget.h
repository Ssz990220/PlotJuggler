#pragma once

#include <QList>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_scene3d_core/tf/tf_buffer.h"

class QComboBox;
class QResizeEvent;

namespace pj::scene3d {
class Scene3DEntity;
class SceneViewWidget;
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class SessionManager;

// Dock content for 3D scene topics. v1 supports multiple PointCloud2 topics
// per widget plus dataset-wide TF; RobotDescription (URDF) lands later.
//
// Internally the dock holds N pj::scene3d::Scene3DEntity instances keyed
// by ObjectTopicId. Each entity owns its own render pass(es), per-instance
// state, and parameter widget — the dock is type-blind beyond the
// addTopic dispatch. See `pj_scene3d_widgets/scene3d_entity.h`.
class Scene3DDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  explicit Scene3DDockWidget(QWidget* parent = nullptr);
  ~Scene3DDockWidget() override;

  void setSessionManager(SessionManager* session);

  // Source of the per-dataset TF buffer. Owned by the app shell rather than
  // the domain-neutral runtime; inject before adding topics.
  void setTransformService(pj::scene3d::TransformService* service);

  // Forward pj_app's active theme name ("light" or "dark") to the
  // underlying SceneViewWidget so the 3D background tracks the host theme.
  void setThemeHint(const QString& theme);

  // Add a topic to the scene. Constructs the right Scene3DEntity for
  // the given builtin family via an internal switch:
  //   kPointCloud      → PointCloudEntity
  //   kFrameTransforms → no entity (TF is dataset-wide; just binds the
  //                      session's TF buffer to the view if not already)
  //   others           → returns false
  // Idempotent on the same topic_id. Returns false if the topic has no
  // registered parser or the entity's attach() fails.
  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  // Remove a topic's entity from the scene. Destroys its per-instance
  // state and render passes. No-op if topic_id isn't currently attached.
  void removeTopic(ObjectTopicId topic_id);

  // Drop entry point used by MainWindow's ObjectWidgetFactory. Thin
  // wrapper around addTopic.
  bool setSceneTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
    return addTopic(topic_id, object_type, title);
  }

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;

  // Layout persistence. Produces a <scene3d> element carrying:
  //   - version="1" (schema versioning)
  //   - fixed_frame_mode + fixed_frame attributes (dock-wide state)
  //   - one <entity kind=... dataset_id=... topic_name=... object_type=...
  //     display_name=... visible=...> child per attached topic, with
  //     the entity's own xmlSaveState payload nested inside.
  // Topic resolution at load goes through
  // SessionManager::objectStore().findTopic(dataset_id, topic_name) — if a
  // topic isn't present in the current dataset (e.g. a different MCAP was
  // loaded), the entry is skipped silently.
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  // IDataWidget hook so DockWidget can offer additional drops to an
  // already-mounted Scene3DDockWidget instead of replacing it. Accepts the
  // same family setSceneTopic accepts (PointCloud, FrameTransforms); refuses
  // anything else so the host falls back to factory replacement.
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;

  // ---- API for Scene3DConfigPanel ----

  // Fixed-frame selection mode. AutoRoot follows the discovered TF root
  // dynamically as TFs arrive — the overlay combo just shows whichever
  // named frame got auto-resolved; there is no synthetic sentinel row.
  // Explicit holds whatever named frame the user picked.
  enum class FixedFrameMode { kAutoRoot, kExplicit };

  // Widget-global fixed-frame state.
  [[nodiscard]] QList<pj::scene3d::FrameRow> availableFrames() const {
    return available_frames_;
  }
  [[nodiscard]] QString currentFixedFrame() const;
  [[nodiscard]] bool isAutoRootMode() const {
    return fixed_frame_mode_ == FixedFrameMode::kAutoRoot;
  }

  // Per-topic queries. TopicInfo carries the minimum the panel needs to
  // populate its list row; per-instance parameter UI is fetched via
  // entityById(topic_id)->createConfigWidget(parent).
  struct TopicInfo {
    ObjectTopicId topic_id;
    QString display_name;
    bool visible;
  };
  [[nodiscard]] std::vector<TopicInfo> entities() const;
  [[nodiscard]] bool topicVisible(ObjectTopicId topic_id) const;

  // Returns the entity for a given topic, or nullptr if not attached.
  // The Scene3DConfigPanel calls createConfigWidget on this when a topic
  // is selected — per-instance parameter UI is owned by the entity, not
  // by the panel.
  [[nodiscard]] pj::scene3d::Scene3DEntity* entityFor(ObjectTopicId topic_id) const;

  // Current orphan state for the entity, used by the panel to replay
  // visuals when binding to an already-populated dock. Returns
  // {false, ""} if the topic isn't attached or isn't orphan.
  struct OrphanSnapshot {
    bool is_orphan = false;
    QString reason;
  };
  [[nodiscard]] OrphanSnapshot orphanState(ObjectTopicId topic_id) const;

 public slots:
  // Flip to Explicit mode and hold `frame` regardless of subsequent TF
  // arrivals. The overlay combo only ever calls this; the AutoRoot mode is
  // entered only at bootstrap.
  void setFixedFrame(const QString& frame);
  // Flip to AutoRoot mode and immediately re-resolve the root from the
  // current available_frames_ via pickFixedFrame. No UI affordance calls
  // this today — kept as a programmatic hook for tests / future restore.
  void setFixedFrameAutoRoot();
  void setTopicVisible(ObjectTopicId topic_id, bool visible);

  // Set the render (draw) order from a topic-id order. Index 0 renders first
  // (behind); the last renders on top — for coplanar overlays (costmaps) this
  // decides the overlap winner. Ids not currently attached are skipped; the
  // view ignores the result unless it forms a full permutation. Driven by the
  // Topics panel's drag-reorder.
  void reorderEntities(const std::vector<ObjectTopicId>& ordered_topic_ids);

 signals:
  // Widget-global (fixed-frame) events.
  void availableFramesChanged(const QList<pj::scene3d::FrameRow>& frames);
  void currentFixedFrameChanged(const QString& frame);
  // Fires only when the mode itself flips. Currently only meaningful for
  // tests / programmatic observers since the overlay combo no longer
  // distinguishes AutoRoot vs Explicit visually.
  void fixedFrameModeChanged(bool is_auto_root);

  // Per-entity events — type-blind. The panel uses entityFor(topic_id)
  // to fetch the Scene3DEntity* and inspect its info() / build its
  // config widget; the dock does not surface drawable-kind specifics.
  void entityAdded(ObjectTopicId topic_id);
  void entityRemoved(ObjectTopicId topic_id);
  void entityVisibilityChanged(ObjectTopicId topic_id, bool visible);
  // The entity's source frame is (no longer) reachable from the dock's
  // currently selected fixed frame. `reason` is a localized message
  // suitable for a tooltip explaining why; empty when is_orphan == false.
  void entityOrphanChanged(ObjectTopicId topic_id, bool is_orphan, const QString& reason);

 private slots:
  void onAvailableFrames(const QList<pj::scene3d::FrameRow>& frames);

 private:
  // Clamp incoming tracker time to the union of all entities' time ranges.
  [[nodiscard]] int64_t clampToEntityRange(int64_t time_ns) const;
  // Merge a freshly-added entity's fallback frames into the global list
  // and refresh the combo.
  void absorbFallbackFrames(pj::scene3d::Scene3DEntity* entity);
  // Propagate a resolved frame name to the view + every entity, emit
  // currentFixedFrameChanged. Does NOT touch fixed_frame_mode_ — callers
  // (setFixedFrame, setFixedFrameAutoRoot, onAvailableFrames bootstrap)
  // own the mode decision.
  void applyResolvedFixedFrame(const QString& frame);
  // Sync the floating top-left frame combo to the current available_frames_,
  // fixed_frame_, and fixed_frame_mode_. Cheap (~tens of items).
  void refreshFrameOverlayCombo();
  // Slot for the floating combo's currentIndexChanged. Reads the current
  // item's UserRole (the bare frame name) and calls setFixedFrame.
  void onOverlayFramePicked(int index);
  // Re-evaluate orphan status for every attached entity. Event-driven —
  // called when fixed_frame changes, when the TF graph changes, when an
  // entity's source frame changes, or on entity attach. Emits
  // entityOrphanChanged for any entity whose state flipped.
  void recomputeOrphanStates();

  SessionManager* session_ = nullptr;
  pj::scene3d::TransformService* transform_service_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  // The view is a native QOpenGLWindow; this is the QWidget that embeds it in
  // the layout (QWidget::createWindowContainer). Owns view_'s widget lifetime.
  QWidget* view_container_ = nullptr;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_buffer_;

  // Owning entity registry, keyed by ObjectTopicId.id.
  std::unordered_map<int64_t, std::unique_ptr<pj::scene3d::Scene3DEntity>> entities_;

  // Last tracker time (ns) pushed to entities by onTrackerTime, used to seed a
  // freshly-added entity so it renders at the current time immediately rather
  // than staying blank until the next playback tick.
  int64_t last_tracker_ns_ = 0;

  // Cached frames so a freshly-bound config panel can populate combos
  // without waiting for the next signal.
  QList<pj::scene3d::FrameRow> available_frames_;
  // Aggregate fallback frames across all entities so the user always
  // has at least one resolvable frame to pick.
  std::vector<std::string> fallback_frames_;

  // AutoRoot until the user explicitly picks a named frame.
  FixedFrameMode fixed_frame_mode_ = FixedFrameMode::kAutoRoot;

  // Floating fixed-frame combo painted on top of view_ at the top-left. A
  // child of `this` (not of view_), so resizeEvent on the dock places it.
  // Hosting it here keeps the sidepanel type-blind: only the dock owns the
  // fixed-frame UX.
  QComboBox* frame_overlay_combo_ = nullptr;

  // Per-entity orphan state. An entity is "orphan" when its primary
  // sourceFrame() can't be transformed into the current fixed frame at
  // the current tracker time. Empty entries mean the entity is reachable
  // (or has not yet decoded any data — Scene3DEntity::sourceFrame()
  // returns empty until then). Kept in the dock because orphan-ness is a
  // function of three pieces of dock-owned state (fixed frame, TF buffer,
  // entity source frame) — putting it on the entity would invert the
  // dependency direction.
  struct EntityOrphanState {
    bool is_orphan = false;
    QString reason;
  };
  std::unordered_map<int64_t, EntityOrphanState> orphan_states_;
};

}  // namespace PJ
