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
#version 450 core
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
constexpr std::string_view kEdlFragSrc = R"GLSL(
#version 450 core
in vec2 v_uv;
out float shade_out;
uniform sampler2D u_depth;
uniform mat4 u_inv_proj;
uniform vec2 u_offsets[8];
uniform float u_strength = 1.0;
uniform float u_radius_px = 0.6;

float logEyeDepth(vec2 uv) {
  float d = texture(u_depth, uv).r;
  if (d >= 0.9999) {
    // Background sentinel: "infinitely far", so max(0, center - neighbour)
    // is always 0 — background neighbours never darken an object (and the
    // result is independent of the scene's absolute scale).
    return 1.0e6;
  }
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 v = u_inv_proj * ndc;
  return log2(max(abs(v.z / v.w), 1e-6));
}

void main() {
  float d = texture(u_depth, v_uv).r;
  if (d >= 0.9999) {
    shade_out = 1.0;  // background itself: untouched
    return;
  }
  float center = logEyeDepth(v_uv);
  vec2 texel = u_radius_px / vec2(textureSize(u_depth, 0));
  float response = 0.0;
  for (int i = 0; i < 8; ++i) {
    // A pixel darkens when a neighbour is CLOSER — depth creases and the far
    // side of silhouettes get the contour shade (Potree's response term).
    response += max(0.0, center - logEyeDepth(v_uv + u_offsets[i] * texel));
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
  withGlFunctions([this](auto& functions) {
    functions.glViewport(0, 0, width_, height_);
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, depth_texture_id_);
  });
  program_->use();
  program_->setInt("u_depth", 0);
  program_->setMat4("u_inv_proj", glm::inverse(view_params.proj));
  program_->setFloat("u_strength", strength_);
  // Scale the neighbour radius by the supersample factor: the depth texture is
  // render_scale x larger under SSAA, so a fixed pixel radius would otherwise
  // shrink the EDL footprint (thinner/weaker outlines). Multiplying keeps the
  // device/world footprint — hence the look — invariant to the render scale.
  program_->setFloat("u_radius_px", radius_px_ * view_params.render_scale);
  // 8 unit-circle neighbour directions (Potree's circular sampling pattern).
  // Constant for the program's lifetime, so compute them once (L.55).
  static const std::array<glm::vec2, 8> kOffsets = [] {
    std::array<glm::vec2, 8> values{};
    for (int i = 0; i < 8; ++i) {
      const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / 8.0f;
      values[i] = glm::vec2(std::cos(angle), std::sin(angle));
    }
    return values;
  }();
  program_->setVec2Array("u_offsets", kOffsets.data(), static_cast<int>(kOffsets.size()));
  fullscreen_vao_.bind();
  withGlFunctions([](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, 3); });
  fullscreen_vao_.unbind();
  withGlFunctions([](auto& functions) {
    functions.glUseProgram(0U);
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
