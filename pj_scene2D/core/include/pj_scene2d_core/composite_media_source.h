#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "pj_scene2d_core/media_frame.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

/// Multi-layer MediaSource. Fans setTimestamp() out to all owned layers and
/// fuses their MediaFrames into one composite frame returned by takeFrame().
///
/// Fusion rules:
///   - Pixel base: the FIRST layer that produces pixels wins for legacy `.base`.
///     The ordered `.pixel_layers` vector is populated bottom-to-top in add
///     order. Layers that only populate legacy `.base` are adapted into one
///     PixelLayer using this composite layer's opacity.
///   - Overlays: every layer's `.overlays` are concatenated in layer order
///     (so overlays from layer 0 render under overlays from layer 1).
///
/// Returns nullopt if no layer produced new data on this poll.
///
/// Ownership: the compositor takes ownership of every added layer. Layers
/// are polled in the order they were added.
class CompositeMediaSource : public MediaSource {
 public:
  CompositeMediaSource() = default;

  /// Append a layer. Layers are polled in addition order during takeFrame().
  void addLayer(std::unique_ptr<MediaSource> layer);

  /// Append a layer with opacity applied to pixel layers produced by it.
  void addLayer(std::unique_ptr<MediaSource> layer, float opacity);

  /// Number of layers currently owned.
  [[nodiscard]] size_t layerCount() const noexcept {
    return layers_.size();
  }

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

 private:
  struct Layer {
    std::unique_ptr<MediaSource> source;
    float opacity = 1.0f;
    // Last contribution (layer opacity already folded into the pixel layers),
    // retained so a tick where this layer reports nothing new still composites
    // its most recent output instead of dropping it (REQUIREMENTS §4.8 —
    // multi-rate layers are the norm; the compositor never drops a layer).
    std::vector<PixelLayer> pixel_layers;
    std::vector<SceneFrame> overlays;
  };

  std::vector<Layer> layers_;
};

}  // namespace PJ
