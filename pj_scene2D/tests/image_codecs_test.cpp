// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <png.h>
#include <turbojpeg.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "pj_scene2d_core/codecs.h"

namespace PJ {
namespace {

std::vector<uint8_t> createTestJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width * height * 3));
  for (size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 255;
    rgb[i + 1] = 0;
    rgb[i + 2] = 0;
  }

  tjhandle compressor = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;  // NOLINT(google-runtime-int)
  tjCompress2(
      compressor, rgb.data(), width, width * 3, height, TJPF_RGB, &jpeg_buf, &jpeg_size, TJSAMP_420, 80,
      TJFLAG_FASTUPSAMPLE);
  std::vector<uint8_t> result(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  tjDestroy(compressor);
  return result;
}

struct PngWriteContext {
  std::vector<uint8_t> data;
};

void pngWriteCallback(png_structp png, png_bytep buf, png_size_t count) {
  auto* ctx = static_cast<PngWriteContext*>(png_get_io_ptr(png));
  ctx->data.insert(ctx->data.end(), buf, buf + count);
}

std::vector<uint8_t> createTestPng(int width, int height, bool with_alpha) {
  int channels = with_alpha ? 4 : 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(width * height * channels));
  for (size_t i = 0; i < pixels.size(); i += static_cast<size_t>(channels)) {
    pixels[i] = 0;
    pixels[i + 1] = 255;
    pixels[i + 2] = 0;
    if (with_alpha) {
      pixels[i + 3] = 128;
    }
  }

  PngWriteContext ctx;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png_create_info_struct(png);
  png_set_write_fn(png, &ctx, pngWriteCallback, nullptr);

  png_set_IHDR(
      png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
      with_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
      PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] = pixels.data() + static_cast<size_t>(y * width * channels);
  }
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return ctx.data;
}

DecodedFrame compressedFrame(const std::vector<uint8_t>& bytes) {
  DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(bytes);
  return frame;
}

// --- JPEG tests ---

TEST(ImageCodecTest, DecodeValidJpeg) {
  ImageDecodeCascade decoder;
  auto jpeg = createTestJpeg(64, 48);
  auto result = decoder.decode(compressedFrame(jpeg));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->width, 64);
  EXPECT_EQ(result->height, 48);
  EXPECT_EQ(result->format, PixelFormat::kRGB888);
  EXPECT_FALSE(result->isNull());
  EXPECT_GT((*result->pixels)[0], 200);
  EXPECT_LT((*result->pixels)[1], 50);
}

TEST(ImageCodecTest, DecodeEmptyInputFails) {
  JpegCodec decoder;
  auto result = decoder.decode({});
  EXPECT_FALSE(result.has_value());
}

TEST(ImageCodecTest, DecodeCorruptInputFails) {
  JpegCodec decoder;
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xD8, 0xFF, 0xE0};
  auto result = decoder.decode(compressedFrame(garbage));
  EXPECT_FALSE(result.has_value());
}

// --- PNG tests ---

TEST(ImageCodecTest, DecodePngRgb) {
  PngCodec decoder;
  auto png = createTestPng(32, 24, false);
  auto result = decoder.decode(compressedFrame(png));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->width, 32);
  EXPECT_EQ(result->height, 24);
  EXPECT_EQ(result->format, PixelFormat::kRGB888);
  EXPECT_LT((*result->pixels)[0], 10);
  EXPECT_GT((*result->pixels)[1], 240);
  EXPECT_LT((*result->pixels)[2], 10);
}

TEST(ImageCodecTest, DecodePngRgba) {
  PngCodec decoder;
  auto png = createTestPng(16, 16, true);
  auto result = decoder.decode(compressedFrame(png));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->format, PixelFormat::kRGBA8888);
  EXPECT_EQ((*result->pixels)[3], 128);
}

TEST(ImageCodecTest, DecodePngEmptyFails) {
  PngCodec decoder;
  auto result = decoder.decode({});
  EXPECT_FALSE(result.has_value());
}

TEST(ImageCodecTest, DecodePngCorruptFails) {
  PngCodec decoder;
  std::vector<uint8_t> garbage = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00};
  auto result = decoder.decode(compressedFrame(garbage));
  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace PJ
