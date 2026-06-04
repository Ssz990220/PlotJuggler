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
#include "pj_datastore/object_store.hpp"
#include "pj_scene_common/scene_dock_widget.h"

namespace PJ {

class CodecPipeline;
class CompositeMediaSource;
class MediaViewerWidget;
class SessionManager;

class Scene2DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene2DDockWidget(QWidget* parent = nullptr);
  ~Scene2DDockWidget() override;

  void setSessionManager(SessionManager* session);
  bool setImageTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

  [[nodiscard]] static std::unique_ptr<CodecPipeline> makePipelineFor(sdk::BuiltinObjectType object_type);

  /// Single source of truth for the canonical object types the 2D scene family
  /// handles (render layers; the 2D family has no scene-wide config topics).
  /// Host-side drop routing and acceptsObjectType() both read this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  [[nodiscard]] size_t compositeLayerCountForTesting() const noexcept;
  [[nodiscard]] std::vector<ObjectTopicId> compositeTopicOrderForTesting() const;

 protected:
  [[nodiscard]] QString xmlTag() const override;
  QWidget* createSceneView() override;
  std::unique_ptr<SceneLayerContext> makeContext() override;
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  void refreshView() override;

 private:
  void reconnectLiveSamples(SessionManager* session);
  void syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers);
  [[nodiscard]] std::optional<int64_t> seedTimestampNs(const std::vector<ISceneLayer*>& ordered_layers) const;

  MediaViewerWidget* bootstrap_ = nullptr;
  MediaViewerWidget* viewer_ = nullptr;
  std::unique_ptr<CompositeMediaSource> composite_;
  QMetaObject::Connection live_samples_conn_;
  std::vector<ObjectTopicId> composite_topic_order_;
};

}  // namespace PJ
