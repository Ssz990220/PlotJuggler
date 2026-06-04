#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>

namespace PJ {

// Seconds <-> nanoseconds conversion factor for the IDataWidget boundary
// (tracker time travels as seconds in a bare double; storage timestamps are
// int64 nanoseconds). Derived from std::chrono so the ratio can never drift
// from the time types it converts between. Prefer converting through
// std::chrono::duration directly where practical (it adds nothing at -O2);
// use this constant where a plain multiplier/divisor reads better.
inline constexpr double kNanosecondsPerSecond = static_cast<double>(std::chrono::nanoseconds::period::den);

}  // namespace PJ
