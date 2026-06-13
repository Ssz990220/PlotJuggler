// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/framebuffer.h"

#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {

Framebuffer::Framebuffer() = default;

Framebuffer::~Framebuffer() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteFramebuffers(1, &id_); });
  }
}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept
    : id_(std::exchange(other.id_, 0U)), owning_context_(std::exchange(other.owning_context_, nullptr)) {}

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteFramebuffers(1, &id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
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
      owning_context_ = QOpenGLContext::currentContext();
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
