#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
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

/// Precomputed bilinear sampling table for the fast CPU fallback rectifier.
///
/// Derived once from an `UndistortMap`, it hoists the per-frame `floor()` and
/// bounds math (the bottleneck of the scalar rectifier) out of the inner loop:
/// each output pixel stores the linear index of its top-left source tap plus the
/// two bilinear fractions. Sampling a frame then costs four multiply-adds per
/// channel and no transcendental math. `src_width`/`src_height` are the source
/// size the taps index into — a frame of a different size needs a rebuilt table.
struct UndistortMapFast {
  int out_width = 0;
  int out_height = 0;
  int src_width = 0;
  int src_height = 0;
  std::vector<int32_t> src_p0;  ///< Linear source pixel index of the top-left bilinear tap; -1 = out of bounds (black).
  std::vector<float> frac_x;    ///< Horizontal bilinear fraction in [0,1).
  std::vector<float> frac_y;    ///< Vertical bilinear fraction in [0,1).

  [[nodiscard]] bool valid() const noexcept {
    const auto n = static_cast<size_t>(out_width) * static_cast<size_t>(out_height);
    return out_width > 0 && out_height > 0 && src_p0.size() == n && frac_x.size() == n && frac_y.size() == n;
  }
};

/// Build the fast sampling table from a float `UndistortMap`. The result is
/// bit-for-bit equivalent to what `rectifyFrame` computes per frame; an invalid
/// or zero-source map yields an invalid table (caller treats it as "do not
/// rectify").
[[nodiscard]] UndistortMapFast buildFastRectifyMap(const UndistortMap& map);

/// Pack an `UndistortMap` into a GPU lookup-texture payload: `out_w*out_h` RG
/// pairs (row-major) giving, per output pixel, the SOURCE sample point normalized
/// to [0,1] texture space — with the +0.5 half-texel offset that makes a
/// `GL_LINEAR` sampler reproduce the CPU bilinear. Out-of-bounds output pixels
/// (the same bound check `rectifyFrame` uses) are written as the sentinel
/// `(-1,-1)` so the fragment shader can render them black. Empty if `map` is
/// invalid.
[[nodiscard]] std::vector<float> undistortMapToNormalizedRG(const UndistortMap& map);

}  // namespace PJ
