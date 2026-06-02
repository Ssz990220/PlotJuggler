// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <vector>

namespace pj::scene3d {

// A closed [first, last] timestamp range (absolute ns) covered by one entity.
struct EntityTimeRange {
  int64_t first;
  int64_t last;
};

// Clamp a tracker time to the span covered by a set of entity time ranges.
//
// The latched-grid subtlety this exists to get right:
//   - Inverted ranges (last < first) are ignored.
//   - A *spanning* entity (first < last) defines both the lower and the upper
//     bound — the tracker can't usefully sit outside the data it has.
//   - A *latched / one-shot* entity (first == last, e.g. a map pinned to the
//     recording's first timestamp) is valid from its timestamp ONWARD, not only
//     at that single instant. So it lowers `lo` — which lets the slider MINIMUM
//     snap onto its exact ns (a double→ns playhead lands a few hundred ns short
//     otherwise and `latestAt` misses the grid) — but it must NOT cap `hi`. If
//     it did, its lone early timestamp would drag the live playhead backwards,
//     hiding everything keyed to "now" (TF axes, live grids). This was the
//     "TF doesn't render unless another topic is present" bug.
//   - If no spanning entity exists, there is no upper bound: the time is only
//     clamped up to `lo`.
//   - With no usable range, the time passes through unchanged.
inline int64_t clampTrackerTimeToRanges(int64_t time_ns, const std::vector<EntityTimeRange>& ranges) {
  int64_t lo = 0;
  int64_t hi = 0;
  bool have_lo = false;
  bool have_hi = false;
  for (const auto& r : ranges) {
    if (r.last < r.first) {
      continue;  // skip inverted ranges
    }
    if (!have_lo || r.first < lo) {
      lo = r.first;
      have_lo = true;
    }
    // Only entities with a real span bound the top; a zero-span (latched) entity
    // deliberately does not, so it can't pull the playhead back to its stamp.
    if (r.last > r.first && (!have_hi || r.last > hi)) {
      hi = r.last;
      have_hi = true;
    }
  }
  if (!have_lo) {
    return time_ns;
  }
  if (time_ns < lo) {
    return lo;
  }
  if (have_hi && time_ns > hi) {
    return hi;
  }
  return time_ns;
}

}  // namespace pj::scene3d
