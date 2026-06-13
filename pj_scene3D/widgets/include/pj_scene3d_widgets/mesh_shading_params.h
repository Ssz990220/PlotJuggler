// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace pj::scene3d {

// Per-view mesh shading / scene-look knobs. Owned by each SceneViewWidget next to
// CompositeParams (SceneViewWidget::meshShadingParams()) and copied into ViewParams
// every frame, so the bound view's mesh/collision opacity and shading reach the
// MeshRenderPass through render-time plumbing rather than a process-global. Read per
// draw; mutate freely from the GUI thread (call view->update() afterwards). Two 3D
// docks therefore hold independent look state and never clobber each other.
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

}  // namespace pj::scene3d
