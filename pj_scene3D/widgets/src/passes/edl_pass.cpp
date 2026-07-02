// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/edl_pass.h"

#include <fmt/core.h>

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <variant>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

constexpr std::string_view kFullscreenVertSrc = R"GLSL(
#version 410 core
out vec2 v_uv;
void main() {
  float x = float(gl_VertexID == 1) * 4.0 - 1.0;
  float y = float(gl_VertexID == 2) * 4.0 - 1.0;
  gl_Position = vec4(x, y, 0.0, 1.0);
  v_uv = vec2(x, y) * 0.5 + 0.5;
}
)GLSL";

// EDL shade factor. Derived from Potree's EDLRenderer/edl.fs (BSD-2-Clause,
// (c) 2011-2020 Markus Schuetz; EDL algorithm: Christian Boucheny /
// CloudCompare). See pj_scene3D/THIRDPARTY.md. As in the SSAO pass, eye-space
// depth reconstructs through u_inv_proj so both perspective and orthographic
// cameras work (the log2-of-linear-depth response is projection-agnostic).
//
// MESH-ONLY contour. EDL here is restricted to mesh surfaces via the scene FBO's
// R8 "is-mesh" mask (u_mask): point clouds, grid, occupancy, axes and the empty
// background neither receive the contour nor cast one. A non-mesh pixel reads as
// "far" (FAR_SENTINEL), so a mesh pixel whose neighbour is non-mesh — or simply a
// farther mesh — darkens: the mesh's silhouette against the void / point clouds /
// farther meshes, plus the near side of its surface creases, get the contour. The
// per-neighbour gap is clamped to u_max_gap so the huge silhouette gap reads as a
// graded outline instead of a solid black band; creases (much smaller gaps) are
// unaffected. With no mask bound (u_has_mask == false) every pixel is treated as
// mesh — a degenerate whole-scene contour, NOT the historical pre-mask EDL look.
// Unreachable in the app: EDL only runs once the off-screen mask chain is ready,
// so a mask is always bound there; the fallback exists only for callers that
// drive EdlPass without a mask.
constexpr std::string_view kEdlFragSrc = R"GLSL(
#version 410 core
in vec2 v_uv;
out float shade_out;
uniform sampler2D u_depth;
uniform sampler2D u_mask;
uniform bool u_has_mask;
uniform mat4 u_inv_proj;
uniform vec2 u_offsets[8];
uniform float u_strength = 1.0;
uniform float u_radius_px = 0.6;
uniform float u_max_gap = 0.02;

const float FAR_SENTINEL = 1.0e6;

bool isMesh(vec2 uv) {
  // Inclusive: the mask is a full 1.0 on mesh pixels, so only MSAA-resolved edge
  // fractions fall below it — counting any coverage hugs the full mesh edge.
  return !u_has_mask || texture(u_mask, uv).r > 0.01;
}

float meshDepth(vec2 uv) {
  if (!isMesh(uv)) {
    return FAR_SENTINEL;  // non-mesh: invisible to EDL, treated as infinitely far
  }
  float d = texture(u_depth, uv).r;
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 v = u_inv_proj * ndc;
  return log2(max(abs(v.z / v.w), 1e-6));
}

void main() {
  if (!isMesh(v_uv)) {
    shade_out = 1.0;  // only mesh pixels receive the eye-dome contour
    return;
  }
  float center = meshDepth(v_uv);
  vec2 texel = u_radius_px / vec2(textureSize(u_depth, 0));
  float response = 0.0;
  for (int i = 0; i < 8; ++i) {
    // Darken a mesh pixel whose neighbour is FARTHER (or non-mesh, read as far):
    // the mesh silhouette and the near side of creases get the contour. The clamp
    // bounds the huge silhouette gap so it reads as a graded outline.
    float nd = meshDepth(v_uv + u_offsets[i] * texel);
    response += min(max(0.0, nd - center), u_max_gap);
  }
  response /= 8.0;
  shade_out = exp(-response * 300.0 * u_strength);
}
)GLSL";

}  // namespace

