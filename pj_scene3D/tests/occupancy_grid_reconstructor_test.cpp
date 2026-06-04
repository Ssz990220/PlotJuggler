// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "pj_base/span.hpp"

namespace PJ {
namespace {

using pj::scene3d::GridUpdate;
using pj::scene3d::OccupancyGridReconstructor;
using pj::scene3d::ReconstructedGrid;

// --- Builders: own the cell bytes via a shared_ptr anchor so the Span stays valid. ---

sdk::OccupancyGrid makeBase(Timestamp ts, uint32_t w, uint32_t h, std::vector<uint8_t> bytes) {
  auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  sdk::OccupancyGrid g;
  g.timestamp_ns = ts;
  g.frame_id = "map";
  g.resolution = 0.05;
  g.width = w;
  g.height = h;
  g.data = Span<const uint8_t>(buf->data(), buf->size());
  g.anchor = buf;
  return g;
}

sdk::OccupancyGridUpdate makeUpdate(
    Timestamp ts, int32_t x, int32_t y, uint32_t w, uint32_t h, std::vector<uint8_t> bytes) {
  auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  sdk::OccupancyGridUpdate u;
  u.timestamp_ns = ts;
  u.frame_id = "map";
  u.x = x;
  u.y = y;
  u.width = w;
  u.height = h;
  u.data = Span<const uint8_t>(buf->data(), buf->size());
  u.anchor = buf;
  return u;
}

// --- Independent reference: cell-by-cell replay (different code path than the
//     reconstructor's row-memcpy), so equivalence is a genuine cross-check. ---

std::vector<int8_t> refReplay(
    const sdk::OccupancyGrid& base, const std::vector<sdk::OccupancyGridUpdate>& updates, Timestamp t) {
  const int64_t gw = base.width;
  const int64_t gh = base.height;
  std::vector<int8_t> cells(static_cast<std::size_t>(gw * gh), static_cast<int8_t>(-1));
  const std::size_t copy_n = std::min(cells.size(), base.data.size());
  for (std::size_t i = 0; i < copy_n; ++i) {
    cells[i] = static_cast<int8_t>(base.data.data()[i]);
  }
  for (const auto& u : updates) {
    if (u.timestamp_ns <= base.timestamp_ns || u.timestamp_ns > t) {
      continue;
    }
    for (uint32_t r = 0; r < u.height; ++r) {
      for (uint32_t c = 0; c < u.width; ++c) {
        const int64_t y = static_cast<int64_t>(u.y) + r;
        const int64_t x = static_cast<int64_t>(u.x) + c;
        if (x < 0 || y < 0 || x >= gw || y >= gh) {
          continue;
        }
        cells[static_cast<std::size_t>(y * gw + x)] =
            static_cast<int8_t>(u.data.data()[static_cast<std::size_t>(r) * u.width + c]);
      }
    }
  }
  return cells;
}

// --- Providers over in-memory timelines. ---

OccupancyGridReconstructor::BaseProvider baseProviderFor(const std::vector<sdk::OccupancyGrid>& bases) {
  return [&bases](Timestamp t) -> std::optional<sdk::OccupancyGrid> {
    std::optional<sdk::OccupancyGrid> best;
    for (const auto& b : bases) {
      if (b.timestamp_ns <= t && (!best || b.timestamp_ns > best->timestamp_ns)) {
        best = b;
      }
    }
    return best;
  };
}

OccupancyGridReconstructor::UpdatesProvider updatesProviderFor(const std::vector<sdk::OccupancyGridUpdate>& updates) {
  return [&updates](Timestamp lo, Timestamp hi) {
    std::vector<sdk::OccupancyGridUpdate> out;
    for (const auto& u : updates) {
      if (u.timestamp_ns > lo && u.timestamp_ns <= hi) {
        out.push_back(u);
      }
    }
    std::sort(out.begin(), out.end(), [](const sdk::OccupancyGridUpdate& a, const sdk::OccupancyGridUpdate& b) {
      return a.timestamp_ns < b.timestamp_ns;
    });
    return out;
  };
}

// Build a base + N in-bounds updates with deterministic pseudo-random placement.
struct Timeline {
  std::vector<sdk::OccupancyGrid> bases;
  std::vector<sdk::OccupancyGridUpdate> updates;
};

Timeline makeTimeline(uint32_t grid = 16, int n_updates = 200, uint32_t seed = 12345) {
  Timeline tl;
  std::vector<uint8_t> base_bytes(static_cast<std::size_t>(grid) * grid);
  for (std::size_t i = 0; i < base_bytes.size(); ++i) {
    base_bytes[i] = static_cast<uint8_t>(i % 101);
  }
  tl.bases.push_back(makeBase(1000, grid, grid, std::move(base_bytes)));

  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pos(0, static_cast<int>(grid) - 3);
  std::uniform_int_distribution<int> dim(1, 3);
  for (int i = 1; i <= n_updates; ++i) {
    const uint32_t w = static_cast<uint32_t>(dim(rng));
    const uint32_t h = static_cast<uint32_t>(dim(rng));
    const int32_t x = pos(rng);
    const int32_t y = pos(rng);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(w) * h, static_cast<uint8_t>(i % 101));
    tl.updates.push_back(makeUpdate(1000 + 10LL * i, x, y, w, h, std::move(bytes)));
  }
  return tl;
}

TEST(OccupancyGridReconstructorTest, BaseOnlyNoUpdates) {
  const Timeline tl = makeTimeline(8, 0);
  OccupancyGridReconstructor r;
  const GridUpdate update = r.reconstructAt(2000, baseProviderFor(tl.bases), updatesProviderFor(tl.updates));
  const ReconstructedGrid& g = update.grid;
  EXPECT_EQ(g.width, 8u);
  EXPECT_EQ(g.height, 8u);
  EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, 2000));
  // First call with a base in effect → a full (re)build, not incremental.
  EXPECT_EQ(update.kind, GridUpdate::Kind::Full);
}

