// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"

namespace PJ {

// Host-side routing/iconography forwarders. Each scene family owns its accepted
// type set (the dock's static handlesObjectType, next to the code implementing
// support); these aliases exist so shell code reads as policy. kSceneEntities is
// currently 2D-only (the 3D family has no marker layer yet), so dropped markers
// route to scene2d; the factory ladder gives 3D precedence if that ever changes.
[[nodiscard]] inline bool is3dSceneObjectType(sdk::BuiltinObjectType type) {
  return Scene3DDockWidget::handlesObjectType(type);
}

[[nodiscard]] inline bool is2dSceneObjectType(sdk::BuiltinObjectType type) {
  return Scene2DDockWidget::handlesObjectType(type);
}

}  // namespace PJ
