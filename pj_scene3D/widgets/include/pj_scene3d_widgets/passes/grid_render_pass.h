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
// TF-independent (ignores FrameContext); extent, divisions, style, and color
// are configurable at runtime (geometry regenerates lazily on next render).
class GridRenderPass : public IRenderPass {
 public:
  // Visual style: classic line grid, or a filled checkerboard (alternate cells
  // drawn as solid quads; the others show the background through).
  enum class Style { kLines, kFilledCells };

  void initializeGL() override;
  void render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) override;
  void releaseGL() override;

  void setColor(const glm::vec3& color);
  void setExtentMetres(float extent_m);
  void setDivisions(int divisions);  // cells per side, clamped to [1, 200]
  void setStyle(Style style);

  [[nodiscard]] float extentMetres() const {
    return extent_m_;
  }
  [[nodiscard]] int divisions() const {
    return divisions_;
  }
  [[nodiscard]] Style style() const {
    return style_;
  }

 private:
  // (Re)build + upload the vertex buffer for the current extent/divisions/style.
  // Requires a current GL context (called from initializeGL/render).
  void rebuildGeometry();

  glm::vec3 color_{0.35f, 0.35f, 0.35f};
  float extent_m_{10.0f};
  int divisions_{10};
  Style style_{Style::kLines};
  bool geometry_dirty_{true};
  int vertex_count_{0};
  bool initialized_{false};
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
};

}  // namespace pj::scene3d
