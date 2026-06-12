// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLFunctions_4_5_Core>
#include <cstdint>

namespace pj::scene3d::gl {

// RAII 2D texture. Mirrors gl::Buffer's ownership model (move-only, lazy gen on
// first use, delete-on-destruct via a current context). upload()/uploadSub()
// keep the existing R8 occupancy-grid path; allocate() creates arbitrary
// single-sample 2D textures for render targets such as RGBA16F/depth.
class Texture {
 public:
  Texture();
  ~Texture();

  Texture(Texture&& other) noexcept;
  Texture& operator=(Texture&& other) noexcept;

  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  [[nodiscard]] GLuint id() const noexcept;

  // (Re)allocate as a width x height R8 texture and upload width*height bytes.
  void upload(uint32_t width, uint32_t height, const uint8_t* data);
  // Patch a sub-rectangle; the texture must already be allocated at the
  // matching full dimensions. `data` is row-major width*height bytes.
  void uploadSub(int32_t x, int32_t y, uint32_t width, uint32_t height, const uint8_t* data);

  // Allocate storage for an uninitialized single-sample 2D texture.
  void allocate(GLenum internal_format, GLenum format, GLenum type, int width, int height);

  void bind(int unit);

 private:
  GLuint id_{0};
};

// Move-only owner for color 2D texture names adopted after pass-specific upload.
// Kept separate from the R8 occupancy-grid Texture so filtering, wrapping, and
// internal formats cannot accidentally cross-contaminate the two call sites.
class Texture2D {
 public:
  Texture2D();
  ~Texture2D();

  Texture2D(Texture2D&& other) noexcept;
  Texture2D& operator=(Texture2D&& other) noexcept;

  Texture2D(const Texture2D&) = delete;
  Texture2D& operator=(const Texture2D&) = delete;

  [[nodiscard]] GLuint id() const noexcept;
  void adopt(GLuint id) noexcept;

 private:
  GLuint id_{0};
};

}  // namespace pj::scene3d::gl
