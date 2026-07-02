// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/poses_render_pass.h"

#include <fmt/core.h>

#include <cstddef>
#include <string_view>
#include <variant>

#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/render_pass.h"  // ViewParams

namespace pj::scene3d {
namespace {

// Per-vertex: pos (loc 0), normal (loc 1). Per-instance: frame-local model
// (loc 2-5) + rgba color (loc 6). The pose's frame->world TF is a uniform, so
// camera/TF motion never re-touches the instance buffer. View-space Lambertian
// matches ArrowGizmo so pose triads read identically to the TF "Frames" gizmos.
constexpr std::string_view kVertSrc = R"(#version 410 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in mat4 in_model;   // consumes locations 2,3,4,5
layout(location = 6) in vec4 in_color;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform mat4 u_frame_world;
out vec3 v_normal_view;
out vec4 v_color;
void main() {
  mat4 model_view = u_view * u_frame_world * in_model;
  gl_Position = u_proj * model_view * vec4(in_pos, 1.0);
  v_normal_view = mat3(model_view) * in_normal;  // uniform scale -> normalized in frag
  v_color = in_color;
}
)";

// Identical look to ArrowGizmo's lit path: ambient + Lambert, linearized so the
// HDR composite's sRGB-encode restores it, and the alpha carries the gizmo
// opacity through SceneViewWidget's annotation blend (tonemap-bypass coverage).
constexpr std::string_view kFragSrc = R"(#version 410 core
in vec3 v_normal_view;
in vec4 v_color;
out vec4 frag_color;
void main() {
  const vec3 L = normalize(vec3(0.30, 0.55, 0.80));
  const float ambient = 0.35;
  float lambert = max(dot(normalize(v_normal_view), L), 0.0);
  vec3 lit = v_color.rgb * (ambient + (1.0 - ambient) * lambert);
  frag_color = vec4(pow(max(lit, vec3(0.0)), vec3(2.2)), v_color.a);
}
)";

// Unit arrow proportions, matching AxisRenderPass::paramsForLength(1.0): each arm
// is this mesh uniformly scaled by the pose triad's size.
constexpr ArrowMeshParams kUnitArrow{
    .length = 1.0f, .shaft_radius = 0.04f, .head_length = 0.30f, .head_radius = 0.10f, .segments = 16};

}  // namespace

void PosesRenderPass::initializeGL() {
  if (initialized_) {
    return;
  }
  auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PosesRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }

  const ArrowMeshData mesh = buildArrowMesh(kUnitArrow);
  index_count_ = static_cast<int>(mesh.indices.size());

  vao_.bind();
  vbo_.uploadStatic(
      GL_ARRAY_BUFFER, mesh.vertices.data(), static_cast<GLsizeiptr>(sizeof(float) * mesh.vertices.size()));
  vbo_.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& f) {
    constexpr GLsizei kStride = static_cast<GLsizei>(sizeof(float) * 6);
    f.glEnableVertexAttribArray(0U);
    f.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
    f.glEnableVertexAttribArray(1U);
    f.glVertexAttribPointer(1U, 3, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<const void*>(sizeof(float) * 3));
  });

  // Per-instance attribs sourced from instance_vbo_ (still empty — the VAO only
  // records the buffer+layout binding here; data lands on the first render).
  instance_vbo_.bind(GL_ARRAY_BUFFER);
  withGlFunctions([](auto& f) {
    constexpr GLsizei kIstride = static_cast<GLsizei>(sizeof(PoseTriadInstance));
    for (GLuint col = 0U; col < 4U; ++col) {
      const GLuint loc = 2U + col;
      f.glEnableVertexAttribArray(loc);
      f.glVertexAttribPointer(
          loc, 4, GL_FLOAT, GL_FALSE, kIstride, reinterpret_cast<const void*>(sizeof(glm::vec4) * col));
      f.glVertexAttribDivisor(loc, 1U);
    }
    f.glEnableVertexAttribArray(6U);
    f.glVertexAttribPointer(
        6U, 4, GL_FLOAT, GL_FALSE, kIstride, reinterpret_cast<const void*>(offsetof(PoseTriadInstance, color)));
    f.glVertexAttribDivisor(6U, 1U);
  });

  ebo_.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, mesh.indices.data(),
      static_cast<GLsizeiptr>(sizeof(std::uint32_t) * mesh.indices.size()));
  ebo_.bind(GL_ELEMENT_ARRAY_BUFFER);  // record the EBO into the VAO
  vao_.unbind();

  initialized_ = true;
  instances_dirty_ = true;  // re-upload instances into the (re)built buffer
}

void PosesRenderPass::releaseGL() {
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  ebo_ = gl::Buffer{};
  instance_vbo_ = gl::Buffer{};
  initialized_ = false;
  instances_dirty_ = true;
}

void PosesRenderPass::setInstances(std::vector<PoseTriadInstance> instances) {
  instances_ = std::move(instances);
  instances_dirty_ = true;
}

void PosesRenderPass::render(const ViewParams& view_params, const glm::mat4& frame_world) {
  if (!initialized_ || program_ == nullptr || instances_.empty() || index_count_ == 0) {
    return;
  }
  if (instances_dirty_) {
    instance_vbo_.uploadStatic(
        GL_ARRAY_BUFFER, instances_.data(), static_cast<GLsizeiptr>(instances_.size() * sizeof(PoseTriadInstance)));
    instances_dirty_ = false;
  }

  program_->use();
  program_->setMat4("u_view", view_params.view);
  program_->setMat4("u_proj", view_params.proj);
  program_->setMat4("u_frame_world", frame_world);

  vao_.bind();
  withGlFunctions([this](auto& f) {
    f.glEnable(GL_DEPTH_TEST);
    // Annotation blend (like the TF axis triads): RGB blends normally while the
    // (ZERO, 1-SRC_ALPHA) alpha factors scale the tonemap-bypass marker by the
    // gizmo's coverage. Restored to the layer loop's data-blend ambient after.
    f.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    f.glDrawElementsInstanced(
        GL_TRIANGLES, index_count_, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(instances_.size()));
    f.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  });
  vao_.unbind();
  withGlFunctions([](auto& f) { f.glUseProgram(0U); });
}

}  // namespace pj::scene3d
