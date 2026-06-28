// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QOpenGLFunctions_4_5_Core>

#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/texture.h"

namespace pj::scene3d {

// Owns the per-context shadow-map render TARGET: a square GL_DEPTH_COMPONENT32F
// depth texture plus its depth-only FBO (no color attachment), sized from
// `kShadowMapSize` independently of the scene FBO / `render_scale`.
//
// NOT an IPostPass: it is a GEOMETRY pre-pass target used BEFORE renderScene, not a
// screen-space pass after resolve. SceneViewWidget fits the light frustum, calls
// begin(), drives each mesh layer's renderShadowCasters() (which depth-draws into the
// bound FBO via that layer's own MeshRenderPass depth program), then end() — after
// which the caller rebinds its scene FBO + viewport. Mesh/grid receivers later sample
// depthTextureId().
//
// Per-context like every pass: releaseGL() drops the GL objects under the dying
// context and ensure() lazily rebuilds in the next one (the codebase's #1 bug class
// is forgetting this). The depth texture keeps the gl::Texture default CLAMP_TO_EDGE
// + GL_NEAREST wrap/filter; the receiver shader guards out-of-frustum UVs as fully
// lit, so no border color or GL_TEXTURE_COMPARE_MODE is needed (plain sampler2D PCF).
class ShadowMapPass {
 public:
  // Allocate the depth texture + FBO if not already complete in the current context.
  // Idempotent and cheap to call every frame. REQUIRES a current GL context.
  void ensure();

  // Bind the shadow FBO, set the shadow-map viewport, clear depth, and enable depth
  // test + slope-scaled polygon offset (caster-side acne control). After this the
  // caller depth-draws every caster, then calls end(). No-op until ready().
  void begin();

  // Disable the polygon offset. The CALLER then rebinds its own scene render target
  // and viewport (only it knows offscreen-vs-backing and the device size).
  void end();

  // Depth texture id for receivers to sample; 0 when not ready.
  [[nodiscard]] GLuint depthTextureId() const noexcept;
  [[nodiscard]] bool ready() const noexcept {
    return ready_;
  }

  // Drop all GL objects under the dying context (mirrors every pass's releaseGL).
  void releaseGL();

 private:
  gl::Framebuffer fbo_;
  gl::Texture depth_;
  bool ready_{false};
};

}  // namespace pj::scene3d
