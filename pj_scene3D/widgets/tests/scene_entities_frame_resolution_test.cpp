// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Reproduction for "a SceneEntities batch renders only ONE box": a batch of 18
// cubes across 17 distinct entity frames (all children of a "display" root) must
// survive decode AND resolve to 18 render instances when the TF tree is present,
// regardless of which frame is the fixed frame. The one-box symptom is the
// marker pass skipping every primitive whose frame does not resolve
// (marker_render_pass.cpp build_solids) — which happens when the TF buffer bound
// to the dock is EMPTY (frames disconnected) and the fixed frame collapses to one
// entity's own leaf frame, so only that single entity resolves (identity).
//
// This test feeds the batch through the real SceneEntitiesLayer decode
// (activeMarkersForTest) and replicates the pass's exact frame-resolution gate
// against a TransformBuffer, so it needs no GL context.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <optional>
#include <string>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/scene_entities_decode.h"
#include "pj_scene3d_core/scene_entities_render.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using namespace pj::scene3d::test;

constexpr int kFrameCount = 17;  // distinct entity frames, all children of "display"
constexpr int kCubeCount = 18;   // one extra cube shares frame_0, so 18 cubes / 17 frames

PJ::Expected<PJ::sdk::ObjectRecord> emitSceneEntities(PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) {
  auto decoded = PJ::deserializeSceneEntities(payload.bytes.data(), payload.bytes.size());
  if (!decoded.has_value()) {
    return PJ::unexpected(std::move(decoded).error());
  }
  return PJ::sdk::ObjectRecord{.ts = std::nullopt, .object = PJ::sdk::BuiltinObject{std::move(*decoded)}};
}

PJ::ObjectTopicId registerTopic(PJ::SessionManager& session) {
  return registerObjectTopic(session, "/entities");
}

void registerParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  session.registerObjectTopicParser(
      topic_id, makeBoundHandle("entities", []() noexcept -> void* {
        return new CountingObjectParser("entities", PJ::sdk::BuiltinObjectType::kSceneEntities, nullptr,
                                        &emitSceneEntities);
      }));
}

std::string frameName(int i) {
  return "frame_" + std::to_string(i);
}

// A cube-bearing entity in its own frame — the robot-trajectory shape: many
// per-joint boxes, each stamped with that joint's frame_id.
PJ::sdk::SceneEntity makeCubeEntity(const std::string& id, const std::string& frame_id) {
  PJ::sdk::SceneEntity entity;
  entity.id = id;
  entity.timestamp = 10;
  entity.frame_id = frame_id;
  PJ::sdk::CubePrimitive cube;
  cube.size = {.x = 1.0, .y = 1.0, .z = 1.0};
  cube.color = {.r = 200, .g = 200, .b = 200, .a = 255};
  entity.cubes.push_back(cube);
  return entity;
}

// 18 cubes across 17 frames (frame_0 carries two). Mirrors the reported batch
// structure (18 cubes / 17 distinct frame_ids).
PJ::sdk::SceneEntities makeBatch() {
  PJ::sdk::SceneEntities batch;
  for (int i = 0; i < kFrameCount; ++i) {
    batch.entities.push_back(makeCubeEntity("e" + std::to_string(i), frameName(i)));
  }
  batch.entities.push_back(makeCubeEntity("e_extra", frameName(0)));  // 18th cube, reuses frame_0
  return batch;
}

// Flat "display"-rooted TF: display -> frame_i for every entity frame.
void fillDisplayRootedTf(pj::scene3d::TransformBuffer& tf) {
  for (int i = 0; i < kFrameCount; ++i) {
    ASSERT_TRUE(tf.setTransform(
        pj::scene3d::StampedTransform{
            .stamp = PJ::fromRaw(0),
            .parent_frame = "display",
            .child_frame = frameName(i),
            .transform = pj::scene3d::Transform({static_cast<double>(i), 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}),
        }));
  }
}

