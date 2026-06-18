// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/depth_pipeline_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"

namespace PJ {
namespace {

ObjectTopicId registerDepthTopic(ObjectStore& store) {
  auto id = store.registerTopic(
      {.dataset_id = DatasetId{1}, .topic_name = "/camera/depth", .metadata_json = R"({"schema":"PJ.DepthImage"})"});
  return id.has_value() ? *id : ObjectTopicId{};
}

std::vector<uint8_t> makeU16Le(const std::vector<uint16_t>& values) {
  std::vector<uint8_t> bytes(values.size() * 2);
  for (size_t i = 0; i < values.size(); ++i) {
    bytes[i * 2 + 0] = static_cast<uint8_t>(values[i] & 0xFF);
    bytes[i * 2 + 1] = static_cast<uint8_t>((values[i] >> 8) & 0xFF);
  }
  return bytes;
}

std::vector<uint8_t> makeF32Le(const std::vector<float>& values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

// Serializes a depth-encoded sdk::Image (the type the depth source consumes —
// there is no kDepthImage producer).
std::vector<uint8_t> serializeDepth(
    uint32_t width, uint32_t height, const std::string& encoding, std::vector<uint8_t> payload) {
  sdk::Image img;
  img.timestamp_ns = 1'234;
  img.width = width;
  img.height = height;
  img.encoding = encoding;
  img.data = Span<const uint8_t>(payload.data(), payload.size());
  return serializeImage(img);
}

const DecodedFrame* onlyPixelLayerFrame(const MediaFrame& frame) {
  EXPECT_FALSE(frame.base.has_value());
  if (frame.pixel_layers.size() != 1u) {
    ADD_FAILURE() << "expected exactly one pixel layer, got " << frame.pixel_layers.size();
    return nullptr;
  }
  return &frame.pixel_layers.front().frame;
}

// Bridges the source's worker-thread frame-ready callback to a condition variable
// the test can block on (decoding is now off-thread). Install AFTER the set*()
// calls and before the first setTimestamp().
struct FrameSync {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;

  void install(DepthPipelineSource& source) {
    source.setFrameReadyCallback([this] {
      std::lock_guard<std::mutex> lock(mutex);
      ready = true;
      cv.notify_all();
    });
  }

  bool waitReady(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock<std::mutex> lock(mutex);
    if (cv.wait_for(lock, timeout, [this] { return ready; })) {
      ready = false;
      return true;
    }
    return false;
  }
};

// Post `ts`, block until the worker decodes a frame, then hand it back.
std::optional<MediaFrame> pumpFrameAt(DepthPipelineSource& source, FrameSync& sync, int64_t ts) {
  source.setTimestamp(ts);
  EXPECT_TRUE(sync.waitReady()) << "decode worker produced no frame within the timeout";
  return source.takeFrame();
}

TEST(DepthPipelineSourceTest, EmitsRawDepthFloatsWithParams) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeU16Le({1000, 2000, 3000, 0});  // mm; 0 == no-data
  const auto bytes = serializeDepth(4, 1, "16UC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 1'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setRange(1.0f, 3.0f);
  source.setColormap(0);  // opaque id == pj_widgets Colormap::kTurbo
  source.setOpacity(0.4f);
  FrameSync sync;
  sync.install(source);

  auto frame = pumpFrameAt(source, sync, 1'000);
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  EXPECT_EQ(layer_frame->width, 4);
  EXPECT_EQ(layer_frame->height, 1);
  EXPECT_EQ(layer_frame->format, PixelFormat::kDepthR32F);
  ASSERT_NE(layer_frame->pixels, nullptr);
  ASSERT_EQ(layer_frame->pixels->size(), 4u * sizeof(float));
  EXPECT_FLOAT_EQ(frame->pixel_layers[0].opacity, 0.4f);

  // Raw metric depth (mm -> m); the invalid sample becomes 0 (the shader's no-data).
  const auto* depths = reinterpret_cast<const float*>(layer_frame->pixels->data());
  EXPECT_FLOAT_EQ(depths[0], 1.0f);
  EXPECT_FLOAT_EQ(depths[1], 2.0f);
  EXPECT_FLOAT_EQ(depths[2], 3.0f);
  EXPECT_FLOAT_EQ(depths[3], 0.0f);

  // The colormap params travel with the frame for the GPU shader.
  EXPECT_TRUE(layer_frame->depth.active);
  EXPECT_FLOAT_EQ(layer_frame->depth.near_m, 1.0f);
  EXPECT_FLOAT_EQ(layer_frame->depth.far_m, 3.0f);
  EXPECT_FALSE(layer_frame->depth.invert);
  EXPECT_EQ(layer_frame->depth.colormap, 0u);  // kTurbo
}

TEST(DepthPipelineSourceTest, Emits32FC1FloatsPassthrough) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeF32Le({0.5f, 1.5f});
  const auto bytes = serializeDepth(2, 1, "32FC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 2'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setRange(0.5f, 1.5f);
  FrameSync sync;
  sync.install(source);

  auto frame = pumpFrameAt(source, sync, 2'000);
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  EXPECT_EQ(layer_frame->format, PixelFormat::kDepthR32F);
  ASSERT_EQ(layer_frame->pixels->size(), 2u * sizeof(float));
  const auto* depths = reinterpret_cast<const float*>(layer_frame->pixels->data());
  EXPECT_FLOAT_EQ(depths[0], 0.5f);
  EXPECT_FLOAT_EQ(depths[1], 1.5f);
}

TEST(DepthPipelineSourceTest, DepthParamsCarryInvertAndColormap) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto bytes = serializeDepth(1, 1, "16UC1", makeU16Le({1000}));
  ASSERT_TRUE(store.pushOwned(topic, 7'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setColormap(2);  // opaque id == pj_widgets Colormap::kPlasma
  source.setInvert(true);
  FrameSync sync;
  sync.install(source);

  auto frame = pumpFrameAt(source, sync, 7'000);
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  EXPECT_TRUE(layer_frame->depth.invert);
  EXPECT_EQ(layer_frame->depth.colormap, 2u);  // kPlasma
}

}  // namespace
}  // namespace PJ
