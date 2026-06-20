// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace pj::scene3d {

// Per-voxel draw predicate, evaluated on the RAW scalar value (data units, not
// normalized). The VoxelGrid vertex shader mirrors voxelShouldDraw() exactly, so
// these headless tests pin the GPU's culling behaviour. The schema does not mark
// "occupied" cells — this viewer-side predicate is what decides visibility.
enum class VoxelDrawMode : uint8_t {
  kAll,        ///< Draw every voxel.
  kNonZero,    ///< Draw where value != 0 (occupancy/semantic default).
  kThreshold,  ///< Draw where value >= threshold (costmaps/ESDF).
  kRange,      ///< Draw where range_lo <= value <= range_hi.
};

// True when a voxel with raw scalar `value` should be drawn under `mode`.
// `threshold` is used only by kThreshold; `range_lo`/`range_hi` only by kRange.
[[nodiscard]] bool voxelShouldDraw(VoxelDrawMode mode, float value, float threshold, float range_lo, float range_hi);

// Normalize a raw value into [0,1] for colormap lookup. A non-positive or
// non-finite span returns 0 (the colormap's low end), never NaN/Inf.
[[nodiscard]] float voxelNormalize(float value, float range_lo, float range_hi);

}  // namespace pj::scene3d
