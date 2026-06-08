// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/camera/camera_math.h"

#include <algorithm>
#include <cmath>

namespace pj::scene3d {

float sceneReach(const AABB& bounds, const glm::vec3& focal) {
  if (!bounds.valid) {
    return 0.0f;
  }
  return aabbDiagonal(bounds) + glm::length(focal - aabbCenter(bounds));
}

void adaptiveNearFar(float working_distance, float scene_reach, float& near, float& far) {
  near = std::max(working_distance * 1e-2f, 1e-3f);
  far = std::max(working_distance * 4.0f, scene_reach) * 1.5f;
  near = std::max(near, far / 1e5f);  // ratio cap → bounded depth precision
}

Ray unprojectRay(glm::vec2 cursor_px, int viewport_w, int viewport_h, const glm::mat4& view, const glm::mat4& proj) {
  Ray ray;
  if (viewport_w <= 0 || viewport_h <= 0) {
    return ray;
  }
  // Pixel → NDC. Qt y grows downward, NDC y grows upward, hence the flip.
  const float ndc_x = 2.0f * cursor_px.x / static_cast<float>(viewport_w) - 1.0f;
  const float ndc_y = 1.0f - 2.0f * cursor_px.y / static_cast<float>(viewport_h);

  const glm::mat4 inv = glm::inverse(proj * view);
  const glm::vec4 near_h = inv * glm::vec4(ndc_x, ndc_y, -1.0f, 1.0f);
  const glm::vec4 far_h = inv * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
  if (std::abs(near_h.w) < 1e-9f || std::abs(far_h.w) < 1e-9f) {
    return ray;
  }
  const glm::vec3 near_pt = glm::vec3(near_h) / near_h.w;
  const glm::vec3 far_pt = glm::vec3(far_h) / far_h.w;
  ray.origin = near_pt;
  ray.dir = glm::normalize(far_pt - near_pt);
  return ray;
}

bool rayPlane(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& p, const glm::vec3& n, float& t) {
  const float denom = glm::dot(rd, n);
  if (std::abs(denom) < 1e-9f) {
    return false;
  }
  t = glm::dot(p - ro, n) / denom;
  return true;
}

}  // namespace pj::scene3d
