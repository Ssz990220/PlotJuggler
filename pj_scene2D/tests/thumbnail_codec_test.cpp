// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/thumbnail_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

PJ::DecodedFrame makeYuv420(int w, int h, uint8_t fill) {
  PJ::DecodedFrame f;
  f.width = w;
  f.height = h;
  f.format = PJ::PixelFormat::kYUV420P;
  f.pixels = std::make_shared<std::vector<uint8_t>>(PJ::expectedBufferSize(w, h, PJ::PixelFormat::kYUV420P), fill);
  f.pts = 0;
  return f;
}

}  // namespace

TEST(ThumbnailCodec, EncodeDecodeRoundtripNoDownscale) {
  auto src = makeYuv420(64, 48, 0x80);  // <= 1280 wide -> no downscale
  auto thumb = PJ::encodeThumbnailJpeg(src, PJ::kThumbnailMaxWidth, PJ::kThumbnailJpegQuality);
  ASSERT_FALSE(thumb.jpeg.empty());
  EXPECT_EQ(thumb.width, 64);
  EXPECT_EQ(thumb.height, 48);

  auto frame = PJ::decodeThumbnailJpeg(thumb.jpeg.data(), thumb.jpeg.size(), thumb.width, thumb.height);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->width, 64);
  EXPECT_EQ(frame->height, 48);
  EXPECT_EQ(frame->format, PJ::PixelFormat::kYUV420P);
  EXPECT_TRUE(frame->isValid());
}

TEST(ThumbnailCodec, DownscalesAboveCap) {
  auto src = makeYuv420(3840, 2160, 0x40);  // 4K -> cap at 1280 wide
  auto thumb = PJ::encodeThumbnailJpeg(src, PJ::kThumbnailMaxWidth, PJ::kThumbnailJpegQuality);
  ASSERT_FALSE(thumb.jpeg.empty());
  EXPECT_EQ(thumb.width, 1280);
  EXPECT_EQ(thumb.height, 720);  // 2160 * 1280 / 3840

  auto frame = PJ::decodeThumbnailJpeg(thumb.jpeg.data(), thumb.jpeg.size(), thumb.width, thumb.height);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->width, 1280);
  EXPECT_EQ(frame->height, 720);
  EXPECT_TRUE(frame->isValid());
}

TEST(ThumbnailCodec, RejectsNonYuv420Input) {
  PJ::DecodedFrame rgb;
  rgb.width = 16;
  rgb.height = 16;
  rgb.format = PJ::PixelFormat::kRGB888;
  rgb.pixels = std::make_shared<std::vector<uint8_t>>(16 * 16 * 3, 0);
  auto thumb = PJ::encodeThumbnailJpeg(rgb, PJ::kThumbnailMaxWidth, PJ::kThumbnailJpegQuality);
  EXPECT_TRUE(thumb.jpeg.empty());
}
