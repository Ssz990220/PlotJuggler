#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/scene_frame.h"

namespace PJ {

/// One ordered pixel layer in a MediaFrame. Layers are composited bottom to top.
struct PixelLayer {
  DecodedFrame frame;
  float opacity = 1.0f;
};

/// Multi-layer frame produced by MediaSource at a given timestamp.
///
/// `base` is the pixel-buffer layer (image, video, depth colormap, segmentation).
/// `pixel_layers` are ordered pixel buffers, bottom to top. An empty
/// `pixel_layers` vector means consumers should use the legacy single `base`
/// as the pixel layer, so existing producers do not need to populate it.
/// `overlays` are vector primitives drawn on top (annotations, markers).
///
/// An empty MediaFrame (no base, no pixel layers, and no overlays) is a valid
/// return — it signals "nothing new since the last takeFrame()".
struct MediaFrame {
  std::optional<DecodedFrame> base;
  std::vector<PixelLayer> pixel_layers;
  std::vector<SceneFrame> overlays;

  [[nodiscard]] bool empty() const noexcept {
    return !base.has_value() && pixel_layers.empty() && overlays.empty();
  }
};

}  // namespace PJ
