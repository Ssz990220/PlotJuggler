// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"

#include <algorithm>
#include <cstring>

namespace pj::scene3d {

namespace {
// Updates applied between successive snapshots (subject to the memory budget,
// which decimates the cache when exceeded). Tunes backward-seek replay cost vs.
// snapshot memory; correctness is independent of this value.
constexpr std::size_t kSnapshotStride = 64;
}  // namespace

OccupancyGridReconstructor::OccupancyGridReconstructor(std::size_t snapshot_budget_bytes)
    : snapshot_budget_bytes_(snapshot_budget_bytes) {}

std::size_t OccupancyGridReconstructor::snapshotBytes() const {
  std::size_t total = 0;
  for (const auto& s : snapshots_) {
    total += s.cells.size();
  }
  return total;
}

void OccupancyGridReconstructor::resetToBase(const PJ::sdk::OccupancyGrid& base) {
  grid_.base_timestamp_ns = base.timestamp_ns;
  grid_.frame_id = base.frame_id;
  grid_.origin = base.origin;
  grid_.resolution = base.resolution;
  grid_.width = base.width;
  grid_.height = base.height;

  const std::size_t n = gridBytes();
  grid_.cells.assign(n, static_cast<int8_t>(-1));  // unknown by default if data is short
  const std::size_t copy_n = std::min(n, base.data.size());
  if (copy_n > 0) {
    std::memcpy(grid_.cells.data(), base.data.data(), copy_n);  // uint8 → int8, bit-identical
  }
  base_cells_ = grid_.cells;  // retain pristine keyframe for backward replay

  snapshots_.clear();
  updates_since_snapshot_ = 0;
  have_epoch_ = true;
  last_t_ = base.timestamp_ns;
}

void OccupancyGridReconstructor::applyUpdate(const PJ::sdk::OccupancyGridUpdate& update) {
  if (grid_.width == 0 || grid_.height == 0 || update.width == 0 || update.height == 0) {
    return;
  }
  // Guard against a malformed patch whose data is shorter than width*height.
  const std::size_t patch_cells = static_cast<std::size_t>(update.width) * update.height;
  if (update.data.size() < patch_cells) {
    return;
  }

  const int64_t gw = grid_.width;
  const int64_t gh = grid_.height;
  // Clamp the patch rectangle to the grid bounds.
  const int64_t x0 = std::max<int64_t>(0, update.x);
  const int64_t y0 = std::max<int64_t>(0, update.y);
  const int64_t x1 = std::min<int64_t>(gw, static_cast<int64_t>(update.x) + update.width);
  const int64_t y1 = std::min<int64_t>(gh, static_cast<int64_t>(update.y) + update.height);
  if (x1 <= x0 || y1 <= y0) {
    return;  // fully out of bounds
  }
  const int64_t copy_w = x1 - x0;

  for (int64_t y = y0; y < y1; ++y) {
    const int64_t src_row = y - update.y;   // row within the patch
    const int64_t src_col = x0 - update.x;  // column within the patch
    const uint8_t* src = update.data.data() + static_cast<std::size_t>(src_row * update.width + src_col);
    int8_t* dst = grid_.cells.data() + static_cast<std::size_t>(y * gw + x0);
    std::memcpy(dst, src, static_cast<std::size_t>(copy_w));
  }

  dirty_rects_.push_back(
      CellRect{
          static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(copy_w),
          static_cast<uint32_t>(y1 - y0)});
}

void OccupancyGridReconstructor::pushSnapshot(PJ::Timestamp ts) {
  snapshots_.push_back(Snapshot{ts, grid_.cells});

  // Honor the memory budget: while over budget, decimate (drop every other
  // snapshot) so the survivors stay evenly spread across the timeline rather
  // than clustering — keeping worst-case backward-replay distance bounded.
  const std::size_t per = gridBytes();
  if (per == 0) {
    return;
  }
  while (snapshotBytes() > snapshot_budget_bytes_ && snapshots_.size() > 1) {
    std::vector<Snapshot> kept;
    kept.reserve(snapshots_.size() / 2 + 1);
    for (std::size_t i = 0; i < snapshots_.size(); i += 2) {
      kept.push_back(std::move(snapshots_[i]));
    }
    snapshots_ = std::move(kept);
  }
}

void OccupancyGridReconstructor::applyRange(
    const UpdatesProvider& updates_in, PJ::Timestamp lo, PJ::Timestamp hi, bool allow_snapshots) {
  if (hi <= lo) {
    return;
  }
  const std::vector<PJ::sdk::OccupancyGridUpdate> ups = updates_in(lo, hi);
  for (const auto& u : ups) {
    applyUpdate(u);
    if (allow_snapshots) {
      if (++updates_since_snapshot_ >= kSnapshotStride) {
        pushSnapshot(u.timestamp_ns);
        updates_since_snapshot_ = 0;
      }
    }
  }
}

void OccupancyGridReconstructor::restoreNearestAtOrBefore(PJ::Timestamp t) {
  // Largest-ts snapshot with ts <= t, else the pristine base keyframe.
  const Snapshot* best = nullptr;
  for (const auto& s : snapshots_) {
    if (s.ts <= t && (best == nullptr || s.ts > best->ts)) {
      best = &s;
    }
  }
  if (best != nullptr) {
    grid_.cells = best->cells;
    last_t_ = best->ts;
  } else {
    grid_.cells = base_cells_;
    last_t_ = grid_.base_timestamp_ns;
  }
}

GridUpdate OccupancyGridReconstructor::reconstructAt(
    PJ::Timestamp t, const BaseProvider& base_at, const UpdatesProvider& updates_in) {
  dirty_rects_.clear();

  const std::optional<PJ::sdk::OccupancyGrid> base = base_at(t);
  if (!base) {
    // No base grid at or before t — nothing to display.
    grid_ = ReconstructedGrid{};
    base_cells_.clear();
    snapshots_.clear();
    have_epoch_ = false;
    last_t_ = 0;
    return GridUpdate{grid_, GridUpdate::Kind::Empty, {}};
  }

  if (!have_epoch_ || base->timestamp_ns != grid_.base_timestamp_ns) {
    // New epoch: a different (or first) base keyframe is now in effect.
    resetToBase(*base);
    applyRange(updates_in, grid_.base_timestamp_ns, t, /*allow_snapshots=*/true);
    last_t_ = t;
    return GridUpdate{grid_, GridUpdate::Kind::Full, dirty_rects_};
  }

  if (t >= last_t_) {
    // Forward within the epoch: apply only the new deltas incrementally.
    applyRange(updates_in, last_t_, t, /*allow_snapshots=*/true);
    last_t_ = t;
    return GridUpdate{grid_, GridUpdate::Kind::Incremental, dirty_rects_};
  }

  // Backward within the epoch: restore the nearest snapshot (or base) and replay.
  restoreNearestAtOrBefore(t);
  applyRange(updates_in, last_t_, t, /*allow_snapshots=*/false);
  last_t_ = t;
  return GridUpdate{grid_, GridUpdate::Kind::Full, dirty_rects_};
}

}  // namespace pj::scene3d
