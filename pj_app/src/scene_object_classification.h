// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pj_base/builtin/builtin_object.hpp"

namespace PJ {

// Host policy: which canonical object types the 3D scene displays. Single source
// of truth for both catalog iconography (a topic "marked as 3D") and drop
// routing (-> Scene3DDockWidget). Image/2D families are classified separately.
// Adding a 3D object type = one case here.
[[nodiscard]] inline bool is3dSceneObjectType(sdk::BuiltinObjectType type) {
  switch (type) {
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kFrameTransforms:
      return true;
    default:
      return false;
  }
}

}  // namespace PJ
