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
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
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

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  // The grid as last reconstructed by renderAt (introspection for the
  // store-window arithmetic); the reference is valid until the next renderAt.
  [[nodiscard]] const ReconstructedGrid& reconstructedGridForTest() const {
    return reconstructor_.grid();
  }
#endif

 private:
  // Decode the first base sample at attach to learn the source frame + time
  // range before render is called.
  bool bootstrap();
  // Reconstruct the grid at time_ns and stage it into the render pass.
  void renderAt(int64_t time_ns);
  // Drop the base-keyframe memo, the updates-topic cursor, and all reconstructor
  // state. Called on attach/detach: the previous attachment's epoch identity and
  // snapshots embed a timeline that may no longer exist (dataset replace, new
  // session) — same shape as SceneEntitiesLayer::resetReplayState().
  void resetStreamingState();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.
  std::optional<PJ::ObjectTopicId> updates_topic_;

  // Memo of the last parsed base keyframe, keyed by its store entry's
  // SequentialUID (stable across front-eviction; minted fresh by dataset
  // replace/flush, so a reload can never serve a stale grid). The copy's anchor
  // keeps the decoded cell bytes alive. Without it, playback over a static map
  // re-parses + deep-copies the full cell payload on every tracker tick.
  PJ::SequentialUID base_cache_uid_{};
  std::optional<PJ::sdk::OccupancyGrid> base_cache_;

  // Retroactive-ingest detector for the '_updates' sibling. The live drive
  // follows the fastest topic's edge, so the reconstructor's forward
  // (last_t_, t] window can advance past the updates topic's ingest; an update
  // landing later with ts <= the consumed time would be skipped forever.
  // updates_cursor_ is the highest updates-topic UID accounted for at the last
  // renderAt; last_consumed_time_ is the high-water reconstruction time. Both
  // only grow between resets — renderAt() invalidates the reconstructor when an
  // entry appears past the cursor with a timestamp at-or-before the high-water.
  PJ::SequentialUID updates_cursor_{};
  std::optional<PJ::Timestamp> last_consumed_time_;

  std::string source_frame_;
  // Set by setTrackerTime, consumed by render(): the reconstruction (base parse +
  // update folds + texture upload) is deferred to the next painted frame instead
  // of running eagerly per tracker tick. Qt coalesces repaints, so a fast scrub
  // reconstructs only the final landed grid, not every skipped intermediate frame.
  bool tracker_dirty_ = false;
  bool visible_ = true;

  // Per-instance display params; mirrored into grid_pass_ and edited through
  // createConfigWidget().
  OccupancyGridRenderPass::ColorScheme color_scheme_ = OccupancyGridRenderPass::ColorScheme::kMap;
  float opacity_ = 0.7f;

  OccupancyGridReconstructor reconstructor_;
  OccupancyGridRenderPass grid_pass_;
};

}  // namespace pj::scene3d
