#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "pj_scene3d_core/tf/tf_connections.h"
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Draws a straight line from every TF frame to its parent frame (rviz2 /
// Foxglove "show parent connections"). The geometry comes from the Qt/GL-free
// core helper buildTfConnectionSegments(); this pass is just the GL shell that
// uploads those segments and draws them as GL_LINES in a single flat color.
//
// Depends on FrameContext (unlike the grid): the segment endpoints are the
// frames' world origins, recomputed every paint so the lines track the scene as
// TF streams and the playhead scrubs. SceneViewWidget gates the pass with its
// own visibility flag (like grid_visible_/axes_visible_), so there is no
// internal visible state here.
class TfConnectionsRenderPass : public IRenderPass {
 public:
  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Line color (RGB 0..1). Default magenta — distinct from the RGB axis triads
  // and from the gray grid.
  void setColor(const glm::vec3& color) {
    color_ = color;
  }
  [[nodiscard]] glm::vec3 color() const noexcept {
    return color_;
  }

 private:
  glm::vec3 color_{1.0f, 0.0f, 1.0f};  // magenta
  bool initialized_{false};
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  // Reused across paints to avoid per-frame heap churn. The segment array is
  // uploaded to the VBO as-is (each TfConnectionSegment is two GL_LINES vertices).
  std::vector<TfConnectionSegment> segments_scratch_;
};

}  // namespace pj::scene3d
