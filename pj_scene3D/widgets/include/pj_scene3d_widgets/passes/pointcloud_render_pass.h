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

struct DecodedPointCloud;  // forward-declare — defined in pj_scene3d_core/pointcloud.h

class PointcloudRenderPass : public IRenderPass {
 public:
  // Shape mode for each point.
  //   kSphere  — perspective-scaled sphere imposter (today's behaviour).
  //   kPoint   — flat 1-pixel-fixed sprite (no perspective).
  //   kCube    — instanced 3D cube, fixed-frame-axis-aligned. Wired in
  //              Stage 6; the setter is accepted today but the draw call
  //              falls back to sphere until the cube program lands.
  enum class Shape { kSphere, kPoint, kCube };

  // Color sourcing mode.
  //   kField — per-point scalar → colormap → fragment color (today).
  //   kSolid — single uniform color, scalar ignored.
  enum class ColorType { kField, kSolid };

  // Colormap selector (used only in ColorType::kField mode).
  enum class Colormap { kTurbo, kViridis, kPlasma, kGrayscale };

  PointcloudRenderPass();
  ~PointcloudRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Replaces the cloud being rendered. Triggers VBO re-upload on next render.
  // If cloud is non-null and cloud->scalar.size() == cloud->positions.size(),
  // the scalar attribute is uploaded; otherwise scalars default to 0.
  void setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud);

  // Range used to normalize the scalar field to [0,1] for the colormap.
  void setColormapRange(float min_value, float max_value);

  // World-coordinate radius for sphere shape (and side length for cube once
  // Stage 6 lands). Default 0.01 m = 1 cm — chosen to match the pre-Stage-5
  // visuals exactly. Stage 7's UI will surface a larger default.
  void setSizeMeters(float meters);

  // Pixel size for kPoint shape (ignored in sphere/cube). Default 2 px.
  void setSizePixels(int pixels);

  // Shape selector — see enum above. Default kSphere.
  void setShape(Shape shape);

  // Color-sourcing selector — see enum above. Default kField.
  void setColorType(ColorType type);

  // Uniform color used when ColorType == kSolid. Components in [0,1].
  // Default = white.
  void setSolidColor(glm::vec3 rgb);

  // Colormap used when ColorType == kField. Default kTurbo.
  void setColormap(Colormap cm);

  // When true, the LUT is sampled with t replaced by (1 - t) — same min/max
  // mapping, reversed color progression. Default false.
  void setInvertLut(bool invert);

  // Per-pass visibility — when false, render() is a no-op. Used by
  // SceneViewWidget to hide individual pointcloud topics without
  // destroying their GL state. Default true.
  void setVisible(bool visible) {
    visible_ = visible;
  }
  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

 private:
  std::shared_ptr<const DecodedPointCloud> cloud_;
  bool cloud_dirty_{false};
  float range_min_{0.0f};
  float range_max_{1.0f};
  float size_meters_{0.01f};
  int size_pixels_{2};
  bool initialized_{false};
  bool visible_{true};

  Shape shape_{Shape::kSphere};
  ColorType color_type_{ColorType::kField};
  glm::vec3 solid_color_{1.0f, 1.0f, 1.0f};
  Colormap colormap_{Colormap::kTurbo};
  bool invert_lut_{false};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  std::size_t vbo_point_count_{0};

  // Cube path — separate program + static cube mesh. The same cloud VBO
  // (vbo_) is bound as a per-instance attribute buffer; no per-frame
  // upload changes versus the points/sphere path.
  std::unique_ptr<gl::Program> cube_program_;
  gl::VertexArray cube_vao_;
  gl::Buffer cube_vbo_;
  gl::Buffer cube_ebo_;
  // The per-instance attribs in cube_vao_ reference vbo_'s buffer ID,
  // which is stable across cloud swaps once allocated. Set the binding
  // up exactly once after vbo_ first has data.
  bool cube_instance_bindings_dirty_{true};
};

}  // namespace pj::scene3d
