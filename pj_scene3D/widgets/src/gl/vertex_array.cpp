// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/vertex_array.h"

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

VertexArray::VertexArray() = default;

VertexArray::~VertexArray() {
  if (id_ != 0U) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteVertexArrays(1, &id_); });
  }
}

VertexArray::VertexArray(VertexArray&& other) noexcept : id_(std::exchange(other.id_, 0U)) {}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteVertexArrays(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
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
    }
    functions.glBindVertexArray(id_);
  });
}

void VertexArray::unbind() {
  withGlFunctions([](auto& functions) { functions.glBindVertexArray(0U); });
}

}  // namespace pj::scene3d::gl
