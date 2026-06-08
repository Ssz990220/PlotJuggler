#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// PJ4's canonical time vocabulary: the one place PJ4-owned code names time. It
// sits just above the FROZEN int64-ns spine (PJ::Timestamp, in the read-only
// plotjuggler_sdk submodule) and converts at the named seams below — the spine
// stays int64 across storage/ABI/wire, chrono lives only above it. Three
// concepts, kept un-mixable by the type system:
//   * Timepoint      — absolute instant (sys_time<nanoseconds>, Unix epoch).
//   * Duration       — a span (nanoseconds); Timepoint + Timepoint won't compile.
//   * DisplaySeconds — display-relative seconds (raw - display_offset) the Qwt
//                      axis / PlaybackEngine speak; a strong double, so absolute
//                      seconds can't masquerade as a display coordinate.
//
// DisplaySeconds wraps a double (not an opaque enum) so display-axis math stays
// natural; the safety lives at toDisplaySeconds()/rawToDisplaySeconds(), which
// DEMAND a DisplayOffset so the raw->display subtraction can't be skipped.

#include <chrono>
#include <compare>

#include "pj_base/dataset.hpp"     // PJ::TimeDomain (display_offset)
#include "pj_base/types.hpp"       // PJ::Timestamp, PJ::Range
#include "pj_runtime/constants.h"  // PJ::kNanosecondsPerSecond (single source of truth)

namespace PJ {

/// An ABSOLUTE wall-clock instant, nanosecond precision, Unix epoch. Lossless
/// mirror of PJ::Timestamp (C++20 guarantees system_clock's epoch == Unix epoch).
using Timepoint = std::chrono::sys_time<std::chrono::nanoseconds>;

/// A length of time (a span, not a point): retention windows, lifetimes, deltas.
using Duration = std::chrono::nanoseconds;

/// The per-dataset display shift carried as a strong-named Duration, so call
/// sites read "this is the subtrahend that turns a Timepoint into display time"
/// (TimeDomain::display_offset). It IS a Duration; the distinct name documents
/// intent without inventing a third arithmetic type.
struct DisplayOffset {
  Duration value{0};
};

/// DISPLAY-RELATIVE time in seconds: the Qwt-axis / PlaybackEngine coordinate.
/// Strong wrapper over double so it cannot be confused with an absolute-seconds
/// double. Arithmetic is intentionally narrow: a difference of two coordinates
/// is a bare seconds delta (no origin); a coordinate may be shifted by a delta.
struct DisplaySeconds {
  double value = 0.0;

  explicit constexpr DisplaySeconds(double seconds = 0.0) noexcept : value(seconds) {}

  friend constexpr bool operator==(const DisplaySeconds&, const DisplaySeconds&) = default;
  friend constexpr auto operator<=>(const DisplaySeconds&, const DisplaySeconds&) = default;

