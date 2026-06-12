// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gizmos/arrow_gizmo.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace pj::scene3d {
namespace {

// View-space Lambertian by default. Light direction is constant in view space so
// the highlight stays in the same screen-relative position regardless of how the
// arrow rotates; marker annotations can request flat color with u_flat_color.
constexpr std::string_view kVertSrc = R"(#version 450 core
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

constexpr std::string_view kFragSrc = R"(#version 450 core
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
  frag_color = vec4(lit, u_color.a);
}
)";

template <typename Callback>
decltype(auto) withGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }
  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    return callback(*functions);
  }
  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
}

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
  vertex_data_.clear();
  index_data_.clear();

  const int segments = std::max(params_.segments, 3);
  const float length = std::max(params_.length, 0.0f);
  const float head_length = std::clamp(params_.head_length, 0.0f, length);
  const float shaft_length = length - head_length;
  const float shaft_radius = std::max(params_.shaft_radius, 0.0f);
  const float head_radius = std::max(params_.head_radius, 0.0f);
  const float two_pi = 2.0f * std::numbers::pi_v<float>;

  auto pushVertex = [&](float px, float py, float pz, float nx, float ny, float nz) {
    vertex_data_.insert(vertex_data_.end(), {px, py, pz, nx, ny, nz});
  };

  // ---- Cylinder shaft (lateral surface) ----
  // Two rings of `segments` vertices: x=0 (back) and x=shaft_length (front).
  // Per-vertex radial normals -> smooth shading around the cylinder.
  const uint32_t shaft_back = static_cast<uint32_t>(vertex_data_.size() / 6);
  for (int ring = 0; ring < 2; ++ring) {
    const float x = (ring == 0) ? 0.0f : shaft_length;
    for (int i = 0; i < segments; ++i) {
      const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
      const float c = std::cos(theta);
      const float s = std::sin(theta);
      pushVertex(x, shaft_radius * c, shaft_radius * s, 0.0f, c, s);
    }
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    const uint32_t a = shaft_back + i;
    const uint32_t b = shaft_back + next;
    const uint32_t c = shaft_back + segments + next;
    const uint32_t d = shaft_back + segments + i;
    index_data_.insert(index_data_.end(), {a, b, c, a, c, d});
  }

  // ---- Cone base disc (faces -X, sits between shaft and head) ----
  // Closes off any visible gap when head_radius > shaft_radius.
  const uint32_t disc_center = static_cast<uint32_t>(vertex_data_.size() / 6);
  pushVertex(shaft_length, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    pushVertex(shaft_length, head_radius * std::cos(theta), head_radius * std::sin(theta), -1.0f, 0.0f, 0.0f);
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data_.insert(
        index_data_.end(),
        {disc_center, disc_center + 1U + static_cast<uint32_t>(next), disc_center + 1U + static_cast<uint32_t>(i)});
  }

  // ---- Cone head (lateral surface) ----
  // Normal at a base-ring point: (head_radius, head_length*cos, head_length*sin) / sqrt(R^2 + H^2)
  // (derived from circumferential × axial cross product).
  const float norm_len = std::sqrt(head_radius * head_radius + head_length * head_length);
  const float nx_cone = (norm_len > 0.0f) ? head_radius / norm_len : 1.0f;
  const uint32_t cone_ring = static_cast<uint32_t>(vertex_data_.size() / 6);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    const float ny = (norm_len > 0.0f) ? head_length * c / norm_len : 0.0f;
    const float nz = (norm_len > 0.0f) ? head_length * s / norm_len : 0.0f;
    pushVertex(shaft_length, head_radius * c, head_radius * s, nx_cone, ny, nz);
  }
  const uint32_t cone_tip = static_cast<uint32_t>(vertex_data_.size() / 6);
  pushVertex(length, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data_.insert(
        index_data_.end(), {cone_ring + static_cast<uint32_t>(i), cone_ring + static_cast<uint32_t>(next), cone_tip});
  }

  // ---- Back cap on the shaft (faces -X) ----
  // Cosmetic — makes the arrow look closed if viewed end-on. Wound CCW
  // when viewed from -X (outside) so backface culling keeps it visible.
  const uint32_t back_center = static_cast<uint32_t>(vertex_data_.size() / 6);
  pushVertex(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    pushVertex(0.0f, shaft_radius * std::cos(theta), shaft_radius * std::sin(theta), -1.0f, 0.0f, 0.0f);
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data_.insert(
        index_data_.end(),
        {back_center, back_center + 1U + static_cast<uint32_t>(next), back_center + 1U + static_cast<uint32_t>(i)});
  }

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
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 6);
    f.glEnableVertexAttribArray(0U);
    f.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    f.glEnableVertexAttribArray(1U);
    f.glVertexAttribPointer(1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
  });
  vao_.unbind();
}

void ArrowGizmo::render(const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading) {
  if (!initialized_ || program_ == nullptr || index_data_.empty()) {
    return;
  }
  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setMat3("u_normal_mat", normal_mat);
  program_->setVec4("u_color", color);
  program_->setInt("u_flat_color", shading == Shading::kFlat ? 1 : 0);
  vao_.bind();
  withGlFunctions([this](auto& f) {
    f.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_data_.size()), GL_UNSIGNED_INT, nullptr);
  });
  vao_.unbind();
  withGlFunctions([](auto& f) { f.glUseProgram(0U); });
}

}  // namespace pj::scene3d