void EdlPass::initializeGL() {
  // Latch the attempt, not the success: a per-context shader-compile failure
  // must not retry every frame (paintGL calls this each frame while EDL is on).
  // ready() still gates renderEdl + the composite's u_has_edl, so the feature
  // safely degrades. releaseGL() clears attempted_ so recreation rebuilds.
  if (attempted_) {
    return;
  }
  attempted_ = true;
  auto result = gl::Program::fromSources(kFullscreenVertSrc, kEdlFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_.emplace(std::move(*program));
    initialized_ = true;
  } else {
    fmt::print(stderr, "EdlPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
  }
}

void EdlPass::render(const ViewParams& /*view_params*/, const FrameContext& /*frame_ctx*/) {}

void EdlPass::resize(int width_px, int height_px) {
  if (width_px <= 0 || height_px <= 0) {
    releaseGL();
    return;
  }
  if (width_ == width_px && height_ == height_px && output_.id() != 0U) {
    return;
  }
  width_ = width_px;
  height_ = height_px;
  fbo_.bind();
  output_.allocate(GL_R16F, GL_RED, GL_HALF_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_.id(), 0);
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    functions.glDrawBuffers(1, &draw_buffer);
  });
  target_ready_ = fbo_.checkComplete();
}

void EdlPass::renderEdl(const ViewParams& view_params) {
  if (!ready() || depth_texture_id_ == 0U) {
    return;
  }
  fbo_.bind();
  // When there is no mask, bind texture 0 to unit 1 and set u_has_mask=false. The
  // shader's `!u_has_mask || texture(u_mask, ...)` short-circuits, so the texture-0
  // bind is never sampled — the unrestricted path is safe, not undefined.
  const bool has_mask = mask_texture_id_ != 0U;
  withGlFunctions([this, has_mask](auto& functions) {
    functions.glViewport(0, 0, width_, height_);
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, depth_texture_id_);
    functions.glActiveTexture(GL_TEXTURE1);
    functions.glBindTexture(GL_TEXTURE_2D, has_mask ? mask_texture_id_ : 0U);
    functions.glActiveTexture(GL_TEXTURE0);
  });
  program_->use();
  program_->setInt("u_depth", 0);
  program_->setInt("u_mask", 1);
  program_->setInt("u_has_mask", has_mask ? 1 : 0);
  program_->setMat4("u_inv_proj", glm::inverse(view_params.proj));
  program_->setFloat("u_strength", strength_);
  // Scale the neighbour radius by the supersample factor: the depth texture is
  // render_scale x larger under SSAA, so a fixed pixel radius would otherwise
  // shrink the EDL footprint (thinner/weaker outlines). Multiplying keeps the
  // device/world footprint — hence the look — invariant to the render scale.
  program_->setFloat("u_radius_px", radius_px_ * view_params.render_scale);
  program_->setFloat("u_max_gap", max_gap_);
  // 8 unit-circle neighbour directions (Potree's circular sampling pattern).
  // Constant for the program's lifetime, so compute them once (L.55).
  static const std::array<glm::vec2, 8> k_offsets = [] {
    std::array<glm::vec2, 8> values{};
    for (int i = 0; i < 8; ++i) {
      const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / 8.0f;
      values[i] = glm::vec2(std::cos(angle), std::sin(angle));
    }
    return values;
  }();
  program_->setVec2Array("u_offsets", k_offsets.data(), static_cast<int>(k_offsets.size()));
  fullscreen_vao_.bind();
  withGlFunctions([](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, 3); });
  fullscreen_vao_.unbind();
  withGlFunctions([](auto& functions) {
    functions.glUseProgram(0U);
    functions.glActiveTexture(GL_TEXTURE1);
    functions.glBindTexture(GL_TEXTURE_2D, 0U);
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, 0U);
  });
}

void EdlPass::releaseGL() {
  program_.reset();
  fullscreen_vao_ = gl::VertexArray{};
  fbo_ = gl::Framebuffer{};
  output_ = gl::Texture{};
  width_ = 0;
  height_ = 0;
  target_ready_ = false;
  initialized_ = false;
  attempted_ = false;
}

GLuint EdlPass::outputTextureId() const noexcept {
  return ready() ? output_.id() : 0U;
}

bool EdlPass::ready() const noexcept {
  return initialized_ && target_ready_ && output_.id() != 0U;
}

}  // namespace pj::scene3d
