#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "pj_scene2d_core/image_annotation_codec.h"

namespace PJ {

// Schema-agnostic decoder of message bytes into a SceneFrame batch. Each
// schema family supported by pj_scene2D registers a concrete decoder via
// makeSceneDecoder().
class ISceneDecoder {
 public:
  virtual ~ISceneDecoder() = default;
  virtual Expected<SceneFrame> decode(const uint8_t* data, size_t size) = 0;
};

namespace detail {

class ImageAnnotationsSceneDecoder final : public ISceneDecoder {
 public:
  Expected<SceneFrame> decode(const uint8_t* data, size_t size) override {
    auto annotations = deserializeImageAnnotation(data, size);
    if (!annotations.has_value()) {
      return unexpected(std::move(annotations).error());
    }
    SceneFrame frame;
    frame.timestamp = annotations->timestamp;
    frame.annotations.push_back(std::move(*annotations));
    return frame;
  }
};

}  // namespace detail

[[nodiscard]] inline std::unique_ptr<ISceneDecoder> makeSceneDecoder(std::string_view schema_name) {
  if (schema_name == kSchemaImageAnnotations) {
    return std::make_unique<detail::ImageAnnotationsSceneDecoder>();
  }
  return nullptr;
}

}  // namespace PJ
