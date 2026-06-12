// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/framebuffer.h"

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

Framebuffer::Framebuffer() = default;

Framebuffer::~Framebuffer() {
  if (id_ != 0U) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteFramebuffers(1, &id_); });
  }
}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept : id_(std::exchange(other.id_, 0U)) {}

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteFramebuffers(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
  }
  return *this;
}

GLuint Framebuffer::id() const noexcept {
  return id_;
}

void Framebuffer::bind(GLenum target) {
  withGlFunctions([this, target](auto& functions) {
    if (id_ == 0U) {
      functions.glGenFramebuffers(1, &id_);
    }
    functions.glBindFramebuffer(target, id_);
  });
}

void Framebuffer::bindDefault(GLuint default_fbo) {
  withGlFunctions([default_fbo](auto& functions) { functions.glBindFramebuffer(GL_FRAMEBUFFER, default_fbo); });
}

bool Framebuffer::checkComplete(GLenum target) const {
  return withGlFunctions([target](auto& functions) {
    const GLenum status = functions.glCheckFramebufferStatus(target);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      qWarning("OpenGL framebuffer incomplete: status=0x%04x", status);
      return false;
    }
    return true;
  });
}

}  // namespace pj::scene3d::gl
