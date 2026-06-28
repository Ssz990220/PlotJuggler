// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/shadow_camera.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace pj::scene3d {

ShadowCameraFit fitDirectionalShadowCamera(const AABB& bounds, const glm::vec3& to_light_dir, int shadow_map_size) {
  ShadowCameraFit fit;  // valid == false, identity matrix
  if (!bounds.valid || shadow_map_size <= 0) {
    return fit;
  }

  const float radius = 0.5f * aabbDiagonal(bounds);  // bounding-sphere radius
  const float dir_len = glm::length(to_light_dir);
  constexpr float kEps = 1e-6f;
  if (radius < kEps || dir_len < kEps) {
    return fit;  // a point box or a zero light direction => no usable frustum
  }

  const glm::vec3 center = aabbCenter(bounds);
  const glm::vec3 to_light = to_light_dir / dir_len;  // unit, points TO the light

  // Pad the ortho extents by a few texels so the texel snap (which can shift the
  // frustum by up to half a texel) can never push a caster corner out of the clip
  // cube. The bounding sphere already circumscribes the box, so the un-padded
  // half-extent is the sphere radius.
  const float base_texel = (2.0f * radius) / static_cast<float>(shadow_map_size);
  const float pad = 3.0f * base_texel;
  const float half = radius + pad;
  const float world_per_texel = (2.0f * half) / static_cast<float>(shadow_map_size);

  // Stable light-space basis: depends ONLY on the light direction, never on the
  // moving scene center. This is what makes the snap below deterministic — two
  // frames whose centers differ by less than a texel snap to the same grid.
  const glm::vec3 up_ref = (std::abs(to_light.z) > 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
  const glm::vec3 right = glm::normalize(glm::cross(up_ref, to_light));
  const glm::vec3 up = glm::normalize(glm::cross(to_light, right));

  // Texel-snap the center: quantize its coordinate along each light-basis axis to a
  // whole texel. A sub-texel scene jitter leaves the snapped center unchanged, so
  // the frustum (and every static receiver's shadow texels) stays put; crossing a
  // texel boundary jumps the frustum exactly one texel — no crawl either way.
  const auto snap = [world_per_texel](float c) { return std::round(c / world_per_texel) * world_per_texel; };
  const glm::vec3 snapped_center = snap(glm::dot(center, right)) * right + snap(glm::dot(center, up)) * up +
                                   snap(glm::dot(center, to_light)) * to_light;

  const glm::vec3 eye = snapped_center + to_light * (radius + pad);
  const glm::mat4 view = glm::lookAt(eye, snapped_center, up_ref);
  const float z_near = pad;
  const float z_far = pad + 2.0f * (radius + pad);
  const glm::mat4 proj = glm::ortho(-half, half, -half, half, z_near, z_far);

  fit.light_view_proj = proj * view;
  fit.world_units_per_texel = world_per_texel;
  fit.valid = true;
  return fit;
}

}  // namespace pj::scene3d
