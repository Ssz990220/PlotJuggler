#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <glm/glm.hpp>
#include <optional>
#include <string>

#include "pj_scene3d_core/tf/transform.h"

namespace pj::scene3d {

// Forward-declared on purpose: keeps the heavy tf_buffer.h out of a header that
// every render pass includes. FrameContext::lookup is defined in render_pass.cpp,
// where the full type is available.
class TransformBuffer;

// Camera + viewport state. Needed by every pass, TF-aware or not.
struct ViewParams {
  glm::mat4 view;
  glm::mat4 proj;
  int viewport_height_px = 0;  // shaders that size primitives in world coords
                               // (e.g. PointcloudRenderPass) divide by this.
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

  // SE(3) fixed_frame<-child at `time`, or nullopt if it can't resolve. Collapses
  // the buffer's Expected<Transform, LookupError> to optional: a pass only needs
  // to know whether the transform exists, never why it doesn't.
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

}  // namespace pj::scene3d
