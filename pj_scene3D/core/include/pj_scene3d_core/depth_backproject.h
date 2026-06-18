#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/types.hpp"                // PJ::Range
#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// Pinhole intrinsics for depth back-projection. fx/fy/cx/cy are in pixels at the
// resolution given by (width, height). depthToPoints rescales them when the depth
// image's own resolution differs from this calibration resolution — the common
// "CameraInfo calibrated at color resolution, paired with a smaller (aligned)
// depth image" case. Leave width/height at 0 when the intrinsics are already at
// the depth image's resolution (e.g. K embedded in the DepthImage itself).
struct DepthIntrinsics {
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  uint32_t width = 0;   ///< calibration width  (0 → same as the depth image)
  uint32_t height = 0;  ///< calibration height (0 → same as the depth image)

  /// Usable only when both focal lengths are positive; a zero/unset K is invalid.
  [[nodiscard]] bool valid() const {
    return fx > 0.0 && fy > 0.0;
  }
};

// Extract intrinsics from a row-major 3x3 K (K[0]=fx, K[4]=fy, K[2]=cx, K[5]=cy)
// calibrated at (width, height). The result is invalid() when K's focal lengths
// are non-positive (an unset/zero K), so callers can branch on .valid().
[[nodiscard]] DepthIntrinsics intrinsicsFromK(const std::array<double, 9>& k, uint32_t width, uint32_t height);

struct BackprojectOptions {
  float min_depth_m = 0.0f;  ///< drop z <= this (0 keeps every z > 0; 0 = "no return" sentinel)
  float max_depth_m = 0.0f;  ///< drop z > this when > 0 (0 = no far clip)
  uint32_t stride = 1;       ///< decimation: keep every Nth column AND row (clamped to >= 1)
};

// Back-project a DepthImage into 3D points in the camera OPTICAL frame
// (REP-103 / OpenCV convention: +X right, +Y down, +Z forward), one point per
// kept pixel. A pixel is dropped when its depth is <= min_depth_m, non-finite,
// or (when max_depth_m > 0) greater than max_depth_m.
//
// Supported encodings: "16UC1" (uint16 little-endian millimetres) and "32FC1"
// (float32 little-endian metres). Returns an EMPTY vector for any other encoding,
// an invalid `intr`, or a `depth.data` buffer smaller than width*height*bytes.
//
// When `scalar_out` is non-null it is cleared and refilled in lockstep with the
// returned positions, each entry holding that point's depth in metres — the
// scalar the depth colormap consumes. (Always parallel to the return value, so
// positions[i] corresponds to (*scalar_out)[i].)
//
// `bounds_out` and `scalar_range_out` are accumulated in the SAME pass that builds
// the points, so a caller needs no extra O(N) sweeps for the AABB or the colormap
// range. When non-null:
//   - `*bounds_out` is the AABB of the kept points (`valid == false` if none).
//   - `*scalar_range_out` is the {min, max} kept depth, ready for the colormap:
//     {0, 1} when no point is kept, and widened to {lo, lo+1} when the spread is
//     under 1e-6 so a flat scene still maps across the colormap.
[[nodiscard]] std::vector<glm::vec3> depthToPoints(
    const PJ::sdk::DepthImage& depth, const DepthIntrinsics& intr, const BackprojectOptions& opts,
    std::vector<float>* scalar_out = nullptr, AABB* bounds_out = nullptr, PJ::Range<float>* scalar_range_out = nullptr);

}  // namespace pj::scene3d
