#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;

// Scene2DLayer for a single streaming-video topic: per-frame canonical
// PJ.VideoFrame / Foxglove CompressedVideo messages in ObjectStore (one
// compressed H.264 frame per entry). Contributes a parser-mode
// StreamingVideoSource — which unwraps each entry to its Annex-B NAL span
// (zero-copy) and decodes on a worker thread — to the dock's composite. No
// per-layer configuration beyond the base.
//
// Every ObjectStore entry is one encoded frame on the wire, so decode is
// GOP-aware and runs through StreamingVideoDecoder.
class VideoLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  VideoLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
};

}  // namespace PJ
