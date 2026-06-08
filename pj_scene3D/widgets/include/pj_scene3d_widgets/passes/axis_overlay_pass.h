#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>

#include "pj_scene3d_widgets/gizmos/arrow_gizmo.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Camera-orientation HUD: three solid colored arrows (ArrowGizmo) oriented
// along world +X / +Y / +Z, drawn in a corner of the viewport. The triad
// rotates as the user orbits — shows where world axes point in screen
// space. No background panel; arrows sit directly over the scene. Does
// not depend on TF (ignores the FrameContext arg in render()).
class AxisOverlayPass : public IRenderPass {
 public:
  enum class Corner { kTopLeft, kTopRight, kBottomLeft, kBottomRight };

  // Default HUD footprint in widget pixels, exposed so layout code that reserves
  // space beside the HUD (e.g. the Scene3D overlay controls) doesn't hardcode
  // these literals. Mirrored by the member initializers below.
  static constexpr int kDefaultSizePx = 80;
  static constexpr int kDefaultMarginPx = 0;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // HUD geometry in widget-relative pixels. Defaults: 80×80 px, 0 px margin.
  // Clamped to size_px >= 1 and margin_px >= 0 to keep glViewport / glScissor
  // out of GL_INVALID_VALUE territory.
  void setSize(int size_px, int margin_px) {
    size_px_ = (size_px > 0) ? size_px : 1;
    margin_px_ = (margin_px > 0) ? margin_px : 0;
  }

  // Which corner of the viewport the HUD sits in. Default: kTopRight.
  void setCorner(Corner corner) {
    corner_ = corner;
  }

  // Arrow geometry knobs (length / shaft radius / head length / head radius
  // / tessellation). Forwarded to the underlying ArrowGizmo. Safe to call
  // before initializeGL — the params are remembered and applied on init.
  void setArrowParams(const ArrowGizmo::Params& params);

 private:
  int size_px_ = kDefaultSizePx;
  int margin_px_ = kDefaultMarginPx;
  Corner corner_ = Corner::kTopRight;
  bool initialized_ = false;
  ArrowGizmo arrow_;
  // HUD-specific defaults (unit-axes fit inside the [-1.3, 1.3] HUD frustum).
  // Designated initializers keep member order aligned with ArrowGizmo::Params.
  ArrowGizmo::Params arrow_params_{
      .length = 0.95f,
      .shaft_radius = 0.05f,
      .head_length = 0.30f,
      .head_radius = 0.13f,
      .segments = 24,
  };
};

}  // namespace pj::scene3d
