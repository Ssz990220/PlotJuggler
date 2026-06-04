// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLFunctions_4_5_Core>
#include <cstdint>

namespace pj::scene3d::gl {

// RAII single-channel (R8) 2D texture. Mirrors gl::Buffer's ownership model
// (move-only, lazy gen on first use, delete-on-destruct via a current context).
// Used by OccupancyGridRenderPass to hold the reconstructed grid's cells; the
// occupancy byte is read back in the shader as value * 255.
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

  void bind(int unit);

 private:
  GLuint id_{0};
};

}  // namespace pj::scene3d::gl