TEST(OccupancyGridReconstructorTest, NoBaseYieldsEmpty) {
  const Timeline tl = makeTimeline(8, 5);
  OccupancyGridReconstructor r;
  const GridUpdate update = r.reconstructAt(500, baseProviderFor(tl.bases), updatesProviderFor(tl.updates));
  EXPECT_TRUE(update.grid.empty());
  EXPECT_EQ(update.kind, GridUpdate::Kind::Empty);
}

// THE crux: any sequence of scrub targets (forward, backward, jumps, repeats)
// yields the same grid as an independent from-scratch replay for that time.
TEST(OccupancyGridReconstructorTest, SeekEquivalenceUnderRandomScrubbing) {
  const Timeline tl = makeTimeline(16, 200);
  const auto base_provider = baseProviderFor(tl.bases);
  const auto updates_provider = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;

  std::mt19937 rng(999);
  std::uniform_int_distribution<Timestamp> time_dist(900, 1000 + 10 * 210);
  for (int iter = 0; iter < 400; ++iter) {
    const Timestamp t = time_dist(rng);
    const ReconstructedGrid& g = r.reconstructAt(t, base_provider, updates_provider).grid;
    if (base_provider(t)) {
      EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, t)) << "mismatch at t=" << t << " iter=" << iter;
    } else {
      EXPECT_TRUE(g.empty()) << "expected empty before base at t=" << t;
    }
  }
}

TEST(OccupancyGridReconstructorTest, BackwardSeekMatchesReplay) {
  const Timeline tl = makeTimeline(16, 150);
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;
  // Go to the end, then step backward to several earlier times.
  r.reconstructAt(1000 + 10 * 151, bp, up);
  for (const Timestamp t :
       {Timestamp{1000 + 10 * 120}, Timestamp{1000 + 10 * 40}, Timestamp{1000 + 10 * 5}, Timestamp{1000}}) {
    const ReconstructedGrid& g = r.reconstructAt(t, bp, up).grid;
    EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, t)) << "backward mismatch at t=" << t;
  }
}

TEST(OccupancyGridReconstructorTest, NewEpochResetsAndIsReversible) {
  Timeline tl = makeTimeline(8, 20);
  // A second, differently-sized base keyframe at a later time.
  std::vector<uint8_t> b2(6u * 6u, 42u);
  tl.bases.push_back(makeBase(5000, 6, 6, std::move(b2)));
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;

  const ReconstructedGrid& g_after = r.reconstructAt(6000, bp, up).grid;
  EXPECT_EQ(g_after.width, 6u);
  EXPECT_EQ(g_after.cells, refReplay(tl.bases.back(), tl.updates, 6000));

  // Seek back into the first epoch.
  const ReconstructedGrid& g_before = r.reconstructAt(1000 + 10 * 10, bp, up).grid;
  EXPECT_EQ(g_before.width, 8u);
  EXPECT_EQ(g_before.cells, refReplay(tl.bases.front(), tl.updates, 1000 + 10 * 10));
}

TEST(OccupancyGridReconstructorTest, OutOfBoundsPatchClampedNoCrash) {
  std::vector<sdk::OccupancyGrid> bases{makeBase(1000, 8, 8, std::vector<uint8_t>(64, 0))};
  std::vector<sdk::OccupancyGridUpdate> updates;
  // Patch straddling the top-left corner and the right edge.
  updates.push_back(makeUpdate(1100, -2, -2, 4, 4, std::vector<uint8_t>(16, 50)));
  updates.push_back(makeUpdate(1200, 6, 6, 5, 5, std::vector<uint8_t>(25, 75)));
  OccupancyGridReconstructor r;
  const ReconstructedGrid& g = r.reconstructAt(1300, baseProviderFor(bases), updatesProviderFor(updates)).grid;
  EXPECT_EQ(g.cells, refReplay(bases.front(), updates, 1300));
}

TEST(OccupancyGridReconstructorTest, SnapshotMemoryBudgetHonored) {
  const Timeline tl = makeTimeline(32, 1000);  // 32*32 = 1024 B per snapshot
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r(/*snapshot_budget_bytes=*/4096);  // ≤ 4 snapshots
  // Drive a full forward pass to accumulate snapshots.
  r.reconstructAt(1000 + 10 * 1001, bp, up);
  EXPECT_LE(r.snapshotBytes(), 4096u);
  // Correctness is independent of the (now sparse) snapshot cache.
  for (const Timestamp t : {1000 + 10 * 800, 1000 + 10 * 300, 1000 + 10 * 50}) {
    EXPECT_EQ(r.reconstructAt(t, bp, up).grid.cells, refReplay(tl.bases.front(), tl.updates, t));
  }
}

}  // namespace
}  // namespace PJ
