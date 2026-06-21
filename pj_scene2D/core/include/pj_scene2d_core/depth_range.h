#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"  // PJ::Range

namespace PJ {

/// Compute a robust [near, far] metric-depth range from a raw float32 depth buffer
/// by taking the `lo_frac`/`hi_frac` percentiles of the *valid* samples (finite and
/// > 0; the 0 no-data sentinel and NaN/inf are ignored). This is what the depth
/// layer's "Auto-fit" action uses: a percentile range is robust to the speckle
/// outliers that made a naive per-frame min/max flicker.
///
/// Returns nullopt when there are no valid samples. When the two percentiles
/// coincide (e.g. a flat depth image) the upper bound is nudged up so the range is
/// never degenerate (the shader divides by far-near).
///
/// `lo_frac`/`hi_frac` are clamped to [0,1] and ordered; `count` is the number of
/// floats `data` points to. The returned Range has `min` = near, `max` = far.
[[nodiscard]] inline std::optional<Range<float>> depthPercentileRange(
    const float* data, std::size_t count, float lo_frac = 0.02f, float hi_frac = 0.98f) {
  if (data == nullptr || count == 0) {
    return std::nullopt;
  }
  std::vector<float> valid;
  valid.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float v = data[i];
    if (std::isfinite(v) && v > 0.0f) {
      valid.push_back(v);
    }
  }
  if (valid.empty()) {
    return std::nullopt;
  }

  float lo = std::clamp(lo_frac, 0.0f, 1.0f);
  float hi = std::clamp(hi_frac, 0.0f, 1.0f);
  if (hi < lo) {
    std::swap(lo, hi);
  }

  const auto n = valid.size();
  const auto pick = [&](float frac) -> float {
    const auto idx = static_cast<std::size_t>(std::llround(frac * static_cast<double>(n - 1)));
    auto it = valid.begin() + static_cast<std::ptrdiff_t>(std::min(idx, n - 1));
    std::nth_element(valid.begin(), it, valid.end());
    return *it;
  };

  float near_m = pick(lo);
  float far_m = pick(hi);  // valid is now partially reordered; pick() re-partitions, which is fine.
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  if (!(far_m > near_m)) {
    far_m = near_m + std::max(1e-3f, std::abs(near_m) * 0.01f);
  }
  return Range<float>{near_m, far_m};
}

}  // namespace PJ
