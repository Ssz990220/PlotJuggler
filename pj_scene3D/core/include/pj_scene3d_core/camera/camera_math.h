// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>

#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// A world-space ray: origin on the near plane, dir normalized toward far.
struct Ray {
  glm::vec3 origin{0.0f};
  glm::vec3 dir{0.0f, 0.0f, -1.0f};
};

// How far the visible scene extends from the eye, used to size the far plane and
// the zoom-out limit: the scene's own diagonal plus the focal's offset from the
// scene center (so framing stays correct even when the pivot is off to one side).
// Returns 0 for an invalid (unknown) AABB → callers fall back to working distance.
[[nodiscard]] float sceneReach(const AABB& bounds, const glm::vec3& focal);

// Decoupled adaptive near/far for a perspective camera. `working_distance` is the
// eye-to-focal distance (orbit radius); `scene_reach` is sceneReach() (0 = unknown).
//   near  = max(working_distance * 1e-2, 1e-3)   — from working distance ONLY, so
//           close inspection never clips regardless of how large the scene is.
//   far   = max(working_distance * 4, scene_reach) * 1.5
//   near  = max(near, far / 1e5)                 — ratio cap to bound depth precision.
// Writes results to out-params (kept testable without constructing a projection).
void adaptiveNearFar(float working_distance, float scene_reach, float& near, float& far);

// Build the world-space ray through a pixel. `cursor_px` is in Qt widget
// coordinates (origin top-left, y increasing downward); the y-flip into NDC is
// handled here. `view`/`proj` are the matrices currently in effect. Returns a
// default ray (origin 0, dir -Z) for a degenerate viewport or non-invertible
// transform. Works for both perspective and orthographic `proj`.
[[nodiscard]] Ray unprojectRay(
    glm::vec2 cursor_px, int viewport_w, int viewport_h, const glm::mat4& view, const glm::mat4& proj);

// Intersect a ray (origin `ro`, direction `rd`) with the infinite plane through
// point `p` with normal `n`. Returns false when the ray is parallel to the plane;
// otherwise sets `t` to the signed ray parameter (hit = ro + t*rd) and returns
// true. Callers that need a forward hit should check `t > 0`.
[[nodiscard]] bool rayPlane(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& p, const glm::vec3& n, float& t);

}  // namespace pj::scene3d
