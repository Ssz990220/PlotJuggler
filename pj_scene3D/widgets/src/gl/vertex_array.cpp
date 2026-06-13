// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/vertex_array.h"

#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {

VertexArray::VertexArray() = default;

VertexArray::~VertexArray() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteVertexArrays(1, &id_); });
  }
}

VertexArray::VertexArray(VertexArray&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteVertexArrays(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
  }
  return *this;
}

GLuint VertexArray::id() const noexcept {
  return id_;
}

void VertexArray::bind() {
  withGlFunctions([this](auto& functions) {
    if (id_ == 0U) {
      functions.glGenVertexArrays(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindVertexArray(id_);
  });
}

void VertexArray::unbind() {
  withGlFunctions([](auto& functions) { functions.glBindVertexArray(0U); });
}

}  // namespace pj::scene3d::gl
