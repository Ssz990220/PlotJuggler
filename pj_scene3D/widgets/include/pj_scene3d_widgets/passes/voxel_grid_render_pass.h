// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms.hpp"  // PJ::sdk::Pose
#include "pj_scene3d_core/voxel_grid_value.h"    // VoxelDrawMode
#include "pj_scene3d_core/voxel_grid_view.h"     // VoxelValueKind
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_widgets/Colormap.h"  // PJ::Colormap

namespace pj::scene3d {

// Everything the pass needs to display one voxel grid: placement metadata plus the
// densely-packed field as EITHER a float volume (kScalar) OR RGBA8 (kRgba). The
// CPU pack runs once per new grid (in the layer), so a re-scrub to a cached grid
// re-uploads nothing. Built by VoxelGridLayer and moved into setGrid().
struct VoxelGridUpload {
  std::string frame_id;
  PJ::sdk::Pose origin;       ///< Lower-front-left corner pose, in `frame_id`.
  glm::vec3 cell_size{1.0f};  ///< Metric voxel size (need not be cubic).
  uint32_t column_count = 0;  ///< x (fastest)
  uint32_t row_count = 0;     ///< y
  uint32_t slice_count = 0;   ///< z
  VoxelValueKind kind = VoxelValueKind::kScalar;
  std::vector<float> scalar;  ///< column*row*slice floats, when kind == kScalar
  std::vector<uint8_t> rgba;  ///< column*row*slice*4 bytes, when kind == kRgba
};

// Draws a dense VoxelGrid as GPU-instanced cubes — one instance per voxel
// (`glDrawElementsInstanced` over a static unit cube). The vertex shader derives
// the voxel (cx,ry,sz) from `gl_InstanceID`, `texelFetch`es its value from a 3D
// texture, evaluates the draw predicate (degenerate-clips culled voxels so they
// rasterize nothing), and places/sizes the cube from origin + cell_size. The only
// per-frame CPU work is setting uniforms and issuing ONE draw call, so display
// cost is independent of voxel count (the hard requirement). Filtering display
// knobs (mode/threshold/range/colormap/opacity) are uniform-only — changing them
// while scrubbing re-uploads nothing.
class VoxelGridRenderPass : public IRenderPass {
 public:
  VoxelGridRenderPass();
  ~VoxelGridRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Stage a new grid for upload on the next render() (which runs with a current GL
  // context). Safe to call from the GUI thread outside paintGL.
  void setGrid(VoxelGridUpload upload);
  void clearGrid();

  void setDrawMode(VoxelDrawMode mode) {
    draw_mode_ = mode;
  }
  void setThreshold(float threshold) {
    threshold_ = threshold;
  }
  void setManualRange(float lo, float hi) {
    manual_lo_ = lo;
    manual_hi_ = hi;
  }
  void setAutoRange(bool on) {
    auto_range_ = on;
  }
  void setColormap(PJ::Colormap colormap) {
    colormap_ = colormap;
  }
  void setOpacity(float opacity) {
    opacity_ = opacity;
  }
  void setVisible(bool visible) {
    visible_ = visible;
  }
  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Whether a grid is currently staged for / held by the pass (set by setGrid,
  // cleared by clearGrid). Lets a layer test assert a back-scrub re-stages.
  [[nodiscard]] bool hasStagedGridForTest() const {
    return has_grid_;
  }
#endif

 private:
  void uploadPending();  // consume pending_full_: (re)allocate + fill the 3D texture

  std::string frame_id_;
  PJ::sdk::Pose origin_;
  glm::vec3 cell_size_{1.0f};
  uint32_t cols_{0};
  uint32_t rows_{0};
  uint32_t slices_{0};
  VoxelValueKind kind_{VoxelValueKind::kScalar};

  // Staged CPU payload, retained across context loss so releaseGL() can re-arm a
  // full re-upload without the layer re-decoding.
  bool has_grid_{false};
  bool pending_full_{false};
  std::vector<float> scalar_;
  std::vector<uint8_t> rgba_;
  float auto_lo_{0.0f};  // min/max over scalar_, computed on upload (kScalar only)
  float auto_hi_{1.0f};

  VoxelDrawMode draw_mode_{VoxelDrawMode::kNonZero};
  float threshold_{0.0f};
  float manual_lo_{0.0f};
  float manual_hi_{1.0f};
  bool auto_range_{true};
  PJ::Colormap colormap_{PJ::Colormap::kTurbo};
  float opacity_{1.0f};
  bool visible_{true};
  bool initialized_{false};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer cube_vbo_;
  gl::Buffer cube_ebo_;
  gl::Texture3D volume_;
};

}  // namespace pj::scene3d
