#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLFunctions_4_5_Core>

namespace pj::scene3d::gl {

class VertexArray {
 public:
  VertexArray();
  ~VertexArray();

  VertexArray(VertexArray&& other) noexcept;
  VertexArray& operator=(VertexArray&& other) noexcept;

  VertexArray(const VertexArray&) = delete;
  VertexArray& operator=(const VertexArray&) = delete;

  [[nodiscard]] GLuint id() const noexcept;
  void bind();
  void unbind();

 private:
  GLuint id_{0};
};

}  // namespace pj::scene3d::gl
