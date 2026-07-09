#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/passes/pointcloud_aabb_reducer.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_widgets/Colormap.h"  // shared Colormap enum + colormapGlsl()

namespace pj::scene3d {

struct DecodedPointCloud;  // forward-declare — defined in pj_scene3d_core/pointcloud.h

// Fast-path retained state: the verbatim wire cloud (its anchor keeps bytes alive
// across GL-context recreation) + the precomputed bind layout. Hold wire BY VALUE
// (copies the BufferAnchor) — never reduce to a Span, or recreation re-uploads a
// dangling view.
struct FastCloudData {
  PJ::sdk::PointCloud wire;    // frame_id / data / anchor all live here — read wire.frame_id
  std::size_t point_count{0};  // == wire.width * wire.height (size_t: no uint32 overflow)
  AttribLayout layout;
};

class PointcloudRenderPass : public IRenderPass {
 public:
  // Shape mode for each point.
  //   kSphere  — world-radius sphere imposter; foreshortens with depth under a
  //              perspective camera, fixed on-screen size under an orthographic one.
  //   kPoint   — flat 1-pixel-fixed sprite (no perspective).
  //   kCube    — instanced 3D cube, fixed-frame-axis-aligned. Wired in
  //              Stage 6; the setter is accepted today but the draw call
  //              falls back to sphere until the cube program lands.
  enum class Shape { kSphere, kPoint, kCube };

  // Color sourcing mode.
  //   kField — per-point scalar → colormap → fragment color (today).
  //   kSolid — single uniform color, scalar ignored.
  //   kRgb   — per-point packed RGBA color used directly (no colormap). Requires
  //            the active cloud to carry `DecodedPointCloud::rgba`; falls back to
  //            white when absent.
  enum class ColorType { kField, kSolid, kRgb };

  // Colormap selector (used only in ColorType::kField mode). The colormap set +
  // its math (CPU LUT and the in-shader GLSL) is shared with the 2D depth view
  // via pj_widgets/Colormap.h, so a scalar maps to the same colour in both views.
  using Colormap = ::PJ::Colormap;

  PointcloudRenderPass();
  ~PointcloudRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Replaces the cloud being rendered. Triggers VBO re-upload on next render.
  // If cloud is non-null and cloud->scalar.size() == cloud->positions.size(),
  // the scalar attribute is uploaded; otherwise scalars default to 0.
  void setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud);
  void setActiveFastCloud(FastCloudData cloud);

  // GPU AABB reduction (fast path only). When enabled, render() dispatches a
  // compute reduction of the fast-path VBO's geometric bounds after each upload
  // and, every frame, polls the prior dispatch; a completed result is delivered
  // through setBoundsCallback(). The caller (PointCloudLayer) only enables this
  // once the layout is GPU-eligible (4-byte-aligned float32 xyz) and keeps a CPU
  // scan running until gpuAabbAvailable() confirms the compute path works.
  void setGpuAabbEnabled(bool enabled) {
    gpu_aabb_enabled_ = enabled;
  }
  // Invoked from render() (GL thread) with the freshly read-back source-frame
  // AABB whenever an async reduction completes. An invalid AABB means the cloud
  // had no finite points.
  void setBoundsCallback(std::function<void(std::optional<AABB>)> callback) {
    bounds_callback_ = std::move(callback);
  }
  // True once the compute reduction has compiled+linked on this context (known
  // only after the first dispatch). Lets the layer drop its CPU scan.
  [[nodiscard]] bool gpuAabbAvailable() const {
    return aabb_reducer_.available();
  }
  // True once a dispatch has been attempted, so gpuAabbAvailable() is
  // authoritative (distinguishes "not yet probed" from "probed, unsupported").
  [[nodiscard]] bool gpuAabbProbed() const {
    return aabb_reducer_.probed();
  }

  // Range used to normalize the scalar field to [0,1] for the colormap.
  void setColormapRange(float min_value, float max_value);

  // FIXED-FRAME axis colouring. -1 (default) → colour by the uploaded per-point
  // scalar attribute. 0/1/2 → ignore the attribute and colour by the x/y/z
  // coordinate of the point AFTER the source→fixed model transform, computed on
  // the GPU in the vertex shader. This keeps colour-by-height consistent across
  // sensors at different mounts (the raw x/y/z field is sensor-local).
  void setScalarAxis(int axis);

