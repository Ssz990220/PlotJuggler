// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/buffer.h"

#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {

Buffer::Buffer() = default;

Buffer::~Buffer() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteBuffers(1, &id_); });
  }
}

Buffer::Buffer(Buffer&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteBuffers(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
  }
  return *this;
}

GLuint Buffer::id() const noexcept {
  return id_;
}

void Buffer::bind(GLenum target) {
  withGlFunctions([this, target](auto& functions) {
    if (id_ == 0U) {
      functions.glGenBuffers(1, &id_);
      owning_context_ = QOpenGLContext::currentContext();
    }
    functions.glBindBuffer(target, id_);
  });
}

void Buffer::uploadStatic(GLenum target, const void* data, GLsizeiptr bytes) {
  bind(target);
  withGlFunctions(
      [target, data, bytes](auto& functions) { functions.glBufferData(target, bytes, data, GL_STATIC_DRAW); });
}

}  // namespace pj::scene3d::gl
