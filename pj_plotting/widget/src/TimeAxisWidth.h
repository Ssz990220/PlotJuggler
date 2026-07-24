#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cmath>
#include <limits>

namespace PJ::plotting_detail {

// The nominal minimum on-screen X width (seconds) for a TIME axis: 2 ns, the
// smallest window a rounded-integer-nanosecond saved viewport can represent as a
// non-degenerate range. Shared by PlotMagnifier's zoom clamp and PlotWidget's
// degenerate-range restore so both agree on the floor.
inline constexpr double kMinTimeXWidthSec = 2.0e-9;

// The smallest X window (seconds) that is guaranteed to remain a NON-DEGENERATE
// pair of distinct doubles when centered at `center` seconds.
//
// The 2 ns nominal floor is not representable near epoch scale: with "Use time
// offset" off, a time axis sits at ~1.6e9 s, where a double's spacing (ULP) is
// ~2.4e-7 s -- so `center +- 1 ns` collapses back to `center`. Take the max of the
// nominal floor and a few ULPs at `center` so the clamped/widened window always
// yields two distinct doubles regardless of where the axis sits. Four ULPs (not
// one) leaves headroom for the intermediate rounding in the magnifier's transform
// round-trip and the restore's ns->seconds conversion.
[[nodiscard]] inline double ulpAwareMinTimeXWidthSec(double center) {
  const double magnitude = std::abs(center);
  // ulp(x) = nextafter(x, +inf) - x; zero-safe (nextafter(0) is denormal-small).
  const double ulp = std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
  return std::max(kMinTimeXWidthSec, 4.0 * ulp);
}

}  // namespace PJ::plotting_detail
