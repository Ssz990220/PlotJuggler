// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gizmos/arrow_gizmo.h"

#include <fmt/core.h>

#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <string_view>
#include <variant>

#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

// View-space Lambertian by default. Light direction is constant in view space so
// the highlight stays in the same screen-relative position regardless of how the
// arrow rotates; marker annotations can request flat color with u_flat_color.
constexpr std::string_view kVertSrc = R"(#version 410 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
uniform mat4 u_mvp;
uniform mat3 u_normal_mat;
out vec3 v_normal_view;
void main() {
  gl_Position = u_mvp * vec4(in_pos, 1.0);
  v_normal_view = normalize(u_normal_mat * in_normal);
}
)";

constexpr std::string_view kFragSrc = R"(#version 410 core
in vec3 v_normal_view;
uniform vec4 u_color;
uniform int u_flat_color;
out vec4 frag_color;
void main() {
  if (u_flat_color != 0) {
    frag_color = u_color;
    return;
  }
  const vec3 L = normalize(vec3(0.30, 0.55, 0.80));
  const float ambient = 0.35;
  float lambert = max(dot(normalize(v_normal_view), L), 0.0);
  vec3 lit = u_color.rgb * (ambient + (1.0 - ambient) * lambert);
  // Annotation draw (Phase 0B/Part C): u_color.a carries the gizmo OPACITY
  // and, through the annotation blend mode SceneViewWidget::renderScene sets —
  // blendFuncSeparate(SRC_ALPHA, 1-SRC_ALPHA, ZERO, 1-SRC_ALPHA) — also drives
  // the tonemap-bypass marker: dstA' = dstA*(1-a), so at alpha 1 the pixel is
  // fully annotation (marker 0, flat vivid color) and at lower opacities the
  // marker scales with coverage. The color is linearized so the composite's
  // bypass sRGB-encode restores it exactly.
  frag_color = vec4(pow(max(lit, vec3(0.0)), vec3(2.2)), u_color.a);
}
)";

bool paramsEqual(const ArrowGizmo::Params& a, const ArrowGizmo::Params& b) {
  return a.length == b.length && a.shaft_radius == b.shaft_radius && a.head_length == b.head_length &&
         a.head_radius == b.head_radius && a.segments == b.segments;
}

}  // namespace

void ArrowGizmo::initializeGL(const Params& params) {
  if (!initialized_) {
    auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
    if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
      program_ = std::make_unique<gl::Program>(std::move(*program));
    } else {
      fmt::print(stderr, "ArrowGizmo shader error: {}\n", std::get<std::string>(result));
      program_.reset();
      return;
    }
    initialized_ = true;
    mesh_dirty_ = true;
    params_ = params;
  } else {
    rebuild(params);
  }
  if (mesh_dirty_ && program_ != nullptr) {
    generateMesh();
    uploadMesh();
  }
}

// REQUIRES a current GL context when initialized_: generateMesh()/uploadMesh()
// bind the VAO and upload buffers, which throw with no context current and
// target a foreign context if the wrong one is current.
void ArrowGizmo::rebuild(const Params& params) {
  if (paramsEqual(params, params_)) {
    return;
  }
  params_ = params;
  mesh_dirty_ = true;
  if (initialized_) {
    generateMesh();
    uploadMesh();
  }
}

void ArrowGizmo::releaseGL() {
  // Drop the GL program + buffers from the dying context; keep the CPU-side
  // mesh and set mesh_dirty_ so initializeGL re-uploads it in the new context.
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  ebo_ = gl::Buffer{};
  initialized_ = false;
  mesh_dirty_ = true;
}

void ArrowGizmo::generateMesh() {
  // Delegate to the shared CPU tessellator (the single source of truth also used
  // by PosesRenderPass's instanced arrows), then keep the bytes for uploadMesh().
  ArrowMeshData mesh = buildArrowMesh(
      ArrowMeshParams{
          params_.length, params_.shaft_radius, params_.head_length, params_.head_radius, params_.segments});
  vertex_data_ = std::move(mesh.vertices);
  index_data_ = std::move(mesh.indices);
  mesh_dirty_ = false;
}

void ArrowGizmo::uploadMesh() {
  if (vertex_data_.empty() || index_data_.empty()) {
    return;
  }
  vao_.bind();
  vbo_.uploadStatic(GL_ARRAY_BUFFER, vertex_data_.data(), static_cast<GLsizeiptr>(sizeof(float) * vertex_data_.size()));
  ebo_.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, index_data_.data(), static_cast<GLsizeiptr>(sizeof(uint32_t) * index_data_.size()));
  withGlFunctions([](auto& f) {
    constexpr GLsizei kStride = static_cast<GLsizei>(sizeof(float) * 6);
    f.glEnableVertexAttribArray(0U);
    f.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
    f.glEnableVertexAttribArray(1U);
    f.glVertexAttribPointer(1U, 3, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<const void*>(sizeof(float) * 3));
  });
  vao_.unbind();
}

void ArrowGizmo::render(const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading) {
  bindForRender();
  drawBound(mvp, normal_mat, color, shading);
  unbindAfterRender();
}

void ArrowGizmo::bindForRender() {
  if (!initialized_ || program_ == nullptr || index_data_.empty()) {
    return;
  }
  program_->use();
}

void ArrowGizmo::drawBound(const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading) {
  if (!initialized_ || program_ == nullptr || index_data_.empty()) {
    return;
  }
  program_->setMat4("u_mvp", mvp);
  program_->setMat3("u_normal_mat", normal_mat);
  program_->setVec4("u_color", color);
  program_->setInt("u_flat_color", shading == Shading::kFlat ? 1 : 0);
  vao_.bind();
  withGlFunctions([this](auto& f) {
    f.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_data_.size()), GL_UNSIGNED_INT, nullptr);
  });
  vao_.unbind();
}

void ArrowGizmo::unbindAfterRender() {
  if (!initialized_ || program_ == nullptr || index_data_.empty()) {
    return;
  }
  withGlFunctions([](auto& f) { f.glUseProgram(0U); });
}

void renderTriadBound(
    ArrowGizmo& arrow, const glm::mat4& proj, const glm::mat4& view, const glm::mat4& base, const glm::mat4& arm_scale,
    const std::array<glm::vec4, 3>& colors, ArrowGizmo::Shading shading) {
  // The gizmo points along +X. Rotate +X -> +Y (+90° about +Z) and +X -> +Z
  // (-90° about +Y). Single source of truth for the three triad call sites.
  static const glm::mat4 k_y_rotate = glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), {0.0f, 0.0f, 1.0f});
  static const glm::mat4 k_z_rotate = glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), {0.0f, 1.0f, 0.0f});

  const glm::mat4 model_x = base * arm_scale;
  const glm::mat4 model_y = base * k_y_rotate * arm_scale;
  const glm::mat4 model_z = base * k_z_rotate * arm_scale;

  arrow.drawBound(proj * view * model_x, glm::mat3(view * model_x), colors[0], shading);
  arrow.drawBound(proj * view * model_y, glm::mat3(view * model_y), colors[1], shading);
  arrow.drawBound(proj * view * model_z, glm::mat3(view * model_z), colors[2], shading);
}

}  // namespace pj::scene3d
