#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_runtime/Time.h"  // PJ::Timepoint, fromRaw/toRaw
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

class SessionManager;

/// Backend-agnostic dock base that owns an ordered stack of scene layers.
///
/// Subclasses provide the concrete scene view, accepted object types, layer
/// context, and the hook that maps the ordered layer list into the renderer.
class SceneDockWidget : public QWidget, public IDataWidget, public IObjectViewer {
  Q_OBJECT
 public:
  explicit SceneDockWidget(QWidget* parent = nullptr);
  ~SceneDockWidget() override;

  QWidget* widget() override;
  void onTrackerTime(double time) override;
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  /// IObjectViewer: drops layers whose ObjectStore topic was evicted (empty
  /// descriptor) via the regular removal path, and reports whether any live
  /// layer remains so the shell can reset an emptied dock to its placeholder.
  bool revalidateObjects() override;

  /// Accepts a topic as either a render layer or a scene-wide config topic.
  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  void removeTopic(ObjectTopicId topic_id);
  void setLayerVisible(ObjectTopicId topic_id, bool visible);

  /// Applies a partial or complete draw order, appending omitted layers after it.
  void reorderLayers(const std::vector<ObjectTopicId>& ordered_topic_ids);

  /// Returns layer snapshots in draw order.
  [[nodiscard]] std::vector<SceneLayerInfo> layers() const;

  /// Returns a non-owning pointer to the layer for a topic, or nullptr.
  [[nodiscard]] ISceneLayer* layerFor(ObjectTopicId topic_id) const;

  void setSessionManager(SessionManager* session);

 signals:
  void layerAdded(ObjectTopicId topic_id);
  void layerRemoved(ObjectTopicId topic_id);
  void layerVisibilityChanged(ObjectTopicId topic_id, bool visible);
  void layerWarningChanged(ObjectTopicId topic_id, bool warn, QString reason);

 protected:
  /// Registry used by subclasses to register their supported layer types.
  [[nodiscard]] LayerFactory& layerFactory();
  [[nodiscard]] const LayerFactory& layerFactory() const;
  [[nodiscard]] SessionManager* sessionManager() const;

  /// XML root tag for this scene family. Defaults to "scene".
  [[nodiscard]] virtual QString xmlTag() const;

  /// Creates the scene widget hosted by this dock; called lazily by the base.
  virtual QWidget* createSceneView() = 0;

  /// Creates the context passed to newly attached layers.
  virtual std::unique_ptr<SceneLayerContext> makeContext() = 0;

  /// Filters object topics before layer construction.
  [[nodiscard]] virtual bool acceptsObjectType(sdk::BuiltinObjectType object_type) const = 0;

  /// Allows subclasses to consume scene-wide config topics without adding a layer.
  virtual bool handleSceneConfigTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  /// Reconciles the concrete view with the current draw order.
  ///
  /// Pointers are owned by this dock and remain valid until the next removal or
  /// clear operation.
  virtual void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) = 0;

  /// Requests a view repaint after layer state changes.
  virtual void refreshView();

  /// Last tracker time forwarded to the layers (ns, already clamped by
  /// clampToLayerRange), or nullopt when no tracker tick (or live nudge) has
  /// arrived yet — 0 is a valid timestamp, so absence is explicit. For derived
  /// docks that drive an additional consumer off the same clock (the 3D view's
  /// render time, the 2D composite seed).
  [[nodiscard]] std::optional<int64_t> lastTrackerNs() const {
    return last_tracker_.has_value() ? std::optional<int64_t>{PJ::toRaw(*last_tracker_)} : std::nullopt;
  }

  /// Records an externally-derived tracker time (e.g. a live-ingest nudge to
  /// the data edge) as the seed for future layer additions and rebuild seeding,
  /// without driving the layers (the caller already did). Takes a raw int64-ns
  /// (the spine edge) and lifts it to a Timepoint for internal storage.
  void noteTrackerTime(int64_t time_ns) {
    last_tracker_ = PJ::fromRaw(time_ns);
  }

  /// Removes every layer: re-points the concrete view off the old layers
  /// (syncViewLayers with an empty order) while they are still alive, then
  /// detaches and destroys them. Concrete docks whose scene view holds raw
  /// layer pointers (e.g. the 3D view's layer list) MUST call this from their
  /// own destructor, while virtual dispatch still reaches their overrides —
  /// the base destructor cannot reconcile the view for them.
  void clearLayers();

 private:
  /// Result of an add attempt, separating the two outcomes addTopic's bool used
  /// to conflate ("layer created" vs "consumed as a scene-config topic").
  enum class AddOutcome { LayerAdded, ConsumedAsConfig, Rejected };

  void ensureSceneViewCreated();
  [[nodiscard]] PJ::Timepoint clampToLayerRange(PJ::Timepoint time) const;
  [[nodiscard]] std::vector<ISceneLayer*> orderedLayerPtrs() const;
  void syncViewLayers();
  /// Destructor-safe teardown: detaches and destroys layers WITHOUT touching
  /// the (pure virtual) view reconciliation. Only for ~SceneDockWidget.
  void destroyLayersUnsynced();
  void recordLayerVisibility(ObjectTopicId topic_id, bool visible);

  /// Orchestrates adding a topic as a render layer or scene-config topic.
  AddOutcome addLayer(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Creates and attaches a layer; returns nullptr if unsupported or attach fails.
  [[nodiscard]] std::unique_ptr<ISceneLayer> createAndAttachLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Connects a layer's signals to the dock's reconcile/notify slots.
  void wireLayerSignals(ISceneLayer* layer, ObjectTopicId topic_id);
  /// Records a constructed layer in the draw order and seeds its tracker time.
  void registerLayer(int64_t key, std::unique_ptr<ISceneLayer> layer);

  std::unordered_map<int64_t, std::unique_ptr<ISceneLayer>> layers_;
  std::vector<int64_t> draw_order_;
  LayerFactory factory_;
  SessionManager* session_ = nullptr;
  std::optional<PJ::Timepoint> last_tracker_;
  QWidget* scene_view_ = nullptr;
  std::unordered_map<int64_t, bool> layer_visibility_cache_;
};

}  // namespace PJ
