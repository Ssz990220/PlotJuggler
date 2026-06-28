// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>

#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// Square shadow-map resolution in texels. Sized independently of the scene FBO and
// its render_scale supersampling — the shadow map is a separate target, so doubling
// scene supersampling must not quadruple shadow-map memory. 2048 gives ~mm texels on
// a single-robot scene; larger scenes trade sharpness for coverage (open decision in
// docs/plans/scene3d_mesh_shadows.md). The light frustum is fit at this resolution so
// the texel-snap in fitDirectionalShadowCamera matches the actual map.
inline constexpr int kShadowMapSize = 2048;

// Result of fitting an orthographic shadow camera to a directional light.
struct ShadowCameraFit {
  // World -> light clip space. Identity when `valid` is false. The receiver
  // shader projects a world position with this, maps clip.xy*0.5+0.5 to shadow-map
  // UV and clip.z*0.5+0.5 to the stored depth, and PCF-compares.
  glm::mat4 light_view_proj{1.0f};
  // World units spanned by one shadow-map texel (the ortho frustum width divided by
  // the map resolution). The receiver uses it to size the world-space normal offset
  // that pushes the comparison point off the surface (peter-panning / acne control),
  // so the bias tracks shadow-map resolution instead of being a magic constant.
  float world_units_per_texel{0.0f};
  // False => the caller must DISABLE shadows this frame (render unshadowed). Set
  // when there are no caster bounds, the bounds are degenerate (a point), or the
  // light direction is degenerate — fitting a frustum to any of those yields a
  // garbage or NaN matrix.
  bool valid{false};
};

// Fit an orthographic directional-light shadow camera that tightly covers `bounds`
// (the union of mesh-caster AABBs, in the world / fixed frame).
//
// `to_light_dir` is the world-space direction TO the light (Z-up); it need not be
// normalized (the existing key light is `MeshShadingParams::key_light_dir`).
// `shadow_map_size` is the square shadow-map resolution in texels; it drives the
// texel-snapping that keeps the shadow edge from crawling as the scene moves.
//
// The fit uses the AABB's bounding SPHERE for the ortho extents (rotation-invariant,
// so the covered area — and thus the shadow resolution — does not pulse as the
// camera or the scene rotates), pads the extents by a few texels to absorb the
// snap, and texel-snaps the frustom so a given world point always projects to the
// same texel until it moves a full texel.
//
// Returns `valid == false` for invalid/degenerate bounds or a zero light direction.
[[nodiscard]] ShadowCameraFit fitDirectionalShadowCamera(
    const AABB& bounds, const glm::vec3& to_light_dir, int shadow_map_size);

// Extend `bounds` to also enclose where its contents' shadows land on the plane
// z == ground_z, under a directional light pointing toward `to_light_dir` (Z-up). Each
// of the 8 corners is marched along -to_light_dir down to the ground plane and unioned
// in, so a shadow frustum fit to the result covers both the casters AND the receiving
// floor (the floor is a receiver but never a caster, so it is otherwise absent from
// caster bounds — and the cast shadow falls outside a caster-only frustum). Returns
// `bounds` unchanged when it is invalid or the light is horizontal/below
// (`to_light_dir.z <= 0`); corners already at/below the plane contribute nothing.
[[nodiscard]] inline AABB extendAabbToGroundShadow(const AABB& bounds, const glm::vec3& to_light_dir, float ground_z) {
  if (!bounds.valid || to_light_dir.z <= 1e-3f) {
    return bounds;
  }
  const glm::vec3 to_light = glm::normalize(to_light_dir);
  AABB out = bounds;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 c{
        (corner & 1) != 0 ? bounds.max.x : bounds.min.x, (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
        (corner & 4) != 0 ? bounds.max.z : bounds.min.z};
    const float height = c.z - ground_z;
    if (height <= 0.0f) {
      continue;  // already on/below the floor — its shadow would be behind the light
    }
    expandAABB(out, c - to_light * (height / to_light.z));  // march to z == ground_z
  }
  return out;
}

}  // namespace pj::scene3d
