// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms.hpp"  // sdk::Pose
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_base/types.hpp"

namespace pj::scene3d {

/// A fully reconstructed occupancy grid at a point in time: the base keyframe's
/// placement (frame / origin / resolution / dims) with all applicable updates
/// applied. `cells` is row-major, `width * height` signed-8-bit values
/// (-1 unknown, 0..100 occupancy %); empty when there is no base grid yet.
struct ReconstructedGrid {
  PJ::Timestamp base_timestamp_ns = 0;  ///< Epoch keyframe timestamp (0 when empty).
  std::string frame_id;
  PJ::sdk::Pose origin;
  double resolution = 0.0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<int8_t> cells;

  [[nodiscard]] bool empty() const {
    return cells.empty();
  }
};

/// Axis-aligned rectangle of cells, for incremental texture upload.
struct CellRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Result of reconstructAt(): the grid plus how it changed, so the renderer can
/// pick a full vs. incremental texture upload without querying stashed state.
/// `grid` and `dirty` stay valid only until the next reconstructAt() call.
struct GridUpdate {
  /// How the grid changed since the previous reconstructAt().
  enum class Kind {
    Full,         ///< Whole grid rebuilt (new epoch or backward seek) — full upload.
    Incremental,  ///< Only `dirty` rects changed (forward seek) — partial upload.
    Empty,        ///< No base grid at this time — nothing to display.
  };

  const ReconstructedGrid& grid;
  Kind kind = Kind::Full;
  std::span<const CellRect> dirty;  ///< Changed rects; meaningful only for Incremental.
};

/// Reconstructs the displayed occupancy grid at an arbitrary time `t` from a base
/// keyframe stream plus an `OccupancyGridUpdate` delta stream, supporting forward
/// and backward time scrubbing.
///
/// Pure: no Qt / GL / ObjectStore dependency — the timeline is injected via two
/// std::function providers, so the reconstruction is unit-testable in isolation.
///
/// Correctness model:
///   displayed(t) = (latest base grid with ts <= t) + every update in
///                  (that base's ts, t], applied in ascending ts order.
/// `reconstructAt(t)` is order-independent: the result for a given `t` never
/// depends on the sequence of prior calls. Internally, forward scrubbing applies
/// only the new deltas; a backward / jump / new-epoch seek restores the nearest
/// snapshot (or the pristine base) and replays a bounded number of deltas. The
/// snapshot cache is a bounded-memory optimization ONLY — correctness always
/// falls back to base + full replay, so a cache bug can never corrupt the result.
class OccupancyGridReconstructor {
 public:
  /// Latest full grid with `ts <= t`, or nullopt if none exists yet.
  using BaseProvider = std::function<std::optional<PJ::sdk::OccupancyGrid>(PJ::Timestamp t)>;
  /// Updates with `lo < ts <= hi`, returned in ascending ts order.
  using UpdatesProvider = std::function<std::vector<PJ::sdk::OccupancyGridUpdate>(PJ::Timestamp lo, PJ::Timestamp hi)>;

  static constexpr std::size_t kDefaultSnapshotBudgetBytes = 64u * 1024u * 1024u;  // 64 MiB

  explicit OccupancyGridReconstructor(std::size_t snapshot_budget_bytes = kDefaultSnapshotBudgetBytes);

  /// Reconstruct the grid as displayed at `t` and describe how it changed. The
  /// returned grid/dirty references are valid until the next call.
  GridUpdate reconstructAt(PJ::Timestamp t, const BaseProvider& base_at, const UpdatesProvider& updates_in);

  /// The current grid (as of the last reconstructAt()); valid until the next call.
  [[nodiscard]] const ReconstructedGrid& grid() const {
    return grid_;
  }

  // --- Introspection (tests / diagnostics) ---
  [[nodiscard]] std::size_t snapshotCount() const {
    return snapshots_.size();
  }
  [[nodiscard]] std::size_t snapshotBytes() const;

 private:
  struct Snapshot {
    PJ::Timestamp ts = 0;
    std::vector<int8_t> cells;
  };

  void resetToBase(const PJ::sdk::OccupancyGrid& base);  // new epoch: clears snapshots
  void applyUpdate(const PJ::sdk::OccupancyGridUpdate& update);
  void applyRange(const UpdatesProvider& updates_in, PJ::Timestamp lo, PJ::Timestamp hi, bool allow_snapshots);
  void restoreNearestAtOrBefore(PJ::Timestamp t);  // backward seek within the epoch
  void pushSnapshot(PJ::Timestamp ts);
  [[nodiscard]] std::size_t gridBytes() const {
    return static_cast<std::size_t>(grid_.width) * grid_.height;
  }

  std::size_t snapshot_budget_bytes_;
  ReconstructedGrid grid_;
  std::vector<int8_t> base_cells_;  // pristine keyframe, retained for backward replay
  PJ::Timestamp last_t_ = 0;
  bool have_epoch_ = false;
  std::vector<Snapshot> snapshots_;  // ascending ts, excludes the base keyframe
  std::size_t updates_since_snapshot_ = 0;
  std::vector<CellRect> dirty_rects_;  // changed rects of the last reconstructAt()
};

}  // namespace pj::scene3d
