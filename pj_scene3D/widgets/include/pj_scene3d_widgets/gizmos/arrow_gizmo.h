#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"

namespace pj::scene3d {

// Solid 3D arrow gizmo: cylinder shaft + cone head, pointing along +X in
// model space. Re-tessellated whenever Params change.
//
// Built for the camera-orientation HUD (three differently-coloured copies
// of one mesh, oriented along X/Y/Z), but kept general enough to reuse for
// future selection / TF-axis / annotation work.
class ArrowGizmo {
 public:
  enum class Shading { kLit, kFlat };

  struct Params {
    // Total length from origin to arrow tip (cylinder + cone, in model units).
    float length = 1.0f;
    // Cylinder shaft radius.
    float shaft_radius = 0.08f;
    // Cone head axial length (subset of `length`; shaft length =
    // length - head_length).
    float head_length = 0.25f;
    // Cone base radius (where it joins the cylinder).
    float head_radius = 0.18f;
    // Cylinder/cone tessellation around the axis. Clamped to >= 3.
    int segments = 24;
  };

  // Build the program and the mesh. Safe to call repeatedly; subsequent
  // calls only re-upload the mesh if `params` differs from the current state.
  void initializeGL(const Params& params);

  // Re-tessellate and re-upload the mesh. No-op if `params` matches the
  // current state. Call after initializeGL. REQUIRES the owning GL context to be
  // current: it binds the VAO and uploads VBO/EBO, which throw with no context
  // current and corrupt a foreign context if the wrong one is current. GUI-slot
  // callers must defer this to a paint (see AxisRenderPass/AxisOverlayPass).
  void rebuild(const Params& params);

  // Drop the GL program + buffers and reset to the pre-initializeGL state, so
  // the next initializeGL rebuilds them in the current context. Used by the
  // owning render pass when the GL context is recreated (see
  // IRenderPass::releaseGL). The CPU-side mesh is kept; mesh_dirty_ is set so
  // it re-uploads on rebuild.
  void releaseGL();

  [[nodiscard]] const Params& params() const noexcept {
    return params_;
  }

  // Draw one arrow. The caller is responsible for setting any required GL
  // state (blend, depth test, etc.); this method only binds its own program
  // and VAO.
  //   mvp        : full projection * view * model transform.
  //   normal_mat : mat3(view * model) — used when shading == kLit so the light
  //                direction is camera-stable.
  //   color      : RGBA tint; the ALPHA is the annotation opacity/coverage,
  //                written as frag alpha and consumed by renderScene's
  //                annotation blend mode (1.0 = solid, today's look).
  void render(
      const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading = Shading::kLit);

  // Split render API for callers that draw many arrows in a row (TF-frame
  // triads), to avoid N redundant program binds. Call bindForRender() once,
  // then drawBound() per arrow, then unbindAfterRender() once. drawBound()
  // assumes the program is already current (bindForRender did program_->use())
  // and skips the per-call use()/glUseProgram(0) that render() pays. Mixing
  // drawBound() with render() (which binds/unbinds its own program) inside one
  // bind scope is a usage error. All three are no-ops when the gizmo isn't
  // initialized, so callers don't need to re-check.
  void bindForRender();
  void drawBound(
      const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading = Shading::kLit);
  void unbindAfterRender();

 private:
  void generateMesh();
  void uploadMesh();

  Params params_{};
  bool initialized_ = false;
  bool mesh_dirty_ = true;
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  gl::Buffer ebo_;
  std::vector<float> vertex_data_;  // interleaved (pos.xyz, normal.xyz)
  std::vector<uint32_t> index_data_;
};

// Draw a single coordinate-frame triad (X/Y/Z arrows) with one bound program.
// The ArrowGizmo mesh points along +X; this owns the two model-space rotations
// that orient the +Y and +Z arms (rotate +90° about +Z, rotate -90° about +Y),
// so the three call sites that re-derived them (AxisRenderPass, AxisOverlayPass,
// MarkerRenderPass axes) share one source of truth.
//
// Composition per arm (preserving each caller's exact order):
//   mvp    = proj * view * (base * rot * arm_scale)
//   normal = mat3(view * base * rot * arm_scale)
// where rot is identity/kYRotate/kZRotate for X/Y/Z. arm_scale lets the marker
// path bake per-axis length/thickness in; pass identity when not needed.
//
// PRECONDITION: arrow.bindForRender() must already have been called (the program
// is current). Issues three drawBound() calls and does NOT bind/unbind the
// program — the caller brackets a whole triad batch with bindForRender/
// unbindAfterRender. `shading` matches the caller's prior render() shading
// (kLit for the axis/HUD triads, kFlat for marker axes).
void renderTriadBound(
    ArrowGizmo& arrow, const glm::mat4& proj, const glm::mat4& view, const glm::mat4& base, const glm::mat4& arm_scale,
    const std::array<glm::vec4, 3>& colors, ArrowGizmo::Shading shading = ArrowGizmo::Shading::kLit);

}  // namespace pj::scene3d
