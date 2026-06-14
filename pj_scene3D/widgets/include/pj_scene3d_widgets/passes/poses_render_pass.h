#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "pj_scene3d_core/poses_in_frame_render.h"  // PoseTriadInstance
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"

namespace pj::scene3d {

struct ViewParams;

// GPU-instanced renderer for a set of coordinate-triad gizmos (one arrow per
// PoseTriadInstance). The instances carry FRAME-LOCAL models; the fixed-frame TF
// transform is supplied to render() as a `u_frame_world` uniform, so the instance
// buffer is re-uploaded only when the set changes (setInstances) — TF and camera
// motion cost nothing. All arms draw in a single glDrawElementsInstanced call, so
// a thousands-of-poses PoseArray (e.g. an AMCL particle cloud) stays one draw.
//
// Not an IRenderPass: it is owned and driven by PosesInFrameLayer (which resolves
// the frame transform), mirroring how PointCloudLayer owns its render pass. Lit
// shading + annotation blend match the TF "Frames" gizmos.
class PosesRenderPass {
 public:
  // Build the program + the unit-arrow mesh + the instanced VAO. Per-frame-safe
  // (guards against re-init); requires a current GL context.
  void initializeGL();

  // Drop all GL objects (program, mesh, instance buffer) for context recreation;
  // the staged CPU instances survive and re-upload on the next render.
  void releaseGL();

  // Stage the arms to draw. CPU-only (no GL) — safe to call off the paint thread,
  // e.g. from the layer's tracker-time decode. Marks the GPU buffer dirty.
  void setInstances(std::vector<PoseTriadInstance> instances);

  // Draw every staged arm. `frame_world` is the fixed_frame<-source_frame SE(3)
  // the caller resolved from this frame's FrameContext. Sets its own annotation
  // blend and restores the data blend (the layer loop's ambient state).
  void render(const ViewParams& view_params, const glm::mat4& frame_world);

 private:
  bool initialized_ = false;
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;  // unit arrow mesh: interleaved pos.xyz, normal.xyz
  gl::Buffer ebo_;
  gl::Buffer instance_vbo_;  // per-instance model (loc 2-5) + color (loc 6)
  int index_count_ = 0;
  std::vector<PoseTriadInstance> instances_;
  bool instances_dirty_ = false;
};

}  // namespace pj::scene3d
