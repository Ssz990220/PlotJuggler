#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QList>
#include <QOpenGLWidget>
#include <QPoint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/time.hpp"  // PJ::Timepoint
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/gpu_profiler.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/moving_average.h"
#include "pj_scene3d_widgets/passes/axis_overlay_pass.h"
#include "pj_scene3d_widgets/passes/axis_render_pass.h"
#include "pj_scene3d_widgets/passes/edl_pass.h"
#include "pj_scene3d_widgets/passes/grid_render_pass.h"
#include "pj_scene3d_widgets/passes/ssao_pass.h"
#include "pj_scene3d_widgets/scene_hdr_fbo.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace pj::scene3d {

class Scene3DLayer;

// Standalone Qt widget that renders the 3D scene: a grid at the fixed-frame
// origin, an XYZ axis triad per TF frame, and zero or more user layers
// (PointCloud topics, future URDF robots, etc.). Layers are non-owning
// — SceneDockWidget owns them and Scene3DDockWidget pushes the base-owned
// render order into this view whenever it changes.
class SceneViewWidget : public QOpenGLWidget {
  Q_OBJECT

 public:
  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(PJ::Timepoint t);
  void setFixedFrame(const std::string& frame);

  // Replace the render order: index 0 renders first (behind), the last on top.
  // For coplanar overlays (costmaps) this is what decides the overlap winner.
  void setLayers(const std::vector<Scene3DLayer*>& ordered);

  [[nodiscard]] const std::vector<Scene3DLayer*>& layers() const {
    return layers_;
  }

  AxisRenderPass& axisPass() {
    return axes_;
  }
  GridRenderPass& gridPass() {
    return grid_;
  }
  SsaoPass& ssaoPass() {
    return ssao_;
  }
  EdlPass& edlPass() {
    return edl_;
  }

  // Composite/look knobs (read in paintGL; call update() after changing).
  // Defaults are the User's 2026-06-10 look-dev pick (mesh_viewer demo).
  struct CompositeParams {
    int tonemap_mode = look::kTonemapMode;  // 0 None, 1 ACES, 2 AgX, 3 Khronos PBR Neutral
    float exposure = look::kExposure;
    float saturation = look::kSaturation;  // post-tonemap; data pixels only
    float ao_strength = look::kAoStrength;
    bool ssao_enabled = true;
    bool edl_enabled = true;
    float edl_floor = look::kEdlFloor;  // EDL darkens toward floor*color (0 = old black; 1 = off)
  };
  [[nodiscard]] CompositeParams& compositeParams() {
    return composite_params_;
  }

  // This view's mesh/collision look knobs (read in paintGL into ViewParams::shading;
  // call update() after changing). Per-view, so two 3D docks diverge independently —
  // the scene-controls panel drives only its bound view's copy.
  [[nodiscard]] MeshShadingParams& meshShadingParams() {
    return shading_params_;
  }
  [[nodiscard]] ICamera& camera() {
    return *camera_;
  }

  // Selectable camera controllers. Enumerator order matches the combo-box order
  // in Scene3DDockWidget, so a combo index casts directly to a CameraModel.
  enum class CameraModel { Orbit, XYOrbit, Fly, TopDownOrtho };
  // Switch the active controller, carrying the current pose across so the view
  // doesn't jump (capture state → construct → adoptState → swap → repaint).
  void setCameraModel(CameraModel model);
  // Latest scene extent (union of entity worldBounds()); forwarded to the active
  // camera for adaptive near/far and framing.
  void setSceneBounds(const AABB& bounds);

  // Re-poll the TransformBuffer for the current frame set; emits
  // framesChanged if the set differs from the previous poll.
  void refreshAvailableFrames();

  const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  // Part C scene-controls fan-out (call update() after changing).
  void setGridStyle(GridRenderPass::Style style) {
    grid_.setStyle(style);
  }
  void setGridDivisions(int divisions) {
    grid_.setDivisions(divisions);
  }
  void setGridExtentMetres(float extent_m) {
    grid_.setExtentMetres(extent_m);
  }
  void setGizmoSize(float length_m) {
    axes_.setAxisLength(length_m);
  }
  void setGizmoOpacity(float opacity) {
    axes_.setOpacity(opacity);
  }
  void setGridVisible(bool visible) {
    grid_visible_ = visible;
  }

