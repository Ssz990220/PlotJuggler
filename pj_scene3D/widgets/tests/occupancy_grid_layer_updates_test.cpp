// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Layer-level coverage for OccupancyGridLayer's two-topic '<base>_updates'
// wiring (review M.63): the UID-based store-window traversal that feeds
// OccupancyGridReconstructor is cross-checked against an independent
// cell-by-cell replay at several scrub targets (forward, backward, across
// epochs). Also regression-covers the retroactive-ingest invalidate path
// (review H.13: an update ingested behind the already-consumed live edge must
// still appear on the next forward render) and the base-keyframe parse memo
// (review M.47: re-rendering an unchanged keyframe must not re-parse it).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kBaseSchema = "mock/occupancy_grid";
constexpr std::string_view kUpdateSchema = "mock/occupancy_grid_update";

std::atomic<int> g_base_parser_calls{0};
std::atomic<int> g_update_parser_calls{0};

// Mock wire format: a uint8 header followed by row-major cell bytes.
//   base:   [width, height, cells...]      cells.size() == width * height
//   update: [x, y, width, height, cells...]

// Parses the base schema into an sdk::OccupancyGrid whose data Span is anchored
// to a private copy of the payload (the ObjectRecord must own its bytes).
PJ::Expected<PJ::sdk::ObjectRecord> emitBaseGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  auto bytes =
      std::make_shared<std::vector<uint8_t>>(payload.bytes.data(), payload.bytes.data() + payload.bytes.size());
  PJ::sdk::OccupancyGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.resolution = 1.0;
  grid.width = (*bytes)[0];
  grid.height = (*bytes)[1];
  grid.data = PJ::Span<const uint8_t>(bytes->data() + 2, bytes->size() - 2);
  grid.anchor = bytes;
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

// Parses the update schema into an sdk::OccupancyGridUpdate whose data Span is
// anchored to a private copy of the payload (the ObjectRecord must own its bytes).
PJ::Expected<PJ::sdk::ObjectRecord> emitGridUpdate(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  auto bytes =
      std::make_shared<std::vector<uint8_t>>(payload.bytes.data(), payload.bytes.data() + payload.bytes.size());
  PJ::sdk::OccupancyGridUpdate update;
  update.timestamp_ns = ts;
  update.frame_id = "map";
  update.x = (*bytes)[0];
  update.y = (*bytes)[1];
  update.width = (*bytes)[2];
  update.height = (*bytes)[3];
  update.data = PJ::Span<const uint8_t>(bytes->data() + 4, bytes->size() - 4);
  update.anchor = bytes;
  return PJ::sdk::ObjectRecord{.ts = ts, .object = update};
}

// --- Independent reference replay (mirror of the reconstructor contract) ---

struct RefBase {
  int64_t ts = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t fill = 0;
};

struct RefUpdate {
  int64_t ts = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t value = 0;
};

struct RefGrid {
  int64_t base_ts = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<int8_t> cells;
};

// displayed(t) = latest base with ts <= t, plus every update in (base_ts, t]
// applied in ascending-ts order (insertion order within equal-ts runs).
std::optional<RefGrid> refReplay(const std::vector<RefBase>& bases, std::vector<RefUpdate> updates, int64_t t) {
  const RefBase* base = nullptr;
  for (const auto& candidate : bases) {
    if (candidate.ts <= t && (base == nullptr || candidate.ts > base->ts)) {
      base = &candidate;
    }
  }
  if (base == nullptr) {
    return std::nullopt;
  }
  RefGrid grid{
      base->ts, base->width, base->height,
      std::vector<int8_t>(static_cast<std::size_t>(base->width) * base->height, static_cast<int8_t>(base->fill))};
  std::stable_sort(updates.begin(), updates.end(), [](const RefUpdate& a, const RefUpdate& b) { return a.ts < b.ts; });
  for (const auto& update : updates) {
    if (update.ts <= base->ts || update.ts > t) {
      continue;
    }
    for (uint32_t row = 0; row < update.height; ++row) {
      for (uint32_t col = 0; col < update.width; ++col) {
        grid.cells[static_cast<std::size_t>(update.y + row) * grid.width + update.x + col] =
            static_cast<int8_t>(update.value);
      }
    }
  }
  return grid;
}

class OccupancyGridLayerUpdatesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_base_parser_calls = 0;
    g_update_parser_calls = 0;

    base_topic_ = registerObjectTopic(session_, "/map");
    updates_topic_ = registerObjectTopic(session_, "/map_updates");

    session_.registerObjectTopicParser(base_topic_, makeBoundHandle(kBaseSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kBaseSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid,
                                             &g_base_parser_calls, &emitBaseGrid);
                                       }));
    session_.registerObjectTopicParser(updates_topic_, makeBoundHandle(kUpdateSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kUpdateSchema, PJ::sdk::BuiltinObjectType::kOccupancyGridUpdate,
                                             &g_update_parser_calls, &emitGridUpdate);
                                       }));
  }

  PJ::ObjectStore& store() {
    return session_.objectStore();
  }

  // Pushes a uniformly-filled width x height base keyframe and mirrors it into
  // the reference timeline.
  void pushBase(int64_t ts, uint32_t width, uint32_t height, uint8_t fill) {
    std::vector<uint8_t> payload{static_cast<uint8_t>(width), static_cast<uint8_t>(height)};
    payload.insert(payload.end(), static_cast<std::size_t>(width) * height, fill);
    ASSERT_TRUE(store().pushOwned(base_topic_, ts, std::move(payload)).has_value());
    ref_bases_.push_back(RefBase{ts, width, height, fill});
  }

  // Pushes a uniformly-filled patch rect and mirrors it into the reference.
  void pushUpdate(int64_t ts, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t value) {
    std::vector<uint8_t> payload{
        static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(width), static_cast<uint8_t>(height)};
    payload.insert(payload.end(), static_cast<std::size_t>(width) * height, value);
    ASSERT_TRUE(store().pushOwned(updates_topic_, ts, std::move(payload)).has_value());
    ref_updates_.push_back(RefUpdate{ts, x, y, width, height, value});
  }

  // Renders the layer at t and asserts its reconstructed grid matches the
  // independent reference replay cell-by-cell.
  void renderAndExpectRef(pj::scene3d::OccupancyGridLayer& layer, int64_t t) {
    layer.renderAtForTest(t);
    const auto expected = refReplay(ref_bases_, ref_updates_, t);
    ASSERT_TRUE(expected.has_value()) << "reference has no base at t=" << t;
    const auto& grid = layer.reconstructedGridForTest();
    ASSERT_FALSE(grid.empty()) << "layer reconstructed an empty grid at t=" << t;
    EXPECT_EQ(grid.base_timestamp_ns, expected->base_ts) << "wrong epoch keyframe at t=" << t;
    ASSERT_EQ(grid.width, expected->width);
    ASSERT_EQ(grid.height, expected->height);
    EXPECT_EQ(grid.cells, expected->cells) << "reconstructed cells diverge from reference replay at t=" << t;
  }

  PJ::SessionManager session_;
  PJ::ObjectTopicId base_topic_;
  PJ::ObjectTopicId updates_topic_;
  std::vector<RefBase> ref_bases_;
  std::vector<RefUpdate> ref_updates_;
};

// M.63: the store-window arithmetic feeding the reconstructor, exercised at the
// layer boundary across forward steps, equal-ts update runs, backward seeks,
// and an epoch change — each target cross-checked against the reference replay.
TEST_F(OccupancyGridLayerUpdatesTest, ScrubReplayMatchesReference) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);
  pushUpdate(120, 0, 0, 1, 1, 100);
  pushUpdate(120, 3, 3, 1, 1, 75);  // equal-ts run with the previous patch
  pushUpdate(130, 2, 0, 2, 1, 25);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, QStringLiteral("map"));
  ASSERT_TRUE(layer.attach(ctx));

  renderAndExpectRef(layer, 100);  // base only
  renderAndExpectRef(layer, 115);  // + first patch
  renderAndExpectRef(layer, 120);  // + the equal-ts pair
  renderAndExpectRef(layer, 130);  // everything
  renderAndExpectRef(layer, 110);  // backward seek inside the epoch
  renderAndExpectRef(layer, 125);  // forward again from the restored state

  // Epoch change: a second keyframe supersedes the first; updates before it no
  // longer apply, updates after it do.
  pushBase(300, 4, 4, 10);
  pushUpdate(310, 0, 3, 2, 1, 90);
  renderAndExpectRef(layer, 310);  // new epoch + its patch
  renderAndExpectRef(layer, 305);  // backward inside the new epoch (base2 only)
  renderAndExpectRef(layer, 125);  // backward across epochs, back to base1
}

// H.13: the live drive advances the consumed window past the updates topic's
// ingest; an update that lands later with ts at-or-before the consumed time
// must still be folded in (via reconstructor invalidate + full rebuild) instead
// of being skipped forever by the forward (last_t_, t] window.
TEST_F(OccupancyGridLayerUpdatesTest, RetroactiveUpdateIsReappliedAfterLiveEdgeOvershoot) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, QStringLiteral("map"));
  ASSERT_TRUE(layer.attach(ctx));

  // The tracker is driven by a faster sibling topic's live edge, far past the
  // updates topic's newest entry (ts=110).
  renderAndExpectRef(layer, 200);

  // Transport jitter: an update stamped 150 is ingested AFTER time 200 was
  // already consumed. The plain forward window (200, 210] would never see it.
  pushUpdate(150, 0, 0, 1, 1, 100);
  renderAndExpectRef(layer, 210);
  EXPECT_EQ(layer.reconstructedGridForTest().cells[0], static_cast<int8_t>(100))
      << "update ingested behind the consumed live edge was dropped";

  // And the rebuild must not loop: another forward render keeps the patch.
  renderAndExpectRef(layer, 220);
}

// M.47: re-rendering while the active base keyframe is unchanged must reuse the
// memoized parse instead of re-parsing (and deep-copying) the cell payload on
// every tracker tick.
TEST_F(OccupancyGridLayerUpdatesTest, UnchangedBaseKeyframeIsNotReparsed) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, QStringLiteral("map"));
  ASSERT_TRUE(layer.attach(ctx));  // bootstrap parses the first sample once

  renderAndExpectRef(layer, 120);  // first renderAt populates the memo
  const int base_parses_after_first_render = g_base_parser_calls.load();

  renderAndExpectRef(layer, 121);
  renderAndExpectRef(layer, 122);
  renderAndExpectRef(layer, 105);  // backward seek: same keyframe, memo still valid
  EXPECT_EQ(g_base_parser_calls.load(), base_parses_after_first_render)
      << "unchanged base keyframe was re-parsed on a later tick";

  // A new keyframe is a different store entry: exactly one more parse.
  pushBase(300, 4, 4, 10);
  renderAndExpectRef(layer, 300);
  EXPECT_EQ(g_base_parser_calls.load(), base_parses_after_first_render + 1);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
