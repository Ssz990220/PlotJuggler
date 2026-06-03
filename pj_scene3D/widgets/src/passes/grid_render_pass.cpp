// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/grid_render_pass.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

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

std::array<glm::vec3, 44> makeGridVertices(float extent_m) {
  std::array<glm::vec3, 44> vertices{};
  const float scale = extent_m / 10.0f;
  std::size_t index = 0U;

  for (int i = 0; i <= 10; ++i) {
    const float offset = (static_cast<float>(i) - 5.0f) * scale;
    vertices[index] = glm::vec3{-5.0f * scale, offset, 0.0f};
    ++index;
    vertices[index] = glm::vec3{5.0f * scale, offset, 0.0f};
    ++index;
  }

  for (int i = 0; i <= 10; ++i) {
    const float offset = (static_cast<float>(i) - 5.0f) * scale;
    vertices[index] = glm::vec3{offset, -5.0f * scale, 0.0f};
    ++index;
    vertices[index] = glm::vec3{offset, 5.0f * scale, 0.0f};
    ++index;
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

  const std::array<glm::vec3, 44> vertices = makeGridVertices(extent_m_);
  vao_.bind();
  vbo_.uploadStatic(GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(sizeof(glm::vec3) * vertices.size()));
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(glm::vec3)), nullptr);
  });
  vao_.unbind();

  initialized_ = true;
}

void GridRenderPass::render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) {
  if (!initialized_ || program_ == nullptr) {
    return;
  }

  const glm::mat4 mvp = view_params.proj * view_params.view * glm::mat4{1.0f};

  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setVec3("u_color", color_);
  vao_.bind();
  withGlFunctions([](auto& functions) { functions.glDrawArrays(GL_LINES, 0, 44); });
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
  extent_m_ = extent_m;
}

}  // namespace pj::scene3d
