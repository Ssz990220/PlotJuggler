// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <vector>

#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

// Screen-space ambient occlusion over the resolved single-sample depth texture
// (plan §A.5). Two R16F targets: raw hemisphere-kernel AO, then a 4x4 box blur.
// The composite multiplies its output into the HDR color before tonemapping.
//
// Runs OUTSIDE the MSAA scene FBO, after SceneHdrFbo::resolve(): the caller
// binds the input via setDepthTexture() and must leave depth-test/blend/scissor
// disabled (paintGL's post-chain state). Per-context like every pass: the
// caller's releaseGlResources() drives releaseGL(); resize() lazily reallocates
// in the next context.
class SsaoPass : public IPostPass {
 public:
  void initializeGL() override;
  // IRenderPass-compatible no-op (FrameContext is irrelevant to a post pass);
  // the real entry point is renderAo().
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  void resize(int width_px, int height_px) override;

  // The resolved single-sample DEPTH_COMPONENT32F texture to sample.
  void setDepthTexture(GLuint depth_texture_id) {
    depth_texture_id_ = depth_texture_id;
  }

  // Tunables (plan §A.5): sampling radius in metres and the occlusion contrast
  // exponent. Read per frame; safe to change from the GUI thread.
  void setRadius(float radius_m) {
    radius_m_ = radius_m;
  }
  void setPower(float ao_power) {
    ao_power_ = ao_power;
  }

  // Compute raw AO + blur into the internal targets. Device-pixel viewport is
  // taken from the last resize(). No-op (output stays invalid) until both FBOs
  // are complete and the programs compiled.
  void renderAo(const ViewParams& view_params);

  // Blurred AO (R16F, 1 = unoccluded). 0 when the pass is not ready — the
  // composite must then skip the AO multiply (u_has_ao = 0).
  [[nodiscard]] GLuint outputTextureId() const noexcept;
  [[nodiscard]] bool ready() const noexcept;

 private:
  bool buildPrograms();

  std::optional<gl::Program> ssao_program_;
  std::optional<gl::Program> blur_program_;
  gl::VertexArray fullscreen_vao_;
  gl::Framebuffer raw_fbo_;
  gl::Framebuffer blur_fbo_;
  gl::Texture raw_ao_;
  gl::Texture blur_ao_;
  std::vector<glm::vec3> kernel_;
  GLuint depth_texture_id_{0};
  float radius_m_{look::kSsaoRadiusM};
  float ao_power_{look::kSsaoPower};
  int width_{0};
  int height_{0};
  bool targets_ready_{false};
  bool initialized_{false};
  // Latches that initializeGL ran in THIS context (whether or not compilation
  // succeeded), so a per-context shader failure isn't re-attempted every frame.
  // Cleared by releaseGL() so context recreation rebuilds.
  bool attempted_{false};
};

}  // namespace pj::scene3d
