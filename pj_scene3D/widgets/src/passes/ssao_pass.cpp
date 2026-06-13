// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/ssao_pass.h"

#include <fmt/core.h>

#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

constexpr int kKernelSize = 32;  // perf default per plan §A.5 (64 = quality)

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

// SSAO adapted from LearnOpenGL (Joey de Vries, CC BY 4.0); depth->normal
// reconstruction adapted from Ben Golus (MIT). See pj_scene3D/THIRDPARTY.md.
// Deviations from plan §A.5, both deliberate: (1) view position reconstructs
// through u_inv_proj instead of near/far + u_proj[0][0] — the cameras include
// an ORTHOGRAPHIC model (TopDownOrtho) the perspective-only linEye formula
// breaks on, and they expose no near/far accessors; inverse(proj) handles both
// projection kinds. (2) The 4x4 noise texture is a 4x4-tiled in-shader hash —
// identical tiling semantics for the 4x4 blur with no texture-upload API.
constexpr std::string_view kSsaoFragSrc = R"GLSL(
#version 450 core
in vec2 v_uv;
out float ssao_out;
uniform sampler2D u_depth;
uniform mat4 u_proj;
uniform mat4 u_inv_proj;
uniform vec3 u_kernel[32];
uniform float u_radius = 0.5;
uniform float u_bias = 0.025;
uniform float u_ao_power = 1.0;

vec3 viewPos(vec2 uv) {
  float d = texture(u_depth, uv).r;
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 v = u_inv_proj * ndc;
  return v.xyz / v.w;
}

vec3 normalFromDepth(vec2 uv) {
  vec2 t = 1.0 / vec2(textureSize(u_depth, 0));
  vec3 c = viewPos(uv);
  vec3 l = viewPos(uv - vec2(t.x, 0.0)), r = viewPos(uv + vec2(t.x, 0.0));
  vec3 d2 = viewPos(uv - vec2(0.0, t.y)), u2 = viewPos(uv + vec2(0.0, t.y));
  vec3 hd = abs(l.z - c.z) < abs(r.z - c.z) ? (c - l) : (r - c);
  vec3 vd = abs(d2.z - c.z) < abs(u2.z - c.z) ? (c - d2) : (u2 - c);
  return normalize(cross(hd, vd));
}

void main() {
  float d = texture(u_depth, v_uv).r;
  if (d >= 0.9999) {  // far plane: background, no occlusion
    ssao_out = 1.0;
    return;
  }
  vec3 P = viewPos(v_uv);
  vec3 N = normalFromDepth(v_uv);
  // 4x4-tiled random rotation (replaces the noise texture; see header note).
  vec2 tile = vec2(ivec2(gl_FragCoord.xy) & 3);
  float angle = fract(sin(dot(tile, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
  vec3 rv = vec3(cos(angle), sin(angle), 0.0);
  vec3 T = normalize(rv - N * dot(rv, N));
  vec3 B = cross(N, T);
  mat3 TBN = mat3(T, B, N);
  float occ = 0.0;
  for (int i = 0; i < 32; ++i) {
    vec3 sp = P + (TBN * u_kernel[i]) * u_radius;
    vec4 o = u_proj * vec4(sp, 1.0);
    o.xyz /= o.w;
    o.xyz = o.xyz * 0.5 + 0.5;
    if (o.x < 0.0 || o.x > 1.0 || o.y < 0.0 || o.y > 1.0) {
      continue;
    }
    float sz = viewPos(o.xy).z;
    float rc = smoothstep(0.0, 1.0, u_radius / max(abs(P.z - sz), 1e-4));
    occ += (sz >= sp.z + u_bias ? 1.0 : 0.0) * rc;
  }
  ssao_out = pow(clamp(1.0 - occ / 32.0, 0.0, 1.0), u_ao_power);
}
)GLSL";

// 4x4 box blur over the AO tile (LearnOpenGL §SSAO blur).
constexpr std::string_view kBlurFragSrc = R"GLSL(
#version 450 core
in vec2 v_uv;
out float blur_out;
uniform sampler2D u_ao;
void main() {
  vec2 texel = 1.0 / vec2(textureSize(u_ao, 0));
  float sum = 0.0;
  for (int x = -2; x < 2; ++x) {
    for (int y = -2; y < 2; ++y) {
      sum += texture(u_ao, v_uv + vec2(float(x), float(y)) * texel).r;
    }
  }
  blur_out = sum / 16.0;
}
)GLSL";

std::optional<gl::Program> compileOrWarn(std::string_view vert, std::string_view frag, const char* what) {
  auto result = gl::Program::fromSources(vert, frag);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    return std::optional<gl::Program>(std::move(*program));
  }
  fmt::print(stderr, "SsaoPass {} shader error: {}\n", what, std::get<std::string>(result));
  return std::nullopt;
}

}  // namespace

