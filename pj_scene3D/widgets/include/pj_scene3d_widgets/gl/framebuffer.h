// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QPointer>

namespace pj::scene3d::gl {

// Move-only RAII wrapper for one OpenGL framebuffer object name.
// Mirrors gl::Buffer: the FBO name is generated lazily on first bind, recording
// the then-current context as the owner, and is deleted only when that owning
// context (or a sharing one) is current — otherwise the id is dropped. This
// class owns no attachments.
class Framebuffer {
 public:
  Framebuffer();
  ~Framebuffer();

  Framebuffer(Framebuffer&& other) noexcept;
  Framebuffer& operator=(Framebuffer&& other) noexcept;

  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  // Return the OpenGL FBO id, or 0 before the first bind().
  [[nodiscard]] GLuint id() const noexcept;

  // Bind this framebuffer to target, lazily creating the OpenGL name.
  void bind(GLenum target = GL_FRAMEBUFFER);

  // Bind a supplied default framebuffer id. Under QOpenGLWidget this must be
  // QOpenGLWidget::defaultFramebufferObject(); never assume the default is 0.
  static void bindDefault(GLuint default_fbo);

  // Check the currently bound framebuffer for target and log qWarning on failure.
  [[nodiscard]] bool checkComplete(GLenum target = GL_FRAMEBUFFER) const;

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

}  // namespace pj::scene3d::gl
