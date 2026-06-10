// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/depth_pipeline_source.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/depth_image_codec.hpp"

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

std::vector<uint8_t> serializeDepth(
    uint32_t width, uint32_t height, const std::string& encoding, std::vector<uint8_t> payload) {
  sdk::DepthImage depth;
  depth.timestamp_ns = 1'234;
  depth.width = width;
  depth.height = height;
  depth.encoding = encoding;
  depth.data = Span<const uint8_t>(payload.data(), payload.size());
  return serializeDepthImage(depth);
}

const DecodedFrame* onlyPixelLayerFrame(const MediaFrame& frame) {
  EXPECT_FALSE(frame.base.has_value());
  if (frame.pixel_layers.size() != 1u) {
    ADD_FAILURE() << "expected exactly one pixel layer, got " << frame.pixel_layers.size();
    return nullptr;
  }
  return &frame.pixel_layers.front().frame;
}

TEST(DepthPipelineSourceTest, Decodes16UC1ToRgbaPixelLayer) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeU16Le({1000, 2000, 3000, 0});
  const auto bytes = serializeDepth(4, 1, "16UC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 1'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setAutoRange(false);
  source.setRange(1.0f, 3.0f);
  source.setColormap(DepthColormap::kTurbo);
  source.setOpacity(0.4f);
  source.setTimestamp(1'000);

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  EXPECT_EQ(layer_frame->width, 4);
  EXPECT_EQ(layer_frame->height, 1);
  EXPECT_EQ(layer_frame->format, PixelFormat::kRGBA8888);
  ASSERT_NE(layer_frame->pixels, nullptr);
  ASSERT_EQ(layer_frame->pixels->size(), 16u);
  EXPECT_FLOAT_EQ(frame->pixel_layers[0].opacity, 0.4f);

  const auto& pixels = *layer_frame->pixels;
  EXPECT_LT(pixels[0], pixels[4]);  // red rises as depth increases
  EXPECT_LT(pixels[4], pixels[8]);
  EXPECT_GT(pixels[2], pixels[6]);  // blue falls as depth increases
  EXPECT_GT(pixels[6], pixels[10]);
  EXPECT_EQ(pixels[15], 0);  // zero depth is invalid/transparent
}

TEST(DepthPipelineSourceTest, Decodes32FC1Meters) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeF32Le({0.5f, 1.5f});
  const auto bytes = serializeDepth(2, 1, "32FC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 2'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setAutoRange(false);
  source.setRange(0.5f, 1.5f);
  source.setTimestamp(2'000);

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  EXPECT_EQ(layer_frame->width, 2);
  EXPECT_EQ(layer_frame->height, 1);
  EXPECT_EQ(layer_frame->format, PixelFormat::kRGBA8888);
  ASSERT_NE(layer_frame->pixels, nullptr);
  EXPECT_EQ(layer_frame->pixels->size(), 8u);
  EXPECT_NE((*layer_frame->pixels)[0], (*layer_frame->pixels)[4]);
}

TEST(DepthPipelineSourceTest, AutoRangeMapsObservedMinMaxToColormapEndpoints) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeU16Le({1000, 2000, 3000});
  const auto bytes = serializeDepth(3, 1, "16UC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 3'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setAutoRange(true);
  source.setColormap(DepthColormap::kTurbo);
  source.setTimestamp(3'000);

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  ASSERT_NE(layer_frame->pixels, nullptr);
  ASSERT_EQ(layer_frame->pixels->size(), 12u);

  const auto& pixels = *layer_frame->pixels;
  EXPECT_EQ(pixels[0], 0);  // min depth -> turbo blue endpoint
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[2], 255);
  EXPECT_EQ(pixels[8], 255);  // max depth -> turbo red endpoint
  EXPECT_EQ(pixels[9], 0);
  EXPECT_EQ(pixels[10], 0);
}

TEST(DepthPipelineSourceTest, AutoRangeAllInvalidUsesFallbackRangeAndStaysTransparent) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeU16Le({0, 0});
  const auto bytes = serializeDepth(2, 1, "16UC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 4'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setRange(2.0f, 2.0f);
  source.setAutoRange(true);
  source.setTimestamp(4'000);

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  ASSERT_NE(layer_frame->pixels, nullptr);
  ASSERT_EQ(layer_frame->pixels->size(), 8u);

  const auto& pixels = *layer_frame->pixels;
  EXPECT_EQ(pixels[3], 0);
  EXPECT_EQ(pixels[7], 0);
}

TEST(DepthPipelineSourceTest, JetColormapOrdersNearBlueFarRed) {
  ObjectStore store;
  auto topic = registerDepthTopic(store);
  ASSERT_NE(topic.id, 0u);

  const auto payload = makeU16Le({1000, 2000, 3000});
  const auto bytes = serializeDepth(3, 1, "16UC1", payload);
  ASSERT_TRUE(store.pushOwned(topic, 5'000, bytes).has_value());

  DepthPipelineSource source(&store, topic);
  source.setAutoRange(false);
  source.setRange(1.0f, 3.0f);
  source.setColormap(DepthColormap::kJet);
  source.setTimestamp(5'000);

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  const DecodedFrame* layer_frame = onlyPixelLayerFrame(*frame);
  ASSERT_NE(layer_frame, nullptr);
  ASSERT_NE(layer_frame->pixels, nullptr);
  ASSERT_EQ(layer_frame->pixels->size(), 12u);

  const auto& pixels = *layer_frame->pixels;
  EXPECT_LT(pixels[0], pixels[8]);   // red rises toward far depth
  EXPECT_GT(pixels[2], pixels[10]);  // blue falls toward far depth
  EXPECT_GT(pixels[5], pixels[1]);   // midpoint has the green peak
  EXPECT_GT(pixels[5], pixels[9]);
}

}  // namespace
}  // namespace PJ
