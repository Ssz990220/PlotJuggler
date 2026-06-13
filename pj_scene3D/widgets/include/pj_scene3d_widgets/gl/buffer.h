#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QPointer>

namespace pj::scene3d::gl {

// Move-only RAII wrapper for one OpenGL buffer object name.
//
// The GL name is generated lazily on the first bind(); the context that is
// current at that moment is recorded as the OWNING context. The destructor /
// move-assignment delete the name only when that owning context (or a context
// sharing its name space) is current — otherwise the id is dropped: a silent
// leak if the owner is still alive, a safe no-op if the owner was destroyed
// (the driver already freed the name with the context). The app does not share
// contexts, so deleting under a foreign context would corrupt a sibling view —
// hence the owning-context guard.
class Buffer {
 public:
  Buffer();
  ~Buffer();

  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  // The OpenGL buffer name, or 0 before the first bind().
  [[nodiscard]] GLuint id() const noexcept;

  // Bind to `target`, lazily generating the name (and recording the owning
  // context) on first use.
  void bind(GLenum target);

  // Bind to `target` and upload `bytes` of `data` as GL_STATIC_DRAW. The caller
  // must reuse the SAME `target` on every later bind() — the buffer remembers no
  // target. For GL_ELEMENT_ARRAY_BUFFER the binding is captured into the
  // CURRENTLY BOUND VAO's state, so a VAO must be bound first (ArrowGizmo relies
  // on vao.bind() preceding ebo.uploadStatic()); uploading an EBO with no/the
  // wrong VAO bound silently corrupts VAO state.
  void uploadStatic(GLenum target, const void* data, GLsizeiptr bytes);

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

}  // namespace pj::scene3d::gl
