#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_scene3d_core/scene_entities_render.h"

namespace pj::scene3d {

// Adapt one canonical sdk::SceneEntities batch into GL-ready render structs.
// Pure CPU geometry: Pose -> local model matrix, frame_id interning, line
// topology expansion, color flattening, flat-normal computation. No Qt, no GL,
// no statefulness (deletions / lifetime are ignored — a future consumer layer).
[[nodiscard]] DecodedSceneEntities decodeSceneEntities(const PJ::sdk::SceneEntities& batch);

}  // namespace pj::scene3d
