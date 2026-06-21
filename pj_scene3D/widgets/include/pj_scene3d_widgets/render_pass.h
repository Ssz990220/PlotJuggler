#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <glm/glm.hpp>
#include <optional>
#include <string>

#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"

namespace pj::scene3d {

// Forward-declared on purpose: keeps the heavy tf_buffer.h out of a header that
// every render pass includes. FrameContext::lookup is defined in render_pass.cpp,
// where the full type is available.
class TransformBuffer;

// Camera + viewport state. Needed by every pass, TF-aware or not.
//
// Two viewport sizes are carried because they differ on HiDPI. viewport_*_px are
// LOGICAL (Qt widget) pixels, kept for the historical pass semantics (e.g. the
// HUD overlay derives DPR = device_height_px / viewport_height_px). device_*_px
// are FRAMEBUFFER pixels — the units gl_PointSize rasterizes in and glViewport
// expects — so any pass sizing primitives for the backing FBO must use them.
// device_height_px / viewport_height_px is the device-pixel ratio. When a caller
// hand-builds ViewParams without the device fields (they default to 0), passes
// fall back to the logical size.
struct ViewParams {
  glm::mat4 view;
  glm::mat4 proj;
  int viewport_height_px = 0;  // LOGICAL widget pixels (see struct doc).
  int viewport_width_px = 0;
  glm::vec3 camera_pos_world{};
  int device_width_px = 0;   // FRAMEBUFFER pixels (gl_PointSize, glViewport).
  int device_height_px = 0;  // FRAMEBUFFER pixels; 0 means "fall back to logical".
  // The bound view's mesh/collision look knobs, copied per frame by paintGL from
  // SceneViewWidget::meshShadingParams(). The mesh passes read it here instead of a
  // process-global, so sibling docks render independent opacity.
  MeshShadingParams shading{};
  // Supersampling factor in effect this frame (the off-screen chain renders at
  // render_scale x device pixels). A screen-space post pass whose parameters are
  // in PIXELS should multiply them by this so its footprint stays invariant to
  // the render resolution. Today only EDL's neighbour radius does (its depth-cue
  // outlines would otherwise shrink under supersampling); SSAO's blur tile has
  // the same dependency and is a known gap — harmless while supersampling is off
  // by default. 1.0 = no supersampling.
  float render_scale = 1.0f;
  // When true, the mesh pass also writes the scene FBO's R8 "is-mesh" mask
  // (COLOR_ATTACHMENT1) by enabling that draw buffer around its draws, so EDL can
  // restrict the eye-dome contour to mesh surfaces. Set only on the off-screen
  // path (the direct-to-backing fallback has no mask attachment). Kept LAST so
  // existing positional aggregate initializers default-init it (to false).
  bool write_mesh_mask = false;
};

// The TF-resolution triple a pass needs to place frame-relative data into the
// scene: the buffer, the fixed frame to resolve into, and the query time. They
// always travel together, so they live in one struct passed alongside (not
// inside) ViewParams. Passes that ignore TF (grid, axis overlay) simply don't
// read it. The reference members make this non-copyable/assignable — intended:
// it is a frame-local by-const-ref parameter, never stored.
struct FrameContext {
  const TransformBuffer& tf;
  const std::string& fixed_frame;
  TimePoint time;  // pj::scene3d::TimePoint (== PJ::Timepoint), from transform.h
  // Camera-relative RENDER ORIGIN: a world point lookup() subtracts (in double)
  // from every transform's translation, so geometry is uploaded relative to it and
  // — paired with ICamera::viewMatrixRelativeTo(render_origin) on the SAME origin —
  // a scene at large world coordinates (following a far frame in a UTM/GPS map)
  // renders without float32 cancellation. {0,0,0} = absolute world: every transform
  // comes back in true fixed-frame coordinates, so hand-built contexts (tests, and
  // any caller that omits it via aggregate init) keep the old behavior verbatim.
  glm::dvec3 render_origin{0.0};

  // SE(3) fixed_frame<-child at `time`, in RENDER space (render_origin already
  // subtracted from the translation), or nullopt if it can't resolve. Collapses the
  // buffer's Expected<Transform, LookupError> to optional: a pass only needs to know
  // whether the transform exists, never why it doesn't.
  [[nodiscard]] std::optional<Transform> lookup(const std::string& child) const;
};

// Abstract interface for one GL rendering pass: owns its shader program and GL
// buffers and draws a single primitive kind (grid, axes, point cloud, …).
// SceneViewWidget and the Scene3DLayers sequence their passes on every paintGL.
class IRenderPass {
 public:
  virtual ~IRenderPass() = default;
  virtual void initializeGL() = 0;
  virtual void render(const ViewParams& view_params, const FrameContext& frame_ctx) = 0;

  // Drop every GL object this pass owns and return it to its pre-initializeGL
  // state (handles forgotten, `initialized_` cleared, pending uploads re-armed).
  // SceneViewWidget calls this when the QOpenGLWidget's GL context is about to
  // be destroyed — the widget recreates its context on every reparent (ADS
  // dock/float/split) and destroys it on teardown. VAOs and FBOs are per-context
  // (never shared across contexts), so a handle cached in the old context is
  // invalid in the new one. After releaseGL the next initializeGL rebuilds
  // cleanly. Must be idempotent and safe to call with the dying context current.
  virtual void releaseGL() = 0;
};

// Contract for screen-space post passes. They share the IRenderPass
// lifecycle/render signature but also resize their render targets in DEVICE
// (framebuffer) pixels — matching the resolved scene HDR chain — when it changes
// size. The shipped implementations are SsaoPass and EdlPass; SceneViewWidget
// drives resize() with the backing FBO's device size each frame.
class IPostPass : public IRenderPass {
 public:
  // width_px/height_px are DEVICE (framebuffer) pixels, not logical widget pixels.
  virtual void resize(int width_px, int height_px) = 0;
};

}  // namespace pj::scene3d
