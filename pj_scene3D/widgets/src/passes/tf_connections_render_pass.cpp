// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/tf_connections_render_pass.h"

#include <fmt/core.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

// Position-only line shader: world-space endpoints transformed by the MVP, a
// single flat fragment color. (The grid pass synthesizes its lines in the
// fragment stage; these are honest GL_LINES primitives.)
constexpr std::string_view kVertSrc = R"(#version 410 core
layout(location = 0) in vec3 in_pos;
uniform mat4 u_mvp;
void main() {
  gl_Position = u_mvp * vec4(in_pos, 1.0);
}
)";

constexpr std::string_view kFragSrc = R"(#version 410 core
uniform vec3 u_color;
out vec4 frag_color;
void main() {
  frag_color = vec4(u_color, 1.0);
}
)";

}  // namespace

void TfConnectionsRenderPass::initializeGL() {
  initialized_ = false;
  auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "TfConnectionsRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }
  initialized_ = true;
}

void TfConnectionsRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!initialized_ || program_ == nullptr) {
    return;
  }

  // Recompute the parent-connection segments for the current TF/time every paint
  // (cheap: a handful of lookups per frame).
  buildTfConnectionSegments(
      frame_ctx.tf, frame_ctx.fixed_frame, frame_ctx.time, segments_scratch_, frame_ctx.render_origin);
  if (segments_scratch_.empty()) {
    return;  // no TF, or every frame is a root / unresolved
  }

  // A TfConnectionSegment is two tightly-packed vec3s — bit-identical to two
  // consecutive GL_LINES vertices — so the segment array IS the vertex stream and
  // uploads directly with no flatten/copy step. The static_assert pins that
  // coupling: a stride/padding change here would silently corrupt the geometry.
  static_assert(
      sizeof(TfConnectionSegment) == 2 * sizeof(glm::vec3), "segment layout must be two packed vec3 vertices");
  const int vertex_count = static_cast<int>(segments_scratch_.size()) * 2;

  // Upload + (re)declare the vec3 position layout each paint: the data changes
  // every frame, and re-specifying the attribute pointer alongside the upload
  // keeps the VAO consistent (same pattern as GridRenderPass::uploadVertices).
  vao_.bind();
  vbo_.uploadStatic(
      GL_ARRAY_BUFFER, segments_scratch_.data(),
      static_cast<GLsizeiptr>(sizeof(TfConnectionSegment) * segments_scratch_.size()));
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(glm::vec3)), nullptr);
  });
  vao_.unbind();

  const glm::mat4 mvp = view_params.proj * view_params.view;  // model is identity (world-space endpoints)
  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setVec3("u_color", color_);

  vao_.bind();
  withGlFunctions([vertex_count](auto& functions) { functions.glDrawArrays(GL_LINES, 0, vertex_count); });
  vao_.unbind();
  unuseProgram();
}

void TfConnectionsRenderPass::releaseGL() {
  // Drop the dead context's GL objects; initializeGL rebuilds the program and the
  // VBO re-uploads lazily on the next render.
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  initialized_ = false;
}

}  // namespace pj::scene3d