  // Source-frame bounds that drive the auto colormap range when setScalarAxis()
  // selected a spatial axis. When engaged, render() derives the [min,max] for
  // that axis from these bounds transformed by the SAME per-frame model used to
  // place the geometry — so colour and range never disagree as the TF moves.
  // Pass std::nullopt to fall back to the explicit setColormapRange() values
  // (manual range, or a non-spatial field's scalar range).
  void setSpatialAutoBounds(std::optional<AABB> source_bounds);

  // World-coordinate radius for sphere shape (and side length for cube once
  // Stage 6 lands). Default 0.01 m = 1 cm — chosen to match the pre-Stage-5
  // visuals exactly. Stage 7's UI will surface a larger default.
  void setSizeMeters(float meters);

  // Pixel size for kPoint shape (ignored in sphere/cube). Fractional sizes are
  // honoured (gl_PointSize is a float). Clamped to >= 1 px. Default 2 px.
  void setSizePixels(float pixels);

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

  // Effective opacity (0..1) for points whose colour scalar leaves [range_min,
  // range_max], instead of clamping them opaque to the colormap ends. 1 = no
  // change; values in (0,1) blend them; 0 culls them in the vertex shader. Only
  // bites in ColorType::kField (solid/rgb have no scalar). The layer folds its
  // opacity scrubber + visibility eye into this single value. Default 1.
  void setOutsideRangeAlpha(float alpha);

  // Per-pass visibility — when false, render() is a no-op. Used by
  // SceneViewWidget to hide individual pointcloud topics without
  // destroying their GL state. Default true.
  void setVisible(bool visible) {
    visible_ = visible;
  }
  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
 public:
  [[nodiscard]] bool cloudDirtyForTest() const {
    return cloud_dirty_;
  }
  [[nodiscard]] std::size_t vboPointCountForTest() const {
    return vbo_point_count_;
  }
  [[nodiscard]] bool cubeInstanceBindingsDirtyForTest() const {
    return cube_instance_bindings_dirty_;
  }
  [[nodiscard]] bool activeCloudIsFastForTest() const {
    return std::holds_alternative<FastCloudData>(cloud_);
  }
#endif

 private:
  [[nodiscard]] const std::string& activeFrameId() const;
  [[nodiscard]] bool hasRetainedCloud() const;

  std::variant<std::monostate, FastCloudData, std::shared_ptr<const DecodedPointCloud>> cloud_;
  bool cloud_dirty_{false};
  float range_min_{0.0f};
  float range_max_{1.0f};
  // -1 → colour by uploaded scalar; 0/1/2 → colour by fixed-frame x/y/z (GPU).
  int scalar_axis_{-1};
  // Engaged only with scalar_axis_ >= 0: source-frame bounds whose transformed
  // axis extent becomes the auto colormap range, recomputed per frame.
  std::optional<AABB> spatial_auto_bounds_;
  float size_meters_{0.01f};
  float size_pixels_{2.0f};
  bool initialized_{false};
  bool visible_{true};

  Shape shape_{Shape::kSphere};
  ColorType color_type_{ColorType::kField};
  glm::vec3 solid_color_{1.0f, 1.0f, 1.0f};
  Colormap colormap_{Colormap::kTurbo};
  bool invert_lut_{false};
  float outside_range_alpha_{1.0f};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  std::size_t vbo_point_count_{0};

  // GPU AABB reduction over vbo_ (fast path only) — see setGpuAabbEnabled().
  PointcloudAabbReducer aabb_reducer_;
  bool gpu_aabb_enabled_{false};
  std::function<void(std::optional<AABB>)> bounds_callback_;

  // Cube path — separate program + static cube mesh. The same cloud VBO
  // (vbo_) is bound as a per-instance attribute buffer; no per-frame
  // upload changes versus the points/sphere path.
  std::unique_ptr<gl::Program> cube_program_;
  gl::VertexArray cube_vao_;
  gl::Buffer cube_vbo_;
  gl::Buffer cube_ebo_;
  // The per-instance attribs in cube_vao_ reference vbo_'s stable buffer ID,
  // but the active cloud can change stride/offset/type, so re-specify the VAO
  // format whenever the retained cloud variant is swapped.
  bool cube_instance_bindings_dirty_{true};
};

}  // namespace pj::scene3d
