#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_scene3d_core/scene_entities_render.h"

namespace pj::scene3d {

// Shared canonical->glm converters. These are the single source of truth for the
// trivial Pose/Point3/Vector3/ColorRGBA narrowings the decode and the widget draw
// paths all need; the widgets used to keep byte-identical (and occasionally
// divergent) private copies. No Qt, no GL — pure geometry, safe in the core.

// Compose a primitive's LOCAL model matrix from its pose: T * R, so the rotation
// acts about the pose's own origin and is then offset to the position (the correct
// rigid-body-pose semantics; R * T would orbit the frame origin). The rotation
// stays a quaternion all the way to the matrix (no Euler round-trip, no gimbal
// lock). Two gotchas handled here: the canonical sdk::Quaternion is {x, y, z, w}
// but glm::quat takes (w, x, y, z); and we normalize defensively in case a producer
// emits a non-unit quaternion (mat4_cast assumes unit length).
[[nodiscard]] glm::mat4 poseToMat4(const PJ::sdk::Pose& pose);

[[nodiscard]] glm::vec3 toVec3(const PJ::sdk::Point3& point);
[[nodiscard]] glm::vec3 toVec3(const PJ::sdk::Vector3& vec);

// ColorRGBA channels are 0..255; the result is the conventional 0..1 float color.
[[nodiscard]] glm::vec4 toVec4(const PJ::sdk::ColorRGBA& color);

// Adapt one canonical sdk::SceneEntities batch into GL-ready render structs.
// Pure CPU geometry: Pose -> local model matrix, frame_id interning, line
// topology expansion, color flattening, flat-normal computation. No Qt, no GL,
// no statefulness (deletions / lifetime are ignored — a future consumer layer).
[[nodiscard]] DecodedSceneEntities decodeSceneEntities(const PJ::sdk::SceneEntities& batch);

}  // namespace pj::scene3d
