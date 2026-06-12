#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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
  // current state. Call after initializeGL.
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
  //   color      : RGBA tint applied per-fragment.
  void render(
      const glm::mat4& mvp, const glm::mat3& normal_mat, const glm::vec4& color, Shading shading = Shading::kLit);

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

}  // namespace pj::scene3d
