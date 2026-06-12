// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"
#include "pj_scene3d_widgets/passes/occupancy_grid_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for a nav_msgs/OccupancyGrid base topic plus its optional
// map_msgs/OccupancyGridUpdate sibling ("<base>_updates"). On every tracker
// tick it asks OccupancyGridReconstructor for the grid as displayed at that
// time (base keyframe + applicable deltas, correct under back-and-forth
// scrubbing) and pushes it to an OccupancyGridRenderPass.
class OccupancyGridLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  OccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~OccupancyGridLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  [[nodiscard]] std::optional<AABB> worldBounds() const override;

  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-instance display params, driven by the config widget.
  void setColorScheme(OccupancyGridRenderPass::ColorScheme scheme);
  void setOpacity(float opacity);
  [[nodiscard]] OccupancyGridRenderPass::ColorScheme colorScheme() const {
    return color_scheme_;
  }
  [[nodiscard]] float opacity() const {
    return opacity_;
  }

 private:
  // Decode the first base sample at attach to learn the source frame + time
  // range before render is called.
  bool bootstrap();
  // Reconstruct the grid at time_ns and stage it into the render pass.
  void renderAt(int64_t time_ns);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.
  std::optional<PJ::ObjectTopicId> updates_topic_;

  std::string source_frame_;
  QString fixed_frame_;
  PJ::Timepoint tracker_time_{};
  bool visible_ = true;

  // Per-instance display params; mirrored into grid_pass_ and edited through
  // createConfigWidget().
  OccupancyGridRenderPass::ColorScheme color_scheme_ = OccupancyGridRenderPass::ColorScheme::kMap;
  float opacity_ = 0.7f;

  OccupancyGridReconstructor reconstructor_;
  OccupancyGridRenderPass grid_pass_;
};

}  // namespace pj::scene3d
