// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/composite_media_source.h"

#include <algorithm>
#include <utility>

namespace PJ {

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer) {
  addLayer(std::move(layer), 1.0f);
}

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer, float opacity) {
  addLayer(std::move(layer), opacity, nullptr);
}

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer, float opacity, const void* identity) {
  if (layer == nullptr) {
    return;
  }
  Layer entry;
  entry.source = std::move(layer);
  entry.opacity = std::clamp(opacity, 0.0f, 1.0f);
  entry.identity = identity;
  layers_.push_back(std::move(entry));
}

void CompositeMediaSource::adoptContributions(const CompositeMediaSource& previous) {
  // A rebuild rewraps the same underlying sources in a fresh composite whose
  // layer caches are empty, so the first takeFrame() would drop to nullopt and
  // the viewer would blank until each source re-decodes. Copy each persisting
  // layer's last fused output (matched by identity) and arm a one-shot emit so
  // the rebuilt composite paints the current frame on its very next poll.
  for (auto& layer : layers_) {
    if (layer.identity == nullptr) {
      continue;
    }
    const auto match = std::find_if(previous.layers_.begin(), previous.layers_.end(), [&](const Layer& prev) {
      return prev.identity == layer.identity;
    });
    if (match == previous.layers_.end()) {
      continue;
    }
    layer.pixel_layers = match->pixel_layers;
    layer.overlays = match->overlays;
    force_emit_ = true;
  }
}

void CompositeMediaSource::setTimestamp(int64_t ts_ns) {
  for (auto& layer : layers_) {
    layer.source->setTimestamp(ts_ns);
  }
}

void CompositeMediaSource::invalidate() {
  // Fan out to every layer so they drop their per-timestamp dedup and re-decode
  // on the next setTimestamp — used after a composite rebuild re-seeds at the
  // (often unchanged) current tracker time.
  for (auto& layer : layers_) {
    layer.source->invalidate();
  }
}

std::optional<MediaFrame> CompositeMediaSource::takeFrame() {
  // Refresh the cached contribution of every layer that produced a new frame
  // this tick; layers that report nothing new keep their previous contribution.
  bool any_new = false;
  for (auto& layer : layers_) {
    auto fresh = layer.source->takeFrame();
    if (!fresh.has_value()) {
      continue;
    }
    any_new = true;
    layer.pixel_layers.clear();
    layer.overlays.clear();

    if (!fresh->pixel_layers.empty()) {
      for (auto& pixel_layer : fresh->pixel_layers) {
        pixel_layer.opacity = std::clamp(pixel_layer.opacity * layer.opacity, 0.0f, 1.0f);
        layer.pixel_layers.push_back(std::move(pixel_layer));
      }
    } else if (fresh->base.has_value()) {
      layer.pixel_layers.push_back(PixelLayer{std::move(*fresh->base), layer.opacity});
    }
    layer.overlays = std::move(fresh->overlays);
  }

  // Nothing new since the last poll: the consumer keeps the composite it has —
  // unless adoptContributions() armed a one-shot emit after a rebuild, in which
  // case we must hand the carried composite to the (just-repointed) consumer now
  // so it doesn't blank waiting for the next decode.
  if (!any_new && !force_emit_) {
    return std::nullopt;
  }
  force_emit_ = false;

  // Reassemble the full stack from every layer's retained contribution so a
  // layer that did not update this tick is still present in the composite.
  MediaFrame composite;
  for (const auto& layer : layers_) {
    for (const auto& pixel_layer : layer.pixel_layers) {
      if (!composite.base.has_value()) {
        composite.base = pixel_layer.frame;
      }
      composite.pixel_layers.push_back(pixel_layer);
    }
    for (const auto& overlay : layer.overlays) {
      composite.overlays.push_back(overlay);
    }
  }
  return composite;
}

}  // namespace PJ
