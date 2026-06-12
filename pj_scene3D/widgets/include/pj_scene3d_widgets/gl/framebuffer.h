// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLFunctions_4_5_Core>

namespace pj::scene3d::gl {

// Move-only RAII wrapper for one OpenGL framebuffer object name.
// Mirrors gl::Buffer: the FBO name is generated lazily on first bind and deleted
// only when a current context exists. This class owns no attachments.
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
};

}  // namespace pj::scene3d::gl
