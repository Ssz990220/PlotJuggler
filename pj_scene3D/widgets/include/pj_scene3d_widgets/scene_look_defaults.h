// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

// Single source of truth for the 3D scene's default LOOK parameters — the values
// that MeshShadingParams, CompositeParams, SsaoPass and EdlPass bake as their
// defaults and that the scene3d_mesh_viewer demo seeds its look-dev controls
// from. Keeping them here (instead of duplicated literals) means the application
// and the demo can never drift apart: change a default once and both follow.
//
// These are the User's 2026-06-13 look-dev picks. Pure C++ (glm + <cmath>), no
// Qt, so it is includable from every layer that needs a default.
namespace pj::scene3d::look {

// World-space direction TO the key ("sun") light from azimuth/elevation in
// degrees (Z-up). Shared by MeshShadingParams' key_light_dir default and the
// demo's key-light sliders / CLI overrides, so the angle defaults live in ONE
// place. Not constexpr: std::cos/std::sin are not constexpr in C++20.
inline glm::vec3 keyDirFromAzEl(float az_deg, float el_deg) {
  const float az = glm::radians(az_deg);
  const float el = glm::radians(el_deg);
  const float ce = std::cos(el);
  return glm::vec3(ce * std::cos(az), ce * std::sin(az), std::sin(el));
}

// ---- Mesh shading (MeshShadingParams) ----
inline constexpr float kRoughness = 0.6f;              // visual-mesh GGX roughness
inline constexpr float kReflectivity = 0.06f;          // dielectric f0
inline constexpr float kAmbientScale = 0.5f;           // image-based ambient weight
inline constexpr float kKeyLightScale = 1.6f;          // fixed world "sun" weight
inline constexpr float kFillLightScale = 0.5f;         // camera-locked fill weight
inline constexpr float kEnvIntensity = 1.0f;           // analytic specular IBL weight
inline constexpr float kKeyLightAzimuthDeg = 40.0f;    // key-light azimuth (Z-up)
inline constexpr float kKeyLightElevationDeg = 55.0f;  // key-light elevation
inline constexpr float kMeshOpacity = 1.0f;
inline constexpr float kCollisionOpacity = 0.4f;

// ---- Composite / post (CompositeParams) ----
inline constexpr int kTonemapMode = 1;  // 0 None, 1 ACES, 2 AgX, 3 Neutral
inline constexpr float kExposure = 1.3f;
inline constexpr float kSaturation = 1.3f;  // post-tonemap saturation boost
inline constexpr float kAoStrength = 1.0f;  // SSAO blend into the composite
inline constexpr float kEdlFloor = 0.3f;    // EDL darkens toward floor*color

// ---- Screen-space passes (SsaoPass / EdlPass) ----
inline constexpr float kSsaoRadiusM = 0.4f;  // SSAO sample radius (metres)
inline constexpr float kSsaoPower = 1.0f;    // SSAO contrast exponent
inline constexpr float kEdlStrength = 1.0f;  // EDL response strength
inline constexpr float kEdlRadiusPx = 0.6f;  // EDL neighbour radius (pixels; plan spec was 1.4)
// Per-neighbour clamp on the log-depth gap (EDL is mesh-only). Surface creases
// produce gaps far below this, so they are unaffected; only a mesh pixel's
// silhouette — against a farther mesh, a non-mesh pixel (point cloud / grid /
// axes), or the empty background — exceeds it and gets bounded, turning a solid
// black band into a graded outline. Raising it darkens/widens that outline.
inline constexpr float kEdlMaxGap = 0.02f;

}  // namespace pj::scene3d::look
