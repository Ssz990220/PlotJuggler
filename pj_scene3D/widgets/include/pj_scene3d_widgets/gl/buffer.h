#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLFunctions_4_5_Core>

namespace pj::scene3d::gl {

class Buffer {
 public:
  Buffer();
  ~Buffer();

  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  [[nodiscard]] GLuint id() const noexcept;
  void bind(GLenum target);
  void uploadStatic(GLenum target, const void* data, GLsizeiptr bytes);

 private:
  GLuint id_{0};
};

}  // namespace pj::scene3d::gl
