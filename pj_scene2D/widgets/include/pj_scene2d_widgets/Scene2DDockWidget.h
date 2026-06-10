#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMetaObject>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene_common/scene_dock_widget.h"

namespace PJ {

class CompositeMediaSource;
class MediaViewerWidget;
class SessionManager;

/// 2D scene dock on top of pj_scene_common's layered SceneDockWidget.
///
/// Registers image/video/depth/annotation/entity layer types, wires their
/// MediaSources into a CompositeMediaSource, and nudges live layers to the newest
/// ObjectStore sample when streaming data arrives.
class Scene2DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene2DDockWidget(QWidget* parent = nullptr);
  ~Scene2DDockWidget() override;

  /// Stores a non-owning SessionManager pointer in the base and reconnects the
  /// live-sample follow connection. Replacing the session drops the old connection.
  void setSessionManager(SessionManager* session);
  /// Adds a 2D render layer for the topic. Returns false when the object type is
  /// unsupported or layer attach fails; success also updates the dock title.
  bool setImageTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Forwards the global point-inspector toggle to the viewer if it already exists;
  /// a pre-create call is intentionally not latched.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

  /// Single source of truth for the canonical object types the 2D scene family
  /// handles (render layers; the 2D family has no scene-wide config topics).
  /// Host-side drop routing and acceptsObjectType() both read this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  [[nodiscard]] size_t compositeLayerCountForTesting() const noexcept;
  /// Mirrors the visible CompositeMediaSource layer order after syncViewLayers().
  [[nodiscard]] std::vector<ObjectTopicId> compositeTopicOrderForTesting() const;

 protected:
  /// Workspace XML tag for the 2D scene dock.
  [[nodiscard]] QString xmlTag() const override;
  /// Builds the QRhi bootstrap child plus the real MediaViewerWidget.
  QWidget* createSceneView() override;
  /// Supplies the current session pointer to Scene2DLayer::attach().
  std::unique_ptr<SceneLayerContext> makeContext() override;
  /// Keeps generic SceneDockWidget routing aligned with the static host classifier.
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  /// Rebuilds the visible-layer CompositeMediaSource in draw order and repoints
  /// the viewer before the old composite is destroyed.
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  /// Repaints only the viewer; the layer list UI is owned by the base dock.
  void refreshView() override;

 private:
  /// Connects live ObjectStore ingestion to jump visible layers to the data edge.
  void reconnectLiveSamples(SessionManager* session);
  /// Drives visible layers and the 2D composite to the newest stored sample.
  void driveVisibleLayersToLiveEdge();
  /// Seeds a freshly rebuilt composite from the last tracker/live time, or from
  /// the first visible layer with a retained tracker timestamp.
  void syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers);
  /// Returns raw nanoseconds because CompositeMediaSource is the core-side seam.
  [[nodiscard]] std::optional<int64_t> seedTimestampNs(const std::vector<ISceneLayer*>& ordered_layers) const;

  // Zero-size QRhiWidget kept as a child so Qt 6.8 creates an RHI-backed backing
  // store on first show(); see TECHNICAL_NOTES.md "QRhiWidget Multi-Instance Lifecycle".
  MediaViewerWidget* bootstrap_ = nullptr;
  // The real viewer is non-owning here; Qt parent ownership is the container made
  // by createSceneView(), while composite_ owns the source it polls.
  MediaViewerWidget* viewer_ = nullptr;
  std::unique_ptr<CompositeMediaSource> composite_;
  // Live-follow subscription; reconnectLiveSamples owns disconnect/replacement.
  QMetaObject::Connection live_samples_conn_;
  // Visible topic ids in the exact order pushed into composite_; testing reads it
  // to verify layer reorder/add/remove reconciliation.
  std::vector<ObjectTopicId> composite_topic_order_;
};

}  // namespace PJ