  /// Difference of two display coordinates: a span in seconds (no origin), so a
  /// plain double — never a DisplaySeconds.
  friend constexpr double operator-(DisplaySeconds a, DisplaySeconds b) noexcept {
    return a.value - b.value;
  }
  /// Shift a display coordinate by a seconds delta (e.g. a tracker step).
  friend constexpr DisplaySeconds operator+(DisplaySeconds a, double delta_sec) noexcept {
    return DisplaySeconds{a.value + delta_sec};
  }
  friend constexpr DisplaySeconds operator-(DisplaySeconds a, double delta_sec) noexcept {
    return DisplaySeconds{a.value - delta_sec};
  }
};

/// Display interval the PlaybackEngine range speaks (reuses PJ::Range, not pair).
using DisplayRange = PJ::Range<DisplaySeconds>;

/// Wrap a display-relative seconds value without implying it came from Qwt.
[[nodiscard]] constexpr DisplaySeconds displaySeconds(double seconds) noexcept {
  return DisplaySeconds{seconds};
}

/// Build a display-relative seconds interval without spelling out the wrapper at
/// each app/UI call site.
[[nodiscard]] constexpr DisplayRange displayRange(double min_seconds, double max_seconds) noexcept {
  return DisplayRange{displaySeconds(min_seconds), displaySeconds(max_seconds)};
}

// --- absolute spine seam: int64-ns <-> Timepoint (apply on the PJ4 side only) ---

/// Lift an int64-ns PJ::Timestamp out of the frozen spine into a Timepoint.
[[nodiscard]] constexpr Timepoint fromRaw(Timestamp ns) noexcept {
  return Timepoint{Duration{ns}};
}

/// Lower a Timepoint back to the int64-ns spine, immediately before crossing a
/// frozen storage/ABI boundary (DataWriter, the C-ABI trampolines, the codecs).
[[nodiscard]] constexpr Timestamp toRaw(Timepoint t) noexcept {
  return t.time_since_epoch().count();
}

/// Lift a frozen int64 interval into an absolute Timepoint interval (reuses
/// PJ::Range, never std::pair).
[[nodiscard]] constexpr Range<Timepoint> fromRawRange(const Range<Timestamp>& r) noexcept {
  return {fromRaw(r.min), fromRaw(r.max)};
}

// --- display seam: the offset is MANDATORY, so raw->display can't be skipped ---

/// Build the per-dataset display shift from the engine's TimeDomain.
[[nodiscard]] constexpr DisplayOffset offsetOf(const TimeDomain& domain) noexcept {
  return DisplayOffset{Duration{domain.display_offset}};
}

/// Absolute Timepoint -> display-relative seconds. THE single named place the
/// display_offset subtraction happens (display_time = raw_time - display_offset).
/// The DisplayOffset argument is required, so a caller cannot forget it.
[[nodiscard]] constexpr DisplaySeconds toDisplaySeconds(Timepoint absolute, DisplayOffset offset) noexcept {
  const Duration display_ns = absolute.time_since_epoch() - offset.value;
  return DisplaySeconds{std::chrono::duration<double>(display_ns).count()};
}

/// Display-relative seconds -> absolute Timepoint (inverse of toDisplaySeconds).
/// Rounds to the nearest nanosecond (not truncate) so the round-trip is stable.
[[nodiscard]] constexpr Timepoint toAbsolute(DisplaySeconds seconds, DisplayOffset offset) noexcept {
  const auto display_ns = std::chrono::round<Duration>(std::chrono::duration<double>(seconds.value));
  return Timepoint{display_ns + offset.value};
}

/// Raw int64-ns -> display-relative seconds, for call sites holding a Timestamp
/// + TimeDomain. Centralizes the (raw - offset)/1e9 idiom in one place.
[[nodiscard]] constexpr DisplaySeconds rawToDisplaySeconds(Timestamp raw_ns, DisplayOffset offset) noexcept {
  return toDisplaySeconds(fromRaw(raw_ns), offset);
}

/// Display-relative seconds -> raw int64-ns (inverse of rawToDisplaySeconds).
[[nodiscard]] constexpr Timestamp displaySecondsToRaw(DisplaySeconds seconds, DisplayOffset offset) noexcept {
  return toRaw(toAbsolute(seconds, offset));
}

// --- Qwt / IDataWidget edge: DisplaySeconds <-> bare double (axis stays double) ---

/// Unwrap to the bare display-seconds double the Qwt axis / IDataWidget expect.
[[nodiscard]] constexpr double toAxisDouble(DisplaySeconds seconds) noexcept {
  return seconds.value;
}

/// Wrap a display-seconds double arriving from the Qwt/IDataWidget edge — the
/// only legal door for an axis double into the typed world, so it can never be
/// mistaken for an absolute-seconds value.
[[nodiscard]] constexpr DisplaySeconds fromAxisDouble(double display_sec) noexcept {
  return displaySeconds(display_sec);
}

}  // namespace PJ
