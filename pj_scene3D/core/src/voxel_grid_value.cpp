// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/voxel_grid_value.h"

namespace pj::scene3d {

bool voxelShouldDraw(VoxelDrawMode mode, float value, float threshold, float range_lo, float range_hi) {
  switch (mode) {
    case VoxelDrawMode::kAll:
      return true;
    case VoxelDrawMode::kNonZero:
      return value != 0.0f;
    case VoxelDrawMode::kThreshold:
      return value >= threshold;
    case VoxelDrawMode::kRange:
      return value >= range_lo && value <= range_hi;
  }
  return true;
}

float voxelNormalize(float value, float range_lo, float range_hi) {
  const float span = range_hi - range_lo;
  // `!(span > 0)` rejects a zero/negative span AND a NaN span (any comparison
  // with NaN is false), so a degenerate or non-finite range collapses to 0.
  if (!(span > 0.0f)) {
    return 0.0f;
  }
  const float t = (value - range_lo) / span;
  if (t < 0.0f) {
    return 0.0f;
  }
  if (t > 1.0f) {
    return 1.0f;
  }
  return t;
}

}  // namespace pj::scene3d
