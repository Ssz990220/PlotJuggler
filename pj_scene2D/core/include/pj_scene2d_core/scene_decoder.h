#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <any>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene2d_core/image_annotation_codec.h"

namespace PJ {

// Decoder of a single overlay message into a SceneFrame. Each schema family
// supported by pj_scene2D registers a concrete decoder via makeSceneDecoder().
//
// Two entry points for the two ingest routes (see scene_decoder_layer):
//  - decode(bytes)  — canonical-producer topics: the store holds canonical bytes.
//  - decode(object) — parser-backed topics: a MessageParser already produced the
//    canonical BuiltinObject, so decode it directly instead of re-serializing it
//    back to bytes just to re-parse them (the 3D side decodes the object too).
class ISceneDecoder {
 public:
  virtual ~ISceneDecoder() = default;
  virtual Expected<SceneFrame> decode(const uint8_t* data, size_t size) = 0;
  virtual Expected<SceneFrame> decode(const sdk::BuiltinObject& object) = 0;
};

namespace detail {

// ImageAnnotation is an alias for sdk::ImageAnnotations (scene_frame.h), so both
// routes land on the same struct — wrap it in a one-annotation SceneFrame.
[[nodiscard]] inline SceneFrame imageAnnotationsToFrame(ImageAnnotation annotations) {
  SceneFrame frame;
  frame.timestamp = annotations.timestamp;
  frame.annotations.push_back(std::move(annotations));
  return frame;
}

class ImageAnnotationsSceneDecoder final : public ISceneDecoder {
 public:
  Expected<SceneFrame> decode(const uint8_t* data, size_t size) override {
    auto annotations = deserializeImageAnnotation(data, size);
    if (!annotations.has_value()) {
      return unexpected(std::move(annotations).error());
    }
    return imageAnnotationsToFrame(std::move(*annotations));
  }

  Expected<SceneFrame> decode(const sdk::BuiltinObject& object) override {
    const auto* annotations = std::any_cast<sdk::ImageAnnotations>(&object);
    if (annotations == nullptr) {
      return unexpected(std::string("ImageAnnotationsSceneDecoder: object is not ImageAnnotations"));
    }
    return imageAnnotationsToFrame(*annotations);
  }
};

}  // namespace detail

class SceneEntities2DDecoder final : public ISceneDecoder {
 public:
  Expected<SceneFrame> decode(const uint8_t* data, size_t size) override;
  Expected<SceneFrame> decode(const sdk::BuiltinObject& object) override;
};

[[nodiscard]] inline std::unique_ptr<ISceneDecoder> makeSceneDecoder(std::string_view schema_name) {
  if (schema_name == kSchemaImageAnnotations) {
    return std::make_unique<detail::ImageAnnotationsSceneDecoder>();
  }
  if (schema_name == kSchemaSceneEntities) {
    return std::make_unique<SceneEntities2DDecoder>();
  }
  return nullptr;
}

}  // namespace PJ
