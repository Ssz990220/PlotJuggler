// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLFunctions_4_5_Core>
#include <QSize>

#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/texture.h"

namespace pj::scene3d {

// Owns the per-context off-screen HDR render chain for SceneViewWidget.
//
// When MSAA is active, geometry renders into internal multisample color/depth
// textures and resolve() blits into single-sample RGBA16F/depth textures. When
// samples <= 1, geometry renders directly into that single-sample resolve FBO.
// Multisample render attachments are internal implementation details and are
// never exposed; post passes must sample resolvedColorTextureId() /
// resolvedDepthTextureId().
class SceneHdrFbo {
 public:
  SceneHdrFbo();
  ~SceneHdrFbo();

  SceneHdrFbo(SceneHdrFbo&& other) noexcept;
  SceneHdrFbo& operator=(SceneHdrFbo&& other) noexcept;

  SceneHdrFbo(const SceneHdrFbo&) = delete;
  SceneHdrFbo& operator=(const SceneHdrFbo&) = delete;

  // Configure the achieved sample count from QOpenGLContext::format().samples().
  // Values <= 1 select the single-sample path.
  void configure(int samples);

  // Allocate or resize all per-context FBO attachments in device pixels.
  // Idempotent when size is unchanged and the attachment ids are still present.
  void resize(int device_w, int device_h);

  // Bind the geometry render FBO. This is the MSAA FBO when samples > 1 and the
  // single-sample resolve FBO otherwise.
  void bind();

  // Resolve multisample color+depth into the single-sample FBO. No-op when the
  // chain is configured single-sample.
  void resolve();

  // Delete every GL object owned by this chain under the current dying context.
  void releaseGL();

  // Return the single-sample color texture id for post passes/present. In the
  // samples <= 1 path, this texture is also the render color attachment.
  [[nodiscard]] GLuint resolvedColorTextureId() const noexcept;

  // Return the single-sample depth texture id for post passes. In the samples <=
  // 1 path, this texture is also the render depth attachment.
  [[nodiscard]] GLuint resolvedDepthTextureId() const noexcept;

  // Current allocation size in device pixels.
  [[nodiscard]] QSize size() const noexcept;

  // True when the currently allocated chain is complete and usable.
  [[nodiscard]] bool ready() const noexcept;

 private:
  [[nodiscard]] bool hasAllocatedIds() const noexcept;
  void deleteMultisampleTextures() noexcept;

  int samples_{0};
  int width_{0};
  int height_{0};
  bool ready_{false};

  gl::Framebuffer render_fbo_;
  gl::Framebuffer resolve_fbo_;
  gl::Texture resolve_color_;
  gl::Texture resolve_depth_;
  GLuint msaa_color_{0};
  GLuint msaa_depth_{0};
};

}  // namespace pj::scene3d
