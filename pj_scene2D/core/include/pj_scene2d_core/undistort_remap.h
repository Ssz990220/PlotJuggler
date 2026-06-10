#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string_view>
#include <vector>

#include "pj_base/builtin/camera_info.hpp"

namespace PJ {

/// Reverse sampling map for image rectification (lens undistortion).
///
/// For every pixel of the *output* (rectified) image — laid out at
/// `out_width x out_height` — it stores the floating-point coordinates of the
/// *source* (raw, distorted) pixel that feeds it, already expressed in the
/// SOURCE image's pixel space. A rectifier then bilinearly samples the source at
/// `(src_x, src_y)` for every output pixel.
///
/// Built once per camera from its `CameraInfo` (intrinsics K, distortion D,
/// rectification R, projection P) and reused for every frame of that camera,
/// since the calibration is constant in time.
struct UndistortMap {
  int out_width = 0;
  int out_height = 0;
  int src_width = 0;         ///< Decoded source width the map's sampling coords were scaled for.
  int src_height = 0;        ///< Decoded source height. A frame at a different size needs a rebuilt map.
  std::vector<float> src_x;  ///< size out_width*out_height; source column per output pixel.
  std::vector<float> src_y;  ///< size out_width*out_height; source row per output pixel.

  [[nodiscard]] bool valid() const noexcept {
    const auto n = static_cast<size_t>(out_width) * static_cast<size_t>(out_height);
    return out_width > 0 && out_height > 0 && src_x.size() == n && src_y.size() == n;
  }
};

/// True when `ci` carries usable pinhole intrinsics (fx, fy nonzero) and a
/// nonzero native resolution. The distortion model may be empty / all-zero, in
/// which case the map degenerates to a pure resolution rescale (still useful:
/// it lifts a downsampled image to the calibrated resolution so annotations
/// authored there line up).
[[nodiscard]] bool isRectifiable(const sdk::CameraInfo& ci) noexcept;

/// Build the reverse rectification map for a camera.
///
/// @param ci     Calibration; K/D/R/P are defined at `ci.width x ci.height`.
/// @param src_w  Width of the actual decoded source image. It may be downsampled
///               relative to `ci.width` (Waymo ships 480-wide JPEGs against
///               1920-wide calibration); source sampling coords are scaled to it.
/// @param src_h  Height of the decoded source image.
/// @param out_w  Output (rectified) width — pass `ci.width` for native output.
/// @param out_h  Output (rectified) height — pass `ci.height` for native output.
/// @returns a `valid()` map, or an empty (invalid) map when `ci` lacks usable
///          intrinsics — callers treat an invalid map as "do not rectify".
[[nodiscard]] UndistortMap computeUndistortMap(const sdk::CameraInfo& ci, int src_w, int src_h, int out_w, int out_h);

}  // namespace PJ
