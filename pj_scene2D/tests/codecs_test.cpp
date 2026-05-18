#include "pj_scene2d_core/codecs.h"

#include <gtest/gtest.h>
#include <png.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct PngWriteCtx {
  std::vector<uint8_t> bytes;
};

void pngWriteCallback(png_structp png, png_bytep data, png_size_t length) {
  auto* ctx = static_cast<PngWriteCtx*>(png_get_io_ptr(png));
  const auto* first = reinterpret_cast<const uint8_t*>(data);
  ctx->bytes.insert(ctx->bytes.end(), first, first + length);
}

void pngFlushCallback(png_structp /*png*/) {}

std::vector<uint8_t> makeMono16Png(int width, int height, const std::vector<uint16_t>& values) {
  PngWriteCtx ctx;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  EXPECT_NE(png, nullptr);
  png_infop info = png_create_info_struct(png);
  EXPECT_NE(info, nullptr);

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }

  png_set_write_fn(png, &ctx, pngWriteCallback, pngFlushCallback);
  png_set_IHDR(
      png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 16, PNG_COLOR_TYPE_GRAY,
      PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png, info);
  std::vector<uint8_t> big_endian_samples(values.size() * 2);
  for (size_t i = 0; i < values.size(); ++i) {
    big_endian_samples[i * 2 + 0] = static_cast<uint8_t>((values[i] >> 8) & 0xFF);
    big_endian_samples[i * 2 + 1] = static_cast<uint8_t>(values[i] & 0xFF);
  }
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] = big_endian_samples.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 2;
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return ctx.bytes;
}

PJ::DecodedFrame rawBytesFrame(const std::vector<uint8_t>& bytes) {
  PJ::DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(bytes);
  return frame;
}

}  // namespace

TEST(AutoImageCodecTest, NormalizesMono16PngToRgbGrayscale) {
  const std::vector<uint16_t> values = {0, 1000, 2000, 3000};
  const std::vector<uint8_t> png = makeMono16Png(2, 2, values);
  ASSERT_FALSE(png.empty());

  PJ::AutoImageCodec codec;
  auto decoded = codec.decode(rawBytesFrame(png));
  ASSERT_TRUE(decoded.has_value()) << decoded.error();

  EXPECT_EQ(decoded->width, 2);
  EXPECT_EQ(decoded->height, 2);
  EXPECT_EQ(decoded->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(decoded->pixels, nullptr);
  ASSERT_EQ(decoded->pixels->size(), 12U);

  const auto& pixels = *decoded->pixels;
  EXPECT_EQ(pixels[0], 0);  // invalid zero depth stays black
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[2], 0);
  EXPECT_EQ(pixels[3], 0);  // min non-zero maps to black
  EXPECT_EQ(pixels[4], 0);
  EXPECT_EQ(pixels[5], 0);
  EXPECT_EQ(pixels[6], 127);  // midpoint maps near half intensity
  EXPECT_EQ(pixels[7], 127);
  EXPECT_EQ(pixels[8], 127);
  EXPECT_EQ(pixels[9], 255);  // max maps to white
  EXPECT_EQ(pixels[10], 255);
  EXPECT_EQ(pixels[11], 255);
}
