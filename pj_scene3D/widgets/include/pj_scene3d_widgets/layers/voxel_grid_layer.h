// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_scene3d_core/voxel_grid_value.h"
#include "pj_scene3d_core/voxel_grid_view.h"
#include "pj_scene3d_widgets/passes/voxel_grid_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/Colormap.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for an sdk::VoxelGrid topic. On each tracker tick it fetches the
// grid valid at that time, packs the selected field into a dense buffer (ONCE per
// new grid / field change — re-scrubbing a seen grid does zero per-voxel CPU
// work), and stages it into a VoxelGridRenderPass which expands it to GPU-instanced
// cubes. The draw predicate, colormap, range and opacity are viewer-side display
// settings persisted in the layout XML.
class VoxelGridLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  VoxelGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~VoxelGridLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  // Layer-row warning surface: set when a grid arrives with dimensions too large
  // to render (corrupt/untrusted wire dims — see kMaxRenderableVoxels). Routed
  // through the dock's orphan-state combine so it persists across list refreshes
  // and TF recomputes, unlike a bare warningChanged emit.
  [[nodiscard]] QString statusWarning() const override {
    return status_warning_;
  }
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

  // Display params, driven by the config widget. Field selection re-packs on the
  // next render; the rest are uniform-only (no re-pack).
  void setActiveField(const QString& field_name);  // empty = auto-pick
  void setDrawMode(VoxelDrawMode mode);
  void setThreshold(double threshold);
  void setAutoRange(bool on);
  void setManualRange(double lo, double hi);
  void setColormap(PJ::Colormap colormap);
  void setOpacity(double opacity);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  [[nodiscard]] QString resolvedFieldForTest() const {
    return QString::fromStdString(resolved_field_name_);
  }
  [[nodiscard]] bool hasGridForTest() const {
    return cached_grid_.has_value();
  }
  // Whether the render pass currently holds a staged grid (vs. cleared) — distinct
  // from the layer's parse memo; used to pin the back-scrub re-stage regression.
  [[nodiscard]] bool passHasStagedGridForTest() const {
    return pass_.hasStagedGridForTest();
  }
#endif

 private:
  // Decode the first sample at attach to learn the source frame + default field.
  bool bootstrap();
  // Fetch the grid at time_ns, pack the selected field, and stage it into the pass.
  // No-ops cheaply when the same grid+field is already uploaded (the scrub path).
  void renderAt(int64_t time_ns);
  // Drop the parse memo + upload bookkeeping; called on attach/detach (a dataset
  // replace mints fresh UIDs, so a stale grid must never be served).
  void resetStreamingState();
  // Resolve which field to display for `grid`, honouring active_field_name_ and
  // falling back to chooseDefaultField. Updates resolved_field_name_ + value_kind_.
  const PJ::sdk::PointField* resolveField(const PJ::sdk::VoxelGrid& grid);
  void pushDisplayParamsToPass();
  // Set (or clear, with an empty reason) the layer-row warning; emits
  // statusWarningChanged() only on a real change so the dock re-combines once.
  void setStatusWarning(const QString& reason);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;

  // Parse memo: the last decoded grid keyed by its store entry's SequentialUID.
  // The copy's anchor keeps the (possibly zero-copy) cell bytes alive.
  PJ::SequentialUID cached_grid_uid_{};
  std::optional<PJ::sdk::VoxelGrid> cached_grid_;

  // What is currently uploaded to the GPU texture: the grid UID and the field
  // *setting* (active_field_name_) that produced it. A tracker move that resolves
  // to the same pair re-packs and re-uploads nothing (req: zero per-voxel CPU work
  // on re-scrub). Reset to invalid to force a re-pack (e.g. on field change).
  PJ::SequentialUID uploaded_uid_{};
  std::string uploaded_field_setting_{"\x01"};  // sentinel != any real "" / name

  std::string source_frame_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  // Non-empty while the layer row shows a warning for an over-cap (corrupt-dims)
  // grid; the source of truth statusWarning() returns. See setStatusWarning().
  QString status_warning_;

  // Display settings (persisted). active_field_name_ empty = auto-pick.
  std::string active_field_name_;
  std::string resolved_field_name_;  // concrete field last used (UI display only)
  VoxelValueKind value_kind_ = VoxelValueKind::kScalar;
  VoxelDrawMode draw_mode_ = VoxelDrawMode::kNonZero;
  double threshold_ = 0.0;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  double opacity_ = 1.0;

  VoxelGridRenderPass pass_;
};

}  // namespace pj::scene3d
