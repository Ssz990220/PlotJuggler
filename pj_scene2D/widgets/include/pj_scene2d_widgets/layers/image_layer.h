#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// Scene2DLayer for a single Image topic: contributes an image MediaSource to
// the composite. No per-layer configuration beyond the base.
class ImageLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  ImageLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
};

}  // namespace PJ
