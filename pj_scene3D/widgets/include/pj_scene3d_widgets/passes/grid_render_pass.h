#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <memory>

#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Draws the reference ground grid on the z=0 plane at the fixed-frame origin.
// TF-independent (ignores FrameContext); extent and color are configurable.
class GridRenderPass : public IRenderPass {
 public:
  void initializeGL() override;
  void render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) override;
  void releaseGL() override;

  void setColor(const glm::vec3& color);
  void setExtentMetres(float extent_m);

 private:
  glm::vec3 color_{0.35f, 0.35f, 0.35f};
  float extent_m_{10.0f};
  bool initialized_{false};
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
};

}  // namespace pj::scene3d
