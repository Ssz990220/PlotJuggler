// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/axis_render_pass.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

// Derive arrow geometry from the user-facing axis_length. Keeps the silhouette
// proportional regardless of the length setting.
ArrowGizmo::Params paramsForLength(float length) {
  ArrowGizmo::Params p;
  p.length = length;
  p.shaft_radius = length * 0.04f;  // shaft = 4% of length
  p.head_length = length * 0.30f;   // cone is 30% of length
  p.head_radius = length * 0.10f;   // cone base radius = 10% of length
  p.segments = 16;
  return p;
}

}  // namespace

void AxisRenderPass::initializeGL() {
  arrow_.initializeGL(paramsForLength(axis_length_));
  initialized_ = true;
}

void AxisRenderPass::releaseGL() {
  arrow_.releaseGL();
  initialized_ = false;
}

void AxisRenderPass::rebuildGizmo() {
  if (initialized_) {
    arrow_.rebuild(paramsForLength(axis_length_));
  }
}

void AxisRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!initialized_) {
    return;
  }

  // Per-axis model-space rotations: the gizmo points along +X. To draw the
  // Y arrow we rotate +X to +Y (+90° around +Z); for Z we rotate +X to +Z
  // (-90° around +Y).
  static const glm::mat4 kYRotate = glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), {0.0f, 0.0f, 1.0f});
  static const glm::mat4 kZRotate = glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), {0.0f, 1.0f, 0.0f});

  // Slightly desaturated R/G/B so adjacent frames don't clash visually with
  // the HUD overlay (which uses the saturated triplets).
  static constexpr glm::vec4 kColorX{0.95f, 0.30f, 0.30f, 1.0f};
  static constexpr glm::vec4 kColorY{0.30f, 0.85f, 0.30f, 1.0f};
  static constexpr glm::vec4 kColorZ{0.35f, 0.50f, 1.00f, 1.0f};

  // Solid 3D arrows participate in normal depth ordering — back ones get
  // occluded by front ones, and arrows hide behind opaque scene geometry.
  withGlFunctions([](auto& f) {
    f.glEnable(GL_DEPTH_TEST);
    f.glLineWidth(1.0f);
  });

  for (const std::string& frame : frame_ctx.tf.getAllFrames()) {
    const auto transform = frame_ctx.lookup(frame);
    if (!transform.has_value()) {
      continue;
    }

    const glm::mat4 frame_model = glm::mat4(transform->matrix());
    const glm::mat4 mx = frame_model;
    const glm::mat4 my = frame_model * kYRotate;
    const glm::mat4 mz = frame_model * kZRotate;

    arrow_.render(view_params.proj * view_params.view * mx, glm::mat3(view_params.view * mx), kColorX);
    arrow_.render(view_params.proj * view_params.view * my, glm::mat3(view_params.view * my), kColorY);
    arrow_.render(view_params.proj * view_params.view * mz, glm::mat3(view_params.view * mz), kColorZ);
  }
}

void AxisRenderPass::setAxisLength(float length_m) {
  if (length_m == axis_length_) {
    return;
  }
  axis_length_ = length_m;
  rebuildGizmo();
}

float AxisRenderPass::axisLength() const noexcept {
  return axis_length_;
}

}  // namespace pj::scene3d
