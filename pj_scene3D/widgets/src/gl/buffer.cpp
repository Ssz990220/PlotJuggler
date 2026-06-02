// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/buffer.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <stdexcept>
#include <utility>

namespace pj::scene3d::gl {
namespace {

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

template <typename Callback>
void withGlFunctionsNoThrow(Callback&& callback) noexcept {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return;
  }

  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    callback(*functions);
    return;
  }

  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    return;
  }
  functions->initializeOpenGLFunctions();
  callback(*functions);
}

}  // namespace

Buffer::Buffer() = default;

Buffer::~Buffer() {
  if (id_ != 0U) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteBuffers(1, &id_); });
  }
}

Buffer::Buffer(Buffer&& other) noexcept : id_(std::exchange(other.id_, 0U)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteBuffers(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
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
