#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <string>
#include <string_view>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// A Scene2DLayer backed by a single canonical-schema decoder (ImageAnnotations,
// SceneEntities, …). The schema name and the list label are injected, so one
// class covers every vector-overlay layer instead of a near-identical subclass
// per schema. mediaSource() is the base default (the layer's borrowed source).
class SceneDecoderLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  SceneDecoderLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      const QString& kind_label, std::string_view schema, QObject* parent = nullptr);

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;

 private:
  std::string schema_;
};

}  // namespace PJ
