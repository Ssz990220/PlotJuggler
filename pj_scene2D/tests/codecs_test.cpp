// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/codecs.h"

#include <gtest/gtest.h>
#include <png.h>

#include <cstdint>
#include <cstring>
#include <string>
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

// CRC-32 (PNG/zlib polynomial) over a byte range — used to re-stamp a patched
// IHDR so png_read_info accepts our forged dimensions.
uint32_t crc32Png(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// Rewrite a valid PNG's IHDR width/height to forge a giant declared size, fixing
// the IHDR CRC so png_read_info still parses it. Layout: [8 sig][4 len][4 "IHDR"]
// [4 width][4 height]...[4 CRC]; the CRC covers "IHDR" + the 13 data bytes.
std::vector<uint8_t> pngWithPatchedDimensions(std::vector<uint8_t> png, uint32_t width, uint32_t height) {
  auto put_be = [&](size_t off, uint32_t v) {
    png[off + 0] = static_cast<uint8_t>(v >> 24);
    png[off + 1] = static_cast<uint8_t>(v >> 16);
    png[off + 2] = static_cast<uint8_t>(v >> 8);
    png[off + 3] = static_cast<uint8_t>(v);
  };
  put_be(16, width);
  put_be(20, height);
  put_be(29, crc32Png(png.data() + 12, 17));
  return png;
}

PJ::DecodedFrame mono8MosaicFrame(int width, int height, const std::vector<uint8_t>& mosaic) {
  PJ::DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(mosaic);
  frame.width = width;
  frame.height = height;
  frame.format = PJ::PixelFormat::kMono8;
  return frame;
}

// Build a width*height mosaic where each site holds the value mapped to the
// channel it samples under the given top-left 2x2 CFA pattern. site_for(r,c)
// returns 'R','G','B'. R=255, G=128, B=0 — a uniform per-channel field, so a
// correct demosaic reconstructs (255,128,0) at every interior pixel.
template <typename SiteFn>
std::vector<uint8_t> channelEncodedMosaic(int width, int height, SiteFn site_for) {
  std::vector<uint8_t> m(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      const char ch = site_for(r, c);
      m[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)] = (ch == 'R')   ? 255
                                                                                        : (ch == 'G') ? 128
                                                                                                      : 0;
    }
  }
  return m;
}

void expectRgb(const PJ::DecodedFrame& f, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(f.width) + static_cast<size_t>(x)) * 3;
  ASSERT_LT(i + 2, f.pixels->size());
  EXPECT_EQ((*f.pixels)[i + 0], r) << "R @ (" << x << "," << y << ")";
  EXPECT_EQ((*f.pixels)[i + 1], g) << "G @ (" << x << "," << y << ")";
  EXPECT_EQ((*f.pixels)[i + 2], b) << "B @ (" << x << "," << y << ")";
}

}  // namespace

TEST(PngCodecTest, RejectsAbsurdDimensionsBeforeAllocating) {
  // A real 2x2 PNG with its IHDR forged to 11000x11000 (121 MP > the 100 MP cap).
  // Without the cap the decoder value-initializes a ~240 MB buffer straight from
  // these header bytes; the cap must reject up front with a dimension error.
  auto small = makeMono16Png(2, 2, {0, 1000, 2000, 3000});
  ASSERT_GT(small.size(), 33u);
  auto huge = pngWithPatchedDimensions(small, 11000, 11000);

  PJ::PngCodec codec;
  auto result = codec.decode(rawBytesFrame(huge));
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("dimension"), std::string::npos) << "got: " << result.error();
}

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

TEST(BayerDecodeTest, RggbInteriorPixelsReconstructChannels) {
  // RGGB top-left tile:  R G / G B
  auto site = [](int r, int c) -> char {
    if (r % 2 == 0 && c % 2 == 0) {
      return 'R';
    }
    if (r % 2 == 1 && c % 2 == 1) {
      return 'B';
    }
    return 'G';
  };
  const std::vector<uint8_t> mosaic = channelEncodedMosaic(4, 4, site);

  PJ::BayerDecode codec(PJ::BayerPattern::kRGGB);
  auto out = codec.decode(mono8MosaicFrame(4, 4, mosaic));
  ASSERT_TRUE(out.has_value()) << out.error();

  EXPECT_EQ(out->width, 4);
  EXPECT_EQ(out->height, 4);
  EXPECT_EQ(out->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(out->pixels, nullptr);
  EXPECT_EQ(out->pixels->size(), 4u * 4u * 3u);

  // Interior pixels have a full neighbourhood, so the uniform-per-channel
  // field must round-trip exactly.
  expectRgb(*out, 1, 1, 255, 128, 0);
  expectRgb(*out, 2, 1, 255, 128, 0);
  expectRgb(*out, 1, 2, 255, 128, 0);
  expectRgb(*out, 2, 2, 255, 128, 0);
}

TEST(BayerDecodeTest, BggrPlacesRedAndBlueOppositeToRggb) {
  // BGGR top-left tile:  B G / G R  — same uniform-per-channel field, so a
  // pattern-aware decode must still yield (255,128,0) at interior pixels.
  auto site = [](int r, int c) -> char {
    if (r % 2 == 0 && c % 2 == 0) {
      return 'B';
    }
    if (r % 2 == 1 && c % 2 == 1) {
      return 'R';
    }
    return 'G';
  };
  const std::vector<uint8_t> mosaic = channelEncodedMosaic(4, 4, site);

  PJ::BayerDecode codec(PJ::BayerPattern::kBGGR);
  auto out = codec.decode(mono8MosaicFrame(4, 4, mosaic));
  ASSERT_TRUE(out.has_value()) << out.error();
  expectRgb(*out, 1, 1, 255, 128, 0);
  expectRgb(*out, 2, 2, 255, 128, 0);
}

// Guard against an out-of-bounds read in the 3x3 neighbourhood loop: a buffer
// smaller than width*height must be rejected, not demosaiced.
TEST(BayerDecodeTest, RejectsBufferTooSmallForDimensions) {
  auto out = PJ::BayerDecode(PJ::BayerPattern::kRGGB).decode(mono8MosaicFrame(4, 4, std::vector<uint8_t>(8, 0)));
  EXPECT_FALSE(out.has_value());
}
