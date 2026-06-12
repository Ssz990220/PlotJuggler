// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Eye-dome lighting over the resolved single-sample depth texture (plan §A.4):
// a per-pixel shade factor from the log-depth gap to 8 circular neighbours,
// darkening silhouettes and depth discontinuities — the classic point-cloud
// contour cue. One R16F target; the composite multiplies it into the HDR color.
//
// Same contract as SsaoPass: runs after SceneHdrFbo::resolve() in the
// post-chain state (depth/blend/scissor off); per-context lifecycle via
// releaseGL(); degrades to "no effect" when not ready (composite gates on
// u_has_edl).
class EdlPass : public IPostPass {
 public:
  void initializeGL() override;
  // IRenderPass-compatible no-op; the real entry point is renderEdl().
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  void resize(int width_px, int height_px) override;

  // The resolved single-sample DEPTH_COMPONENT32F texture to sample.
  void setDepthTexture(GLuint depth_texture_id) {
    depth_texture_id_ = depth_texture_id;
  }

  // Tunables (plan §A.4). Read per frame; GUI-thread safe.
  void setStrength(float strength) {
    strength_ = strength;
  }
  void setRadiusPx(float radius_px) {
    radius_px_ = radius_px;
  }

  // Compute the shade factor into the internal target.
  void renderEdl(const ViewParams& view_params);

  // R16F shade factor (1 = no darkening). 0 when not ready.
  [[nodiscard]] GLuint outputTextureId() const noexcept;
  [[nodiscard]] bool ready() const noexcept;

 private:
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray fullscreen_vao_;
  gl::Framebuffer fbo_;
  gl::Texture output_;
  GLuint depth_texture_id_{0};
  float strength_{1.0f};
  float radius_px_{0.6f};  // User look-dev default (plan spec was 1.4)
  int width_{0};
  int height_{0};
  bool target_ready_{false};
  bool initialized_{false};
};

}  // namespace pj::scene3d
