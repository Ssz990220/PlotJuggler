#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pj_base/builtin/image_annotations_codec.hpp"
#include "pj_scene2d_core/scene_frame.h"

namespace PJ {

// Singular-named wrappers around the canonical (plural) SDK codec. These
// exist purely so consumers in this module can keep their existing spelling.
[[nodiscard]] inline std::vector<uint8_t> serializeImageAnnotation(const ImageAnnotation& ia) {
  return serializeImageAnnotations(ia);
}

[[nodiscard]] inline Expected<ImageAnnotation> deserializeImageAnnotation(const uint8_t* data, size_t size) {
  return deserializeImageAnnotations(data, size);
}

}  // namespace PJ
