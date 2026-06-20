// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural coverage for VoxelGridLayer: lazy decode + sample-UID memo (a
// re-scrub to a seen grid does NO re-parse — the zero-per-voxel-CPU contract),
// new-sample re-decode, streaming attach before the first sample, the per-use
// parser binding, XML round-trip, and worldBounds.

#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QString>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/voxel_grid";

std::atomic<int> g_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};

// A fixed 2x2x2 occupancy grid (uint8 field, strides {1,2,4}); the static buffer
// outlives every parse so the zero-copy Span stays valid.
PJ::Expected<PJ::sdk::ObjectRecord> emitVoxel(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const uint8_t k_data[8] = {0, 10, 0, 20, 0, 0, 30, 0};
  PJ::sdk::VoxelGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.cell_size = {1.0, 1.0, 1.0};
  grid.column_count = 2;
  grid.row_count = 2;
  grid.slice_count = 2;
  grid.cell_stride = 1;
  grid.row_stride = 2;
  grid.slice_stride = 4;
  PJ::sdk::PointField field;
  field.name = "occupancy";
  field.offset = 0;
  field.datatype = PJ::sdk::PointField::Datatype::kUint8;
  field.count = 1;
  grid.fields = {field};
  grid.data = PJ::Span<const uint8_t>(k_data, 8);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

void registerVoxelParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kVoxelGrid, &g_parser_calls, &emitVoxel);
                                    }));
}

TEST(VoxelGridLayer, RescrubToSameGridDoesNoReparse) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(g_parser_calls.load(), 1);  // bootstrap decoded the first sample

  layer.renderAtForTest(100);
  const int after_first = g_parser_calls.load();
  EXPECT_GE(after_first, 1);
  EXPECT_TRUE(layer.hasGridForTest());
  EXPECT_EQ(layer.resolvedFieldForTest(), QStringLiteral("occupancy"));

  // Re-scrub to the SAME store entry: no re-parse, no re-pack (req: scrubbing a
  // previously-seen grid does zero per-voxel CPU work).
  layer.renderAtForTest(100);
  layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), after_first) << "re-scrub to a cached grid must not re-parse";
}

// Regression (triple-review critical): scrubbing to before the first sample
// clears the pass; scrubbing back onto the SAME store entry must re-stage the grid.
// The bug was that renderAt's no-sample branch cleared the pass but left
// uploaded_uid_ intact, so the fast-path skipped the re-upload and the grid
// silently vanished for the rest of the session.
TEST(VoxelGridLayer, BackScrubAfterEmptyTickReStagesGrid) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.passHasStagedGridForTest());

  // Scrub before the only sample: latestAt() returns nothing -> pass cleared.
  layer.renderAtForTest(50);
  EXPECT_FALSE(layer.passHasStagedGridForTest());

  // Scrub back onto the same entry: the grid must come back.
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.passHasStagedGridForTest()) << "grid must re-stage after a back-scrub through an empty tick";
}

TEST(VoxelGridLayer, NewSampleReDecodes) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);
  const int after_first = g_parser_calls.load();

  // A second, distinct store entry → a fresh UID → must re-decode.
  ASSERT_TRUE(store.pushOwned(topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  layer.renderAtForTest(200);
  EXPECT_GT(g_parser_calls.load(), after_first) << "a new sample must trigger a re-decode";
}

TEST(VoxelGridLayer, StreamingAttachBeforeSampleSelfHeals) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  registerVoxelParser(session, topic_id);  // parser, but NO sample yet

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx)) << "attach must tolerate an empty store";
  EXPECT_EQ(g_parser_calls.load(), 0) << "bootstrap on an empty store must not parse";

  layer.renderAtForTest(100);
  EXPECT_FALSE(layer.hasGridForTest());

  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  layer.renderAtForTest(100);
  EXPECT_GE(g_parser_calls.load(), 1);
  EXPECT_TRUE(layer.hasGridForTest());
  EXPECT_EQ(layer.sourceFrame(), QStringLiteral("map"));
}

TEST(VoxelGridLayer, RebindsToReRegisteredParser) {
  g_parser_calls.store(0);
  g_second_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);  // first parser counts g_parser_calls

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx));  // bootstrap decodes via the first parser

  // Keep the first parser alive so the buggy stale-pointer path (if reintroduced)
  // is a deterministic wrong-counter call rather than a use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Re-register the topic's parser (the file-reload swap). The layer resolves the
  // binding per use, so the next decode must reach the NEW parser. bootstrap() did
  // not populate the parse memo, so renderAt re-decodes through the fresh binding.
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kVoxelGrid, &g_second_parser_calls,
                                          &emitVoxel);
                                    }));

  const int stale_before = g_parser_calls.load();
  layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), stale_before) << "layer called the replaced parser after reload";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer must rebind to the re-registered parser";
}

TEST(VoxelGridLayer, WorldBoundsReflectGrid) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);

  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->min.x, 0.0f);
  EXPECT_FLOAT_EQ(bounds->max.x, 2.0f);  // 2 cells * 1.0 m
  EXPECT_FLOAT_EQ(bounds->max.z, 2.0f);
}

TEST(VoxelGridLayer, XmlRoundTrip) {
  PJ::SessionManager session;
  const auto topic_id = registerObjectTopic(session, "/voxels");

  pj::scene3d::VoxelGridLayer source(topic_id, QStringLiteral("voxels"));
  source.setActiveField(QStringLiteral("cost"));
  source.setDrawMode(pj::scene3d::VoxelDrawMode::kThreshold);
  source.setThreshold(0.75);
  source.setAutoRange(false);
  source.setManualRange(-2.0, 8.0);
  source.setColormap(PJ::Colormap::kViridis);
  source.setOpacity(0.5);

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  pj::scene3d::VoxelGridLayer restored(topic_id, QStringLiteral("voxels"));
  ASSERT_TRUE(restored.xmlLoadState(saved));

  // Re-serialize and compare: every persisted attribute must survive the trip.
  QDomDocument doc2;
  const QDomElement re_saved = restored.xmlSaveState(doc2);
  EXPECT_EQ(re_saved.attribute("field"), QStringLiteral("cost"));
  EXPECT_EQ(re_saved.attribute("draw_mode").toInt(), static_cast<int>(pj::scene3d::VoxelDrawMode::kThreshold));
  EXPECT_DOUBLE_EQ(re_saved.attribute("threshold").toDouble(), 0.75);
  EXPECT_EQ(re_saved.attribute("auto_range").toInt(), 0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_lo").toDouble(), -2.0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_hi").toDouble(), 8.0);
  EXPECT_EQ(re_saved.attribute("colormap").toInt(), static_cast<int>(PJ::Colormap::kViridis));
  EXPECT_DOUBLE_EQ(re_saved.attribute("opacity").toDouble(), 0.5);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
