#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gizmos/arrow_gizmo.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Per-TF-frame axis triad: three solid colored arrows (ArrowGizmo) drawn
// at every frame in the TransformBuffer, each oriented along that frame's
// local +X / +Y / +Z. Uses the same gizmo class as the camera-orientation
// HUD so visuals are consistent across the scene.
class AxisRenderPass : public IRenderPass {
 public:
  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  void setAxisLength(float length_m);
  [[nodiscard]] float axisLength() const noexcept;

 private:
  void rebuildGizmo();

  // Defaults: 0.15 m total length, thin shaft and small cone head — proper
  // 3D arrows at the scale you'd expect for a small robot's TF frames.
  // Per-axis radii are scaled off length so changes to setAxisLength keep
  // the silhouette aesthetically consistent.
  float axis_length_{0.15f};
  bool initialized_{false};
  ArrowGizmo arrow_;
};

}  // namespace pj::scene3d
