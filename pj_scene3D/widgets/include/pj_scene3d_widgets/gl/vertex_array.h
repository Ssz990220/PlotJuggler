#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QPointer>

namespace pj::scene3d::gl {

// Move-only RAII wrapper for one OpenGL vertex array object (VAO) name.
//
// The name is generated lazily on the first bind(), recording the then-current
// context as the OWNING context. The destructor / move-assignment delete the
// name only when that owning context (or a sharing one) is current; otherwise
// the id is dropped (a leak if the owner is alive, a safe no-op if it was
// destroyed). VAO names are PER-CONTEXT and never shared across contexts, so the
// owning-context guard is essential here: the app does not share contexts, and
// deleting a VAO under a foreign context would corrupt a sibling view's state.
class VertexArray {
 public:
  VertexArray();
  ~VertexArray();

  VertexArray(VertexArray&& other) noexcept;
  VertexArray& operator=(VertexArray&& other) noexcept;

  VertexArray(const VertexArray&) = delete;
  VertexArray& operator=(const VertexArray&) = delete;

  // The OpenGL VAO name, or 0 before the first bind().
  [[nodiscard]] GLuint id() const noexcept;

  // Bind this VAO, lazily generating the name (and recording the owning context)
  // on first use. Any GL_ELEMENT_ARRAY_BUFFER bound while this VAO is bound is
  // captured into the VAO's state.
  void bind();
  void unbind();

 private:
  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
};

}  // namespace pj::scene3d::gl
