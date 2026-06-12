// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/grid_render_pass.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

constexpr std::string_view kGridVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(in_pos, 1.0); }
)";

constexpr std::string_view kGridFragSrc = R"(#version 450 core
out vec4 frag_color;
uniform vec3 u_color;
void main() { frag_color = vec4(u_color, 1.0); }
)";

// Line grid: (divisions+1) lines per axis, 2 verts per line.
std::vector<glm::vec3> makeLineVertices(float extent_m, int divisions) {
  std::vector<glm::vec3> vertices;
  vertices.reserve(static_cast<std::size_t>(divisions + 1) * 4U);
  const float half = extent_m * 0.5f;
  const float cell = extent_m / static_cast<float>(divisions);
  for (int i = 0; i <= divisions; ++i) {
    const float offset = -half + static_cast<float>(i) * cell;
    vertices.emplace_back(-half, offset, 0.0f);
    vertices.emplace_back(half, offset, 0.0f);
    vertices.emplace_back(offset, -half, 0.0f);
    vertices.emplace_back(offset, half, 0.0f);
  }
  return vertices;
}

// Filled checkerboard: alternate cells as solid quads (2 triangles each); the
// remaining cells show the background through, giving the classic checker look.
std::vector<glm::vec3> makeCellVertices(float extent_m, int divisions) {
  std::vector<glm::vec3> vertices;
  const float half = extent_m * 0.5f;
  const float cell = extent_m / static_cast<float>(divisions);
  for (int i = 0; i < divisions; ++i) {
    for (int j = 0; j < divisions; ++j) {
      if (((i + j) & 1) != 0) {
        continue;
      }
      const float x0 = -half + static_cast<float>(i) * cell;
      const float y0 = -half + static_cast<float>(j) * cell;
      const float x1 = x0 + cell;
      const float y1 = y0 + cell;
      vertices.emplace_back(x0, y0, 0.0f);
      vertices.emplace_back(x1, y0, 0.0f);
      vertices.emplace_back(x1, y1, 0.0f);
      vertices.emplace_back(x0, y0, 0.0f);
      vertices.emplace_back(x1, y1, 0.0f);
      vertices.emplace_back(x0, y1, 0.0f);
    }
  }
  return vertices;
}

}  // namespace

void GridRenderPass::initializeGL() {
  initialized_ = false;
  auto result = gl::Program::fromSources(kGridVertSrc, kGridFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "GridRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }
  geometry_dirty_ = true;
  rebuildGeometry();
  initialized_ = true;
}

void GridRenderPass::rebuildGeometry() {
  if (!geometry_dirty_) {
    return;
  }
  const std::vector<glm::vec3> vertices =
      style_ == Style::kLines ? makeLineVertices(extent_m_, divisions_) : makeCellVertices(extent_m_, divisions_);
  vertex_count_ = static_cast<int>(vertices.size());
  vao_.bind();
  vbo_.uploadStatic(GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(sizeof(glm::vec3) * vertices.size()));
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(glm::vec3)), nullptr);
  });
  vao_.unbind();
  geometry_dirty_ = false;
}

void GridRenderPass::render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) {
  if (!initialized_ || program_ == nullptr) {
    return;
  }
  rebuildGeometry();  // lazy: extent/divisions/style changed since last frame
  if (vertex_count_ == 0) {
    return;
  }

  const glm::mat4 mvp = view_params.proj * view_params.view * glm::mat4{1.0f};

  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setVec3("u_color", color_);
  vao_.bind();
  const GLenum mode = style_ == Style::kLines ? GL_LINES : GL_TRIANGLES;
  const int count = vertex_count_;
  withGlFunctions([mode, count](auto& functions) { functions.glDrawArrays(mode, 0, count); });
  vao_.unbind();
  unuseProgram();
}

void GridRenderPass::releaseGL() {
  // Forget the old context's GL objects and re-arm initializeGL(), which
  // rebuilds the program + re-uploads the static grid VBO from scratch.
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  initialized_ = false;
}

void GridRenderPass::setColor(const glm::vec3& color) {
  color_ = color;
}

void GridRenderPass::setExtentMetres(float extent_m) {
  if (extent_m_ != extent_m) {
    extent_m_ = extent_m;
    geometry_dirty_ = true;
  }
}

void GridRenderPass::setDivisions(int divisions) {
  divisions = std::clamp(divisions, 1, 200);
  if (divisions_ != divisions) {
    divisions_ = divisions;
    geometry_dirty_ = true;
  }
}

void GridRenderPass::setStyle(Style style) {
  if (style_ != style) {
    style_ = style;
    geometry_dirty_ = true;
  }
}

}  // namespace pj::scene3d
