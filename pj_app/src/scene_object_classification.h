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
// handled by both families; the factory ladder gives 3D precedence (3D is tried
// first, 2D falls back for types the 3D family does not claim).
[[nodiscard]] inline bool is3dSceneObjectType(sdk::BuiltinObjectType type) {
  return Scene3DDockWidget::handlesObjectType(type);
}

[[nodiscard]] inline bool is2dSceneObjectType(sdk::BuiltinObjectType type) {
  return Scene2DDockWidget::handlesObjectType(type);
}

// The image-family object types — those a 2D media viewer renders: stills
// (kImage, raw or compressed), depth images (kImage with a depth encoding, or
// kDepthImage), video (kVideoFrame), and image annotations. Used for the curve-
// list "2D-viewable" icon and for placeholder-drop routing.
//
// A depth-encoded kImage is ALSO hostable in 3D (DepthCloud), but the depth-vs-
// color split is an `encoding` (payload) distinction, not a type one — so it is
// resolved by the consumer (the dock peeking a sample), never here. Because this
// predicate cannot tell depth from color, placeholder-drop routing treats the
// whole family as 2D-first; depth→3D is an explicit action (drop onto an
// existing 3D dock, or "Open in 3D view").
[[nodiscard]] inline bool isImageFamilyObjectType(sdk::BuiltinObjectType type) {
  return type == sdk::BuiltinObjectType::kImage || type == sdk::BuiltinObjectType::kVideoFrame ||
         type == sdk::BuiltinObjectType::kDepthImage || type == sdk::BuiltinObjectType::kImageAnnotations;
}

}  // namespace PJ
