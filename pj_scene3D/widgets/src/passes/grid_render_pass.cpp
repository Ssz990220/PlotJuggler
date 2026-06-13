// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/grid_render_pass.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"

namespace pj::scene3d {
namespace {

// One program serves both styles. The vertex stage forwards the checkerboard
// parity flag and the world-space XY (model is identity, so in_pos IS the world
// position) so the fragment stage can draw the grid lines *procedurally*.
constexpr std::string_view kGridVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in float in_parity;
uniform mat4 u_mvp;
flat out float v_parity;
out vec2 v_world_xy;
void main() {
  v_parity = in_parity;
  v_world_xy = in_pos.xy;
  gl_Position = u_mvp * vec4(in_pos, 1.0);
}
)";

// u_mode 0 = plain line grid (GL_LINES geometry, flat line color).
// u_mode 1 = checkerboard fill with the grid lines baked in: the cell tone comes
// from the parity flag; the lines are drawn analytically from the distance to the
// nearest cell edge, anti-aliased with screen-space derivatives (fwidth). Drawing
// the lines in the SAME surface as the fill — instead of as a second coplanar
// pass — is what eliminates the grazing-angle z-fighting (no separate primitive
// to win/lose the depth test in patches), and the fwidth term keeps them ~1px
// wide in screen space so they fade smoothly into the distance instead of
// breaking into dashes.
constexpr std::string_view kGridFragSrc = R"(#version 450 core
flat in float v_parity;
in vec2 v_world_xy;
uniform int u_mode;
uniform vec3 u_color;       // line color
uniform vec3 u_color_a;     // checkerboard tone A
uniform vec3 u_color_b;     // checkerboard tone B
uniform float u_cell_m;     // cell size in metres
out vec4 frag_color;
void main() {
  if (u_mode == 0) {
    frag_color = vec4(u_color, 1.0);
    return;
  }
  vec3 tone = mix(u_color_a, u_color_b, v_parity);
  vec2 coord = v_world_xy / u_cell_m;
  vec2 grid = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
  float line = 1.0 - min(min(grid.x, grid.y), 1.0);
  frag_color = vec4(mix(tone, u_color, line), 1.0);
}
)";

// Upload `verts` into (vao, vbo) and bind the (vec3 pos, float parity) layout.
// Requires a current GL context. Empty `verts` is fine (count 0, nothing drawn).
void uploadVertices(gl::VertexArray& vao, gl::Buffer& vbo, const std::vector<GridVertex>& verts) {
  vao.bind();
  vbo.uploadStatic(GL_ARRAY_BUFFER, verts.data(), static_cast<GLsizeiptr>(sizeof(GridVertex) * verts.size()));
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GridVertex)), nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GridVertex)),
        reinterpret_cast<const void*>(offsetof(GridVertex, parity)));
  });
  vao.unbind();
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
  // Each style needs exactly one buffer: kLines draws line primitives; kFilledCells
  // draws the cell surface and synthesizes its lines in the fragment shader.
  if (style_ == Style::kLines) {
    const std::vector<GridVertex> lines = buildGridLines(extent_m_, divisions_);
    line_vertex_count_ = static_cast<int>(lines.size());
    uploadVertices(line_vao_, line_vbo_, lines);
    cell_vertex_count_ = 0;
  } else {
    const std::vector<GridVertex> cells = buildCheckerboardCells(extent_m_, divisions_);
    cell_vertex_count_ = static_cast<int>(cells.size());
    uploadVertices(cell_vao_, cell_vbo_, cells);
    line_vertex_count_ = 0;
  }
  geometry_dirty_ = false;
}

void GridRenderPass::render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) {
  if (!initialized_ || program_ == nullptr) {
    return;
  }
  rebuildGeometry();  // lazy: extent/divisions/style changed since last frame

  const glm::mat4 mvp = view_params.proj * view_params.view * glm::mat4{1.0f};
  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setVec3("u_color", color_);

  if (style_ == Style::kFilledCells && cell_vertex_count_ > 0) {
    program_->setInt("u_mode", 1);
    program_->setVec3("u_color_a", cell_color_a_);
    program_->setVec3("u_color_b", cell_color_b_);
    program_->setFloat("u_cell_m", extent_m_ / static_cast<float>(divisions_));
    cell_vao_.bind();
    const int cell_count = cell_vertex_count_;
    withGlFunctions([cell_count](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, cell_count); });
    cell_vao_.unbind();
  } else if (style_ == Style::kLines && line_vertex_count_ > 0) {
    program_->setInt("u_mode", 0);
    line_vao_.bind();
    const int line_count = line_vertex_count_;
    withGlFunctions([line_count](auto& functions) { functions.glDrawArrays(GL_LINES, 0, line_count); });
    line_vao_.unbind();
  }
  unuseProgram();
}

void GridRenderPass::releaseGL() {
  // Forget the old context's GL objects and re-arm initializeGL(), which
  // rebuilds the program + re-uploads the static grid VBOs from scratch.
  program_.reset();
  line_vao_ = gl::VertexArray{};
  line_vbo_ = gl::Buffer{};
  cell_vao_ = gl::VertexArray{};
  cell_vbo_ = gl::Buffer{};
  initialized_ = false;
}

void GridRenderPass::setColor(const glm::vec3& color) {
  color_ = color;
}

void GridRenderPass::setCellColors(const glm::vec3& tone_a, const glm::vec3& tone_b) {
  cell_color_a_ = tone_a;
  cell_color_b_ = tone_b;
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