// The exact MarkerRenderPass::build_solids gate: a cube survives iff its interned
// frame resolves against the fixed frame.
int resolvedCubeCount(
    const pj::scene3d::DecodedSceneEntities& batch, const pj::scene3d::TransformBuffer& tf,
    const std::string& fixed_frame) {
  const pj::scene3d::FrameContext frame_ctx{tf, fixed_frame, PJ::fromRaw(10)};
  int resolved = 0;
  for (const auto& cube : batch.cubes) {
    if (cube.frame_index < batch.frames.size() && frame_ctx.lookup(batch.frames[cube.frame_index]).has_value()) {
      ++resolved;
    }
  }
  return resolved;
}

pj::scene3d::Scene3DLayerContext makeContext(PJ::SessionManager& session) {
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  return ctx;
}

const pj::scene3d::DecodedSceneEntities& decodeThroughLayer(pj::scene3d::SceneEntitiesLayer& layer) {
  const auto* active = layer.activeMarkersForTest();
  EXPECT_NE(active, nullptr) << "layer never set an active marker batch";
  static const pj::scene3d::DecodedSceneEntities kEmpty;
  return active != nullptr ? *active : kEmpty;
}

}  // namespace

// The layer's decode must preserve the ENTIRE batch: 18 cubes across 17 frames.
// (Guards the layer-ingest / decode half of the pipeline.)
TEST(SceneEntitiesFrameResolutionTest, LayerDecodePreservesAllCubesAndFrames) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  ASSERT_TRUE(session.objectStore().pushOwned(topic_id, 10, PJ::serializeSceneEntities(makeBatch())));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, QStringLiteral("/entities"));
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(10));

  const auto& decoded = decodeThroughLayer(layer);
  EXPECT_EQ(decoded.cubes.size(), static_cast<std::size_t>(kCubeCount)) << "decode dropped cubes";
  EXPECT_EQ(decoded.frames.size(), static_cast<std::size_t>(kFrameCount)) << "decode dropped/merged frames";
}

// WITH the display-rooted TF present, ALL 18 cubes resolve — whether the fixed
// frame is the root ("display") or any leaf entity frame (siblings resolve
// through the common ancestor). This is the correct Foxglove behavior.
TEST(SceneEntitiesFrameResolutionTest, AllCubesResolveWhenTfPresent) {
  const pj::scene3d::DecodedSceneEntities decoded = pj::scene3d::decodeSceneEntities(makeBatch());
  ASSERT_EQ(decoded.cubes.size(), static_cast<std::size_t>(kCubeCount));

  pj::scene3d::TransformBuffer tf(pj::scene3d::TransformBuffer::kKeepAll);
  fillDisplayRootedTf(tf);

  EXPECT_EQ(resolvedCubeCount(decoded, tf, "display"), kCubeCount) << "root fixed frame dropped cubes";
  EXPECT_EQ(resolvedCubeCount(decoded, tf, frameName(0)), kCubeCount) << "leaf fixed frame dropped sibling cubes";
  EXPECT_EQ(resolvedCubeCount(decoded, tf, frameName(9)), kCubeCount) << "another leaf fixed frame dropped siblings";
}

// The one-box symptom, reproduced: with an EMPTY TF buffer (the bug condition —
// the dock bound a TF buffer that was never populated for the entities' dataset)
// and the fixed frame collapsed to one entity's own leaf frame, only that single
// cube resolves. This is what the user sees.
TEST(SceneEntitiesFrameResolutionTest, EmptyTfCollapsesToSingleCube_ReproducesBug) {
  const pj::scene3d::DecodedSceneEntities decoded = pj::scene3d::decodeSceneEntities(makeBatch());
  ASSERT_EQ(decoded.cubes.size(), static_cast<std::size_t>(kCubeCount));

  const pj::scene3d::TransformBuffer empty_tf(pj::scene3d::TransformBuffer::kKeepAll);

  // frame_0 carries two cubes (e0 + e_extra); every other frame is unreachable in
  // the empty buffer, so only frame_0's cubes render.
  EXPECT_EQ(resolvedCubeCount(decoded, empty_tf, frameName(0)), 2)
      << "empty-TF fixed=frame_0 should resolve only the two frame_0 cubes";
  EXPECT_EQ(resolvedCubeCount(decoded, empty_tf, frameName(5)), 1)
      << "empty-TF fixed=frame_5 should resolve only its one cube (the radio-button symptom)";
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
