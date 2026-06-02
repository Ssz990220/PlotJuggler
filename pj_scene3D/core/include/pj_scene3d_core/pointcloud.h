#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace pj::scene3d {

// Pure render struct: the decoded geometry a render pass consumes. Field-layout
// metadata (name/offset/datatype) belongs to the canonical sdk::PointCloud and is
// read straight from it by the decode adapter, so nothing wire-shaped lives here.
struct DecodedPointCloud {
  std::chrono::nanoseconds stamp{0};
  std::string frame_id;
  std::vector<glm::vec3> positions;  // one entry per point
  std::vector<float> scalar;         // colorize-by-field values; empty when no field is selected
  std::string scalar_field_name;     // canonical field `scalar` was extracted from
};

}  // namespace pj::scene3d
