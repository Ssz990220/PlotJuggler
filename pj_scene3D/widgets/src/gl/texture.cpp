// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/gl/texture.h"

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

Texture::Texture() = default;

Texture::~Texture() {
  if (id_ != 0U) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
}

Texture::Texture(Texture&& other) noexcept : id_(std::exchange(other.id_, 0U)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
  }
  return *this;
}

GLuint Texture::id() const noexcept {
  return id_;
}

void Texture::upload(uint32_t width, uint32_t height, const uint8_t* data) {
  withGlFunctions([this, width, height, data](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
    }
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // R8 rows are not 4-byte aligned
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RED, GL_UNSIGNED_BYTE,
        data);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  });
}

void Texture::uploadSub(int32_t x, int32_t y, uint32_t width, uint32_t height, const uint8_t* data) {
  if (id_ == 0U) {
    return;  // not allocated yet — caller must upload() first
  }
  withGlFunctions([this, x, y, width, height, data](auto& functions) {
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions.glTexSubImage2D(
        GL_TEXTURE_2D, 0, x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RED, GL_UNSIGNED_BYTE,
        data);
  });
}

void Texture::allocate(GLenum internal_format, GLenum format, GLenum type, int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }

  withGlFunctions([this, internal_format, format, type, width, height](auto& functions) {
    if (id_ == 0U) {
      functions.glGenTextures(1, &id_);
    }
    functions.glBindTexture(GL_TEXTURE_2D, id_);
    functions.glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(internal_format), static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), 0, format, type, nullptr);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  });
}

void Texture::bind(int unit) {
  withGlFunctions([this, unit](auto& functions) {
    functions.glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    functions.glBindTexture(GL_TEXTURE_2D, id_);
  });
}

Texture2D::Texture2D() = default;

Texture2D::~Texture2D() {
  if (id_ != 0U) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
}

Texture2D::Texture2D(Texture2D&& other) noexcept : id_(std::exchange(other.id_, 0U)) {}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
  }
  return *this;
}

GLuint Texture2D::id() const noexcept {
  return id_;
}

void Texture2D::adopt(GLuint id) noexcept {
  if (id_ != 0U && id_ != id) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteTextures(1, &id_); });
  }
  id_ = id;
}

}  // namespace pj::scene3d::gl
