// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/video_color.h"

namespace PJ {

std::array<float, 16> buildYuvMatrix(YuvColorSpace space, YuvColorRange range) noexcept {
  // Luma coefficients (Kr, Kb); Kg = 1 - Kr - Kb.
  const double kr = (space == YuvColorSpace::kBt601) ? 0.299 : 0.2126;
  const double kb = (space == YuvColorSpace::kBt601) ? 0.114 : 0.0722;
  const double kg = 1.0 - kr - kb;

  // Full-range chroma → RGB coefficients (applied to centred chroma, V-0.5 / U-0.5).
  const double rv = 2.0 * (1.0 - kr);             // R from V
  const double bu = 2.0 * (1.0 - kb);             // B from U
  const double gu = -2.0 * kb * (1.0 - kb) / kg;  // G from U
  const double gv = -2.0 * kr * (1.0 - kr) / kg;  // G from V

  // Range scale + offset. Limited (studio/TV) range maps luma 16..235 and chroma
  // 16..240 onto 0..1; full range is identity with chroma centred at 0.5.
  const bool full = (range == YuvColorRange::kFull);
  const double y_scale = full ? 1.0 : 255.0 / 219.0;
  const double c_scale = full ? 1.0 : 255.0 / 224.0;
  const double y_off = full ? 0.0 : 16.0 / 255.0;
  const double c_off = full ? 0.5 : 128.0 / 255.0;

  // The shader feeds (u - 0.5, v - 0.5); the true centre is c_off, so fold the
  // (0.5 - c_off) correction into the constant column along with the luma offset.
  const double c_delta = 0.5 - c_off;
  const double off_r = -y_scale * y_off + rv * c_scale * c_delta;
  const double off_g = -y_scale * y_off + (gu + gv) * c_scale * c_delta;
  const double off_b = -y_scale * y_off + bu * c_scale * c_delta;

  const auto f = [](double v) { return static_cast<float>(v); };
  // Column-major: col0 ×Y, col1 ×(U-0.5), col2 ×(V-0.5), col3 ×1 (constant).
  return {
      // col0 (Y)
      f(y_scale),
      f(y_scale),
      f(y_scale),
      0.0f,
      // col1 (U - 0.5)
      0.0f,
      f(gu * c_scale),
      f(bu * c_scale),
      0.0f,
      // col2 (V - 0.5)
      f(rv * c_scale),
      f(gv * c_scale),
      0.0f,
      0.0f,
      // col3 (constant offset)
      f(off_r),
      f(off_g),
      f(off_b),
      1.0f,
  };
}

}  // namespace PJ