  // Per-view scene-control readback (the inverse of the setters above) so the
  // config panel can REFLECT the focused dock's own look on bind, and the dock
  // can persist it per-dock in xmlSaveState — each 3D view keeps independent
  // grid/frame/mesh settings rather than sharing one global look.
  [[nodiscard]] bool gridVisible() const {
    return grid_visible_;
  }
  [[nodiscard]] GridRenderPass::Style gridStyle() const {
    return grid_.style();
  }
  [[nodiscard]] int gridDivisions() const {
    return grid_.divisions();
  }
  [[nodiscard]] float gridExtentMetres() const {
    return grid_.extentMetres();
  }
  [[nodiscard]] float gizmoSize() const {
    return axes_.axisLength();
  }
  [[nodiscard]] float gizmoOpacity() const {
    return axes_.opacity();
  }

  // Show/hide the per-frame TF axis triads. The TF buffer is still used to
  // transform layers regardless — this only gates drawing the axes. Default
  // visible (TF is a first-class always-on display; see docs/REQUIREMENTS.md §4).
  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }

  // ---- Performance instrumentation & anti-aliasing benchmark hooks ----------
  // These exist to MEASURE rendering cost (GPU/CPU ms per frame) and to sweep
  // anti-aliasing settings; none change the default app look. The HUD is an
  // off-by-default overlay; render scale and the explicit sample override are
  // driven only by the mesh_viewer --benchmark sweep.

  // Toggle the on-screen GPU/CPU timing overlay (drawn over the scene each
  // frame). Also bound to the 'P' key. Off by default.
  void setShowPerfHud(bool on);
  [[nodiscard]] bool showPerfHud() const {
    return show_perf_hud_;
  }

  // Supersampling factor: the off-screen HDR chain renders at device_px * scale
  // and the present pass downsamples it to device resolution (true SSAA — it
  // anti-aliases both silhouettes and in-triangle shading). 1.0 = off (no
  // behavior change). Benchmark/measurement knob; not wired into the app UI.
  void setRenderScale(float scale);
  [[nodiscard]] float renderScale() const {
    return render_scale_;
  }

  // Override the off-screen MSAA sample count (1 disables MSAA; 2/4/8 typical).
  // The scene FBO is independent of the window's backing FBO, so this can be
  // swept at runtime without recreating the context. Reconfigures immediately
  // when a context is current. Benchmark knob; not wired into the app UI.
  void setSceneSamples(int samples);
  // The sample count the chain is actually allocated at (after driver clamping);
  // 1 when single-sampled.
  [[nodiscard]] int achievedSceneSamples() const;

  // Whole-scene-render GPU time (ms), measured non-stalling via a GL_TIME_ELAPSED
  // query and smoothed by a moving average. 0 until enough frames elapse.
  [[nodiscard]] double gpuFrameMillis() const {
    return scene_profiler_.averageMillis();
  }
  [[nodiscard]] bool hasGpuResult() const {
    return scene_profiler_.hasResult();
  }
  // CPU wall-time submitting the scene in paintGL (ms), smoothed with the same
  // moving average as the GPU readout. Should stay ~flat across MSAA settings —
  // the AA cost is the GPU's, not the CPU's. Excludes the HUD draw.
  [[nodiscard]] double cpuFrameMillis() const {
    return cpu_avg_.average();
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  // Draw the GPU/CPU timing overlay with QPainter on top of the rendered scene.
  // Called at the end of paintGL when show_perf_hud_; uses the latest harvested
  // profiler result (a few frames stale, which is imperceptible for a HUD).
  void drawPerfHud();
  // Close the GPU timer, record the CPU submission time, and draw the HUD — the
  // shared tail of both paintGL exits. Gated on show_perf_hud_.
  void finishFrameInstrumentation();
  // Release every pass's and layer's GL resources, returning them to their
  // pre-initializeGL state. Connected to the current QOpenGLContext's
  // aboutToBeDestroyed (rewired per context in initializeGL) and also called
  // from the destructor. QOpenGLWidget recreates its context on every reparent
  // (ADS dock/float/split) and destroys it on teardown; VAOs/FBOs aren't shared
  // across contexts, so stale handles must be dropped and rebuilt.
  void releaseGlResources();
  // Compile the fullscreen passthrough used to present the resolved HDR color
  // into the backing FBO. Per-context; rebuilt by initializeGL after recreation.
  void initializePresentProgram();
  // Clear + draw the full pass/layer sequence into the currently bound target.
  // mask_alpha_writes applies the legacy Wayland alpha guard (direct-to-backing
  // fallback only; the off-screen path forces alpha=1.0 in the present shader).
  void renderScene(const ViewParams& view_params, const FrameContext& frame_ctx, bool mask_alpha_writes);

  // Owned passes that don't depend on the layer count.
  AxisRenderPass axes_;
  GridRenderPass grid_;
  AxisOverlayPass overlay_;
  // Off-screen HDR render chain (Phase 0A): geometry renders into an RGBA16F +
  // DEPTH32F FBO at the backing FBO's achieved MSAA count, is resolved to
  // single-sample, and is presented to the backing FBO by the passthrough below.
  // Per-context, like every pass: released in releaseGlResources, rebuilt lazily.
  SceneHdrFbo scene_fbo_;
  // Fullscreen passthrough (empty-VAO triangle) presenting the resolved color
  // into the backing FBO; forces alpha=1.0 (the Wayland opaque-surface guard).
  std::optional<gl::Program> present_program_;
  gl::VertexArray present_vao_;
  // Screen-space AO over the resolved depth (Phase D); composite multiplies its
  // output into the HDR color. Degrades to no-AO when unavailable (u_has_ao=0).
  SsaoPass ssao_;
  // Eye-dome lighting over the resolved depth (Phase B); composite multiplies
  // its shade factor into the HDR color. Same degrade rule as SSAO.
  EdlPass edl_;
  CompositeParams composite_params_;
  // Per-view mesh/collision look knobs, copied into ViewParams::shading each paintGL.
  MeshShadingParams shading_params_;

  // Non-owning layer registry, in the order supplied by SceneDockWidget.
  std::vector<Scene3DLayer*> layers_;

  // Active camera controller (one of the CameraModel kinds). Owned; swapped by
  // setCameraModel(). Defaults to the improved Orbit.
  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};

  // Latest scene extent, retained so a camera-model swap can re-apply it to the
  // freshly constructed controller (bounds are not part of CameraState).
  AABB scene_bounds_{};

  std::shared_ptr<TransformBuffer> tf_;
  // The time the scene renders at. Distinct from the global playhead: the dock
  // pushes a clamped time via setTrackerTime, and paint is async from ticks, so
  // this is render state, not the clock. Fed to the per-frame FrameContext.
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;

  QList<FrameRow> last_frame_list_;

  // Whether the TF axis triads are drawn (see setAxesVisible). Does not affect
  // layer frame resolution, only the axes pass.
  bool axes_visible_ = true;
  // Whether the ground grid draws (Part C "Grid" eye toggle).
  bool grid_visible_ = true;
  // One warning per context when the HDR chain is unavailable and paintGL falls
  // back to direct-to-backing rendering; re-armed by initializeGL.
  bool scene_fbo_fallback_logged_ = false;

  // ---- Performance instrumentation state (see the benchmark-hooks block) -----
  // Non-stalling GPU timer over the scene passes; per-context, released in
  // releaseGlResources() and rebuilt lazily.
  gl::GpuProfiler scene_profiler_;
  // CPU stopwatch over the paintGL submission span (excludes the HUD draw).
  QElapsedTimer cpu_timer_;
  // Smoothed CPU submission time (same moving average as the GPU readout).
  MovingAverage cpu_avg_;
  // Supersampling factor for the off-screen chain (1.0 = off; see setRenderScale).
  float render_scale_ = 1.0f;
  // Explicit MSAA override (-1 = follow the context's achieved sample count, the
  // default; >= 0 = forced by setSceneSamples for the benchmark sweep).
  int scene_samples_override_ = -1;
  // Whether the on-screen GPU/CPU timing overlay draws (toggle: 'P').
  bool show_perf_hud_ = false;

  QPoint last_mouse_pos_;
  Qt::MouseButton active_button_{Qt::NoButton};

  // Connection to the current GL context's aboutToBeDestroyed signal. Rewired to
  // each new context in initializeGL and disconnected in the destructor so the
  // teardown hook never fires on a half-destroyed widget.
  QMetaObject::Connection context_cleanup_connection_;
};

}  // namespace pj::scene3d
