// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/pixel_inspector.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

PJ::DecodedFrame makeFrame(PJ::PixelFormat format, int width, int height, std::vector<uint8_t> pixels) {
  PJ::DecodedFrame frame;
  frame.format = format;
  frame.width = width;
  frame.height = height;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
  return frame;
}

TEST(PixelInspectorMapping, CenterWithoutLetterboxMapsToImageCenter) {
  const auto point =
      PJ::widgetPointToImagePixel(QPointF(400.0, 300.0), QSize(800, 600), QSize(640, 480), 1.0f, 0.0f, 0.0f);

  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(*point, QPoint(320, 240));
}

TEST(PixelInspectorMapping, PillarboxRejectsMouseOutsideImage) {
  EXPECT_FALSE(
      PJ::widgetPointToImagePixel(QPointF(49.0, 50.0), QSize(200, 100), QSize(100, 100), 1.0f, 0.0f, 0.0f).has_value());

  const auto left_edge =
      PJ::widgetPointToImagePixel(QPointF(50.0, 50.0), QSize(200, 100), QSize(100, 100), 1.0f, 0.0f, 0.0f);
  ASSERT_TRUE(left_edge.has_value());
  EXPECT_EQ(*left_edge, QPoint(0, 50));
}

TEST(PixelInspectorMapping, ZoomAndPanUseSameTransformAsViewer) {
  const auto point =
      PJ::widgetPointToImagePixel(QPointF(150.0, 50.0), QSize(200, 200), QSize(100, 100), 2.0f, 0.25f, 0.25f);

  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(*point, QPoint(50, 50));
}

TEST(PixelInspectorPixels, ReadsRgbFamilyFormatsAsDisplayRgb) {
  const auto expect_pixel = [](PJ::PixelFormat format, std::vector<uint8_t> data, PJ::InspectorRgb expected) {
    const auto rgb = PJ::pixelRgbAt(makeFrame(format, 1, 1, std::move(data)), 0, 0);
    ASSERT_TRUE(rgb.has_value());
    EXPECT_EQ(*rgb, expected);
  };

  expect_pixel(PJ::PixelFormat::kRGB888, {1, 2, 3}, {1, 2, 3});
  expect_pixel(PJ::PixelFormat::kBGR888, {1, 2, 3}, {3, 2, 1});
  expect_pixel(PJ::PixelFormat::kRGBA8888, {1, 2, 3, 4}, {1, 2, 3});
  expect_pixel(PJ::PixelFormat::kBGRA8888, {1, 2, 3, 4}, {3, 2, 1});
}

TEST(PixelInspectorPixels, ReadsYuv420UsingShaderCompatibleConversion) {
  auto frame = makeFrame(
      PJ::PixelFormat::kYUV420P, 2, 2,
      {
          100, 100, 100, 100,  // Y
          128,                 // U
          128                  // V
      });

  const auto rgb = PJ::pixelRgbAt(frame, 1, 1);
  ASSERT_TRUE(rgb.has_value());
  EXPECT_NEAR(rgb->r, rgb->g, 2);
  EXPECT_NEAR(rgb->g, rgb->b, 2);
  EXPECT_NEAR(rgb->r, 100, 3);
}

TEST(PixelInspectorPixels, CropPadsOutsideImageWithBlack) {
  auto frame = makeFrame(
      PJ::PixelFormat::kRGB888, 2, 2,
      {
          10,
          20,
          30,
          40,
          50,
          60,
          70,
          80,
          90,
          100,
          110,
          120,
      });

  const auto crop = PJ::extractRgbCrop(frame, 0, 0, 3);
  ASSERT_EQ(crop.size(), 27U);
  EXPECT_EQ(crop[0], 0);
  EXPECT_EQ(crop[1], 0);
  EXPECT_EQ(crop[2], 0);

  const size_t center = 4U * 3U;
  EXPECT_EQ(crop[center], 10);
  EXPECT_EQ(crop[center + 1], 20);
  EXPECT_EQ(crop[center + 2], 30);
}

}  // namespace
