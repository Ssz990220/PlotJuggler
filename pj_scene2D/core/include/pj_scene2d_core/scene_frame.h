#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <vector>

#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/types.hpp"

namespace PJ {

// Compatibility aliases. Existing pj_scene2D consumers spell these in the
// singular ("ImageAnnotation"); the canonical SDK exports them in the plural
// ("ImageAnnotations"). Keep both readable so the canonical-object refactor
// does not also force a global rename in this module.
using AnnotationTopology = sdk::AnnotationTopology;
using CircleAnnotation = sdk::CircleAnnotation;
using ColorRGBA = sdk::ColorRGBA;
using ImageAnnotation = sdk::ImageAnnotations;
using Point2 = sdk::Point2;
using PointsAnnotation = sdk::PointsAnnotation;
using TextAnnotation = sdk::TextAnnotation;

// Time-stamped batch of image-overlay annotations produced by an
// ISceneDecoder. Carried as a frame on the media pipeline.
struct SceneFrame {
  Timestamp timestamp = 0;
  std::vector<ImageAnnotation> annotations;
  bool operator==(const SceneFrame&) const = default;

  [[nodiscard]] bool empty() const noexcept {
    return annotations.empty();
  }
};

}  // namespace PJ
