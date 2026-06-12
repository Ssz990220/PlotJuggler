// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression test for the file-reload crash: SessionManager::replaceDataset()
// re-registers each surviving topic's parser slot, destroying the previous
// MessageParserHandle. A layer that cached the raw parser pointer at attach()
// then calls parseObject() on freed memory on the next tracker tick. Layers
// must instead resolve the parser binding through the session on every use,
// so a reload transparently rebinds them to the new parser.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <atomic>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace {

constexpr std::string_view kSchema = "mock/occupancy_grid";

std::atomic<int> g_first_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};

// Minimal parser producing a fixed 1x1 grid; bumps `counter` per parseObject.
class CountingGridParser : public PJ::MessageParserPluginBase {
 public:
  explicit CountingGridParser(std::atomic<int>* counter) {
    registerSchemaHandler(
        kSchema,
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kOccupancyGrid,
            .parse_scalars = {},
            .parse_object =
                [counter](PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
              counter->fetch_add(1);
              static const uint8_t kCell[1] = {0};
              PJ::sdk::OccupancyGrid grid;
              grid.timestamp_ns = ts;
              grid.frame_id = "map";
              grid.resolution = 0.05;
              grid.width = 1;
              grid.height = 1;
              grid.data = PJ::Span<const uint8_t>(kCell, 1);
              return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
            },
        });
  }
};

// Each call site must pass a distinct lambda type: vtableWithCreate() holds
// one `static` vtable per CreateFn instantiation, so a shared plain function
// pointer type would latch the first create function for both handles.
template <typename CreateFn>
std::unique_ptr<PJ::MessageParserHandle> makeBoundHandle(CreateFn create_fn) {
  static constexpr const char* kManifest =
      R"({"id":"counting-grid-parser","name":"Counting Grid Parser","version":"1.0.0","encoding":["mock"]})";
  auto handle =
      std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(create_fn, kManifest));
  EXPECT_TRUE(handle->valid());
  const auto bound = handle->bindSchema(kSchema, {});
  EXPECT_TRUE(bound.has_value());
  return handle;
}

TEST(OccupancyGridLayerRebind, ReloadSwapsParserWithoutTouchingStaleOne) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/map";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());

  session.registerObjectTopicParser(
      *topic_id, makeBoundHandle([]() noexcept -> void* { return new CountingGridParser(&g_first_parser_calls); }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::OccupancyGridLayer layer(*topic_id, QStringLiteral("map"));
  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(g_first_parser_calls.load(), 1);  // bootstrap decoded the first sample

  // Keep the first parser's memory alive from the test, so the buggy
  // stale-pointer call below shows up as a deterministic wrong-parser call
  // instead of undefined behavior. Production holds no such guard — there the
  // same call is a use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(*topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: replaceDataset() re-registers the surviving
  // topic's parser under its stable id, overwriting the slot and dropping the
  // old handle.
  session.registerObjectTopicParser(
      *topic_id, makeBoundHandle([]() noexcept -> void* { return new CountingGridParser(&g_second_parser_calls); }));

  const int stale_calls_before = g_first_parser_calls.load();
  layer.setTrackerTime(PJ::fromRaw(100));

  EXPECT_EQ(g_first_parser_calls.load(), stale_calls_before)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