void SsaoPass::initializeGL() {
  // Latch the attempt, not the success: a per-context shader-compile failure
  // must not retry every frame (paintGL calls this each frame while SSAO is on).
  // ready() still gates renderAo + the composite's u_has_ao, so the feature
  // safely degrades. releaseGL() clears attempted_ so recreation rebuilds.
  if (attempted_) {
    return;
  }
  attempted_ = true;
  initialized_ = buildPrograms();
  if (kernel_.empty()) {
    // Hemisphere kernel, denser near the origin (LearnOpenGL lerp-scale).
    // Deterministic seed: the kernel is a fixed quality knob, not entropy.
    std::mt19937 rng(0x55A0u);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    kernel_.reserve(kKernelSize);
    for (int i = 0; i < kKernelSize; ++i) {
      glm::vec3 sample(uni(rng) * 2.0f - 1.0f, uni(rng) * 2.0f - 1.0f, uni(rng));
      sample = glm::normalize(sample) * uni(rng);
      const float t = static_cast<float>(i) / static_cast<float>(kKernelSize);
      sample *= 0.1f + 0.9f * t * t;
      kernel_.push_back(sample);
    }
  }
}

bool SsaoPass::buildPrograms() {
  ssao_program_ = compileOrWarn(kFullscreenVertSrc, kSsaoFragSrc, "ssao");
  blur_program_ = compileOrWarn(kFullscreenVertSrc, kBlurFragSrc, "blur");
  return ssao_program_.has_value() && blur_program_.has_value();
}

void SsaoPass::render(const ViewParams& /*view_params*/, const FrameContext& /*frame_ctx*/) {}

void SsaoPass::resize(int width_px, int height_px) {
  if (width_px <= 0 || height_px <= 0) {
    releaseGL();
    return;
  }
  if (width_ == width_px && height_ == height_px && raw_ao_.id() != 0U && blur_ao_.id() != 0U) {
    return;
  }
  width_ = width_px;
  height_ = height_px;

  raw_fbo_.bind();
  raw_ao_.allocate(GL_R16F, GL_RED, GL_HALF_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, raw_ao_.id(), 0);
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    functions.glDrawBuffers(1, &draw_buffer);
  });
  const bool raw_complete = raw_fbo_.checkComplete();

  blur_fbo_.bind();
  blur_ao_.allocate(GL_R16F, GL_RED, GL_HALF_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur_ao_.id(), 0);
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    functions.glDrawBuffers(1, &draw_buffer);
  });
  targets_ready_ = raw_complete && blur_fbo_.checkComplete();
}

void SsaoPass::renderAo(const ViewParams& view_params) {
  if (!ready() || depth_texture_id_ == 0U) {
    return;
  }

  raw_fbo_.bind();
  withGlFunctions([this](auto& functions) {
    functions.glViewport(0, 0, width_, height_);
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, depth_texture_id_);
  });
  ssao_program_->use();
  ssao_program_->setInt("u_depth", 0);
  ssao_program_->setMat4("u_proj", view_params.proj);
  ssao_program_->setMat4("u_inv_proj", glm::inverse(view_params.proj));
  ssao_program_->setFloat("u_radius", radius_m_);
  ssao_program_->setFloat("u_ao_power", ao_power_);
  ssao_program_->setVec3Array("u_kernel", kernel_.data(), static_cast<int>(kernel_.size()));
  fullscreen_vao_.bind();
  withGlFunctions([](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, 3); });

  blur_fbo_.bind();
  withGlFunctions([this](auto& functions) {
    functions.glActiveTexture(GL_TEXTURE0);
    functions.glBindTexture(GL_TEXTURE_2D, raw_ao_.id());
  });
  blur_program_->use();
  blur_program_->setInt("u_ao", 0);
  withGlFunctions([](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, 3); });
  fullscreen_vao_.unbind();
  withGlFunctions([](auto& functions) {
    functions.glUseProgram(0U);
    functions.glBindTexture(GL_TEXTURE_2D, 0U);
  });
}

void SsaoPass::releaseGL() {
  ssao_program_.reset();
  blur_program_.reset();
  fullscreen_vao_ = gl::VertexArray{};
  raw_fbo_ = gl::Framebuffer{};
  blur_fbo_ = gl::Framebuffer{};
  raw_ao_ = gl::Texture{};
  blur_ao_ = gl::Texture{};
  width_ = 0;
  height_ = 0;
  targets_ready_ = false;
  initialized_ = false;
  attempted_ = false;
}

GLuint SsaoPass::outputTextureId() const noexcept {
  return ready() ? blur_ao_.id() : 0U;
}

bool SsaoPass::ready() const noexcept {
  return initialized_ && targets_ready_ && raw_ao_.id() != 0U && blur_ao_.id() != 0U;
}

}  // namespace pj::scene3d
