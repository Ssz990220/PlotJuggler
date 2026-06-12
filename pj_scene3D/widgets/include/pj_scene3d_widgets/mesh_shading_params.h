// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace pj::scene3d {

// Scene-wide mesh shading knobs, shared by every MeshRenderPass instance.
// Pre-Part-C-SceneLighting stopgap: a process-wide setting keeps the look
// consistent across docks and lets the scene-controls panel / mesh_viewer demo
// tune it live. Read per draw — mutate freely from the GUI thread.
// Defaults are the User's 2026-06-10 look-dev pick (mesh_viewer demo).
struct MeshShadingParams {
  float roughness = 0.6f;          // visual-mesh GGX roughness (collision stays 0.85)
  float reflectivity = 0.06f;      // dielectric f0
  float ambient_scale = 1.0f;      // hemispheric ambient weight
  float direct_scale = 1.15f;      // headlight weight
  float mesh_opacity = 1.0f;       // visual meshes; 0 hides them (plan §9.3)
  float collision_opacity = 0.4f;  // collision hulls (translucent overlay); 0 hides
  bool meshes_visible = true;      // Part C eye toggles (independent of opacity)
  bool collisions_visible = true;
};
[[nodiscard]] MeshShadingParams& meshShadingParams();

}  // namespace pj::scene3d
