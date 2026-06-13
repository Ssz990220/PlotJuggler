#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string>
#include <vector>

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

  // Set the triad arrow length in metres. Safe to call from any GUI slot with
  // no GL context current: the length is remembered and the gizmo is rebuilt on
  // the next paint (render()), where the owning context is guaranteed current.
  void setAxisLength(float length_m);
  [[nodiscard]] float axisLength() const noexcept;

  // Triad opacity (Part C "Gizmos opacity"): 1.0 = solid annotation (today's
  // look); lower values blend with the scene via the annotation blend mode.
  void setOpacity(float opacity) {
    opacity_ = opacity;
  }
  [[nodiscard]] float opacity() const noexcept {
    return opacity_;
  }

 private:
  // Defaults: 0.15 m total length, thin shaft and small cone head — proper
  // 3D arrows at the scale you'd expect for a small robot's TF frames.
  // Per-axis radii are scaled off length so changes to setAxisLength keep
  // the silhouette aesthetically consistent.
  float axis_length_{0.15f};
  float opacity_{1.0f};
  bool initialized_{false};
  // Set when setAxisLength changes the length while initialized_; consumed at
  // the top of render() so the GL rebuild runs under a current context.
  bool gizmo_dirty_{false};
  ArrowGizmo arrow_;
  // Reused across frames by render() to avoid a per-frame heap allocation of
  // the frame-name list (L.54). Only ever touched on the render thread.
  std::vector<std::string> frames_scratch_;
};

}  // namespace pj::scene3d
