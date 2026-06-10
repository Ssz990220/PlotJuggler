// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Unit tests for the camera-rectification math: computeUndistortMap (reverse
// undistortion map) and rectifyFrame (bilinear resample). The realistic case
// uses the Waymo CAMERA_FRONT plumb_bob calibration.

#include "pj_scene2d_core/undistort_remap.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "pj_scene2d_core/image_rectifier.h"

namespace PJ {
namespace {

// Waymo CAMERA_FRONT calibration (foxglove.CameraCalibration ground truth).
sdk::CameraInfo cameraFrontCalibration() {
  sdk::CameraInfo ci;
  ci.frame_id = "camera_front";
  ci.width = 1920;
  ci.height = 1280;
  ci.distortion_model = "plumb_bob";
  ci.D = {0.047117, -0.345292, 0.001674, -0.000927, 0.0};  // k1,k2,p1,p2,k3
  ci.K = {2069.819153, 0.0, 957.40706, 0.0, 2069.819153, 656.482744, 0.0, 0.0, 1.0};
  ci.R = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  ci.P = {2069.819153, 0.0, 957.40706, 0.0, 0.0, 2069.819153, 656.482744, 0.0, 0.0, 0.0, 1.0, 0.0};
  return ci;
}

TEST(UndistortRemapTest, IsRectifiableRequiresIntrinsicsAndSize) {
  EXPECT_TRUE(isRectifiable(cameraFrontCalibration()));

  sdk::CameraInfo no_fx = cameraFrontCalibration();
  no_fx.K[0] = 0.0;
  EXPECT_FALSE(isRectifiable(no_fx));

  sdk::CameraInfo no_size = cameraFrontCalibration();
  no_size.width = 0;
  EXPECT_FALSE(isRectifiable(no_size));
}

TEST(UndistortRemapTest, InvalidCalibrationYieldsInvalidMap) {
  sdk::CameraInfo bad;  // all-zero intrinsics
  const UndistortMap map = computeUndistortMap(bad, 640, 480, 640, 480);
  EXPECT_FALSE(map.valid());
}

TEST(UndistortRemapTest, MapHasExpectedShape) {
  const UndistortMap map = computeUndistortMap(cameraFrontCalibration(), 480, 320, 1920, 1280);
  ASSERT_TRUE(map.valid());
  EXPECT_EQ(map.out_width, 1920);
  EXPECT_EQ(map.out_height, 1280);
  EXPECT_EQ(map.src_x.size(), static_cast<size_t>(1920) * 1280);
  EXPECT_EQ(map.src_y.size(), map.src_x.size());
}

TEST(UndistortRemapTest, PrincipalPointMapsToItself) {
  const sdk::CameraInfo ci = cameraFrontCalibration();
  // Native-resolution output (src == calibration size): no rescale.
  const UndistortMap map = computeUndistortMap(ci, 1920, 1280, 1920, 1280);
  ASSERT_TRUE(map.valid());

  // At the principal point the normalized ray is (0,0): distortion is identity,
  // so the rectified principal pixel samples the source principal pixel.
  const int u = static_cast<int>(std::lround(ci.K[2]));  // cx
  const int v = static_cast<int>(std::lround(ci.K[5]));  // cy
  const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(map.out_width) + static_cast<size_t>(u);
  EXPECT_NEAR(map.src_x[idx], ci.K[2], 1.0);
  EXPECT_NEAR(map.src_y[idx], ci.K[5], 1.0);
}

TEST(UndistortRemapTest, CornersPullInwardForBarrelDistortion) {
  // k2 = -0.345 dominates: rectifying shrinks the edges, so each rectified
  // corner samples a source pixel pulled toward the image interior.
  const UndistortMap map = computeUndistortMap(cameraFrontCalibration(), 1920, 1280, 1920, 1280);
  ASSERT_TRUE(map.valid());
  const int w = map.out_width;
  const int h = map.out_height;

  const size_t tl = 0;
  const size_t br = static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(w - 1);

  EXPECT_GT(map.src_x[tl], 0.0F);                       // top-left x pulled right (inward)
  EXPECT_GT(map.src_y[tl], 0.0F);                       // top-left y pulled down (inward)
  EXPECT_LT(map.src_x[br], static_cast<float>(w - 1));  // bottom-right x pulled left
  EXPECT_LT(map.src_y[br], static_cast<float>(h - 1));  // bottom-right y pulled up

  // Every sampled coordinate stays essentially within the source image. The
  // distortion can map a few edge pixels a sub-pixel fraction beyond the border
  // (rectifyFrame renders those black via its bounds check), so allow a 1px
  // guard — but a gross error such as a missing resolution rescale (~4x) would
  // blow far past this.
  constexpr float kGuard = 1.0F;
  for (size_t i = 0; i < map.src_x.size(); ++i) {
    EXPECT_GE(map.src_x[i], -kGuard);
    EXPECT_LE(map.src_x[i], static_cast<float>(w) + kGuard);
    EXPECT_GE(map.src_y[i], -kGuard);
    EXPECT_LE(map.src_y[i], static_cast<float>(h) + kGuard);
  }
}

TEST(UndistortRemapTest, SourceScaledToDownsampledImage) {
  // Calibration at 1920x1280 but the decoded image is half-size: the principal
  // pixel's source coordinate must scale by 0.5.
  const sdk::CameraInfo ci = cameraFrontCalibration();
  const UndistortMap map = computeUndistortMap(ci, 960, 640, 1920, 1280);
  ASSERT_TRUE(map.valid());
  const int u = static_cast<int>(std::lround(ci.K[2]));
  const int v = static_cast<int>(std::lround(ci.K[5]));
  const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(map.out_width) + static_cast<size_t>(u);
  EXPECT_NEAR(map.src_x[idx], ci.K[2] * 0.5, 1.0);
  EXPECT_NEAR(map.src_y[idx], ci.K[5] * 0.5, 1.0);
}

TEST(UndistortRemapTest, MalformedProjectionFallsBackToIntrinsics) {
  // A P matrix with focal lengths but a zeroed principal point must not be used
  // (it would map the optical center to the corner); the math falls back to K,
  // so the principal point still maps to itself.
  sdk::CameraInfo ci = cameraFrontCalibration();
  ci.P = {2069.819153, 0.0, 0.0, 0.0, 0.0, 2069.819153, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0};
  const UndistortMap map = computeUndistortMap(ci, 1920, 1280, 1920, 1280);
  ASSERT_TRUE(map.valid());
  const int u = static_cast<int>(std::lround(ci.K[2]));
  const int v = static_cast<int>(std::lround(ci.K[5]));
  const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(map.out_width) + static_cast<size_t>(u);
  EXPECT_NEAR(map.src_x[idx], ci.K[2], 1.0);
  EXPECT_NEAR(map.src_y[idx], ci.K[5], 1.0);
}

// --- rectifyFrame -----------------------------------------------------------

DecodedFrame makeRgbFrame(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  DecodedFrame f;
  f.width = w;
  f.height = h;
  f.format = PixelFormat::kRGB888;
  f.frame_id = "cam";
  f.pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 3);
  for (int i = 0; i < w * h; ++i) {
    (*f.pixels)[static_cast<size_t>(i) * 3 + 0] = r;
    (*f.pixels)[static_cast<size_t>(i) * 3 + 1] = g;
    (*f.pixels)[static_cast<size_t>(i) * 3 + 2] = b;
  }
  return f;
}

// A pass-through map: every output pixel samples the same-position source pixel.
UndistortMap identityMap(int w, int h) {
  UndistortMap m;
  m.out_width = w;
  m.out_height = h;
  m.src_x.resize(static_cast<size_t>(w) * h);
  m.src_y.resize(m.src_x.size());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      m.src_x[i] = static_cast<float>(x);
      m.src_y[i] = static_cast<float>(y);
    }
  }
  return m;
}

TEST(ImageRectifierTest, IdentityMapReproducesSolidImage) {
  const DecodedFrame src = makeRgbFrame(8, 6, 10, 20, 30);
  const auto out = rectifyFrame(src, identityMap(8, 6));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->width, 8);
  EXPECT_EQ(out->height, 6);
  EXPECT_EQ(out->format, PixelFormat::kRGB888);
  EXPECT_EQ(out->frame_id, "cam");
  ASSERT_TRUE(out->isValid());
  // Interior pixels (whose 2x2 neighborhood is in bounds) keep the solid color.
  const auto& px = *out->pixels;
  const size_t center = (static_cast<size_t>(3) * 8 + 4) * 3;
  EXPECT_EQ(px[center + 0], 10);
  EXPECT_EQ(px[center + 1], 20);
  EXPECT_EQ(px[center + 2], 30);
}

TEST(ImageRectifierTest, OutOfBoundsSamplesRenderBlack) {
  const DecodedFrame src = makeRgbFrame(8, 6, 255, 255, 255);
  UndistortMap m = identityMap(8, 6);
  // Point the top-left output pixel far outside the source.
  m.src_x[0] = -100.0F;
  m.src_y[0] = -100.0F;
  const auto out = rectifyFrame(src, m);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ((*out->pixels)[0], 0);
  EXPECT_EQ((*out->pixels)[1], 0);
  EXPECT_EQ((*out->pixels)[2], 0);
}

TEST(ImageRectifierTest, UnsupportedFormatReturnsNullopt) {
  DecodedFrame src = makeRgbFrame(8, 6, 1, 2, 3);
  src.format = PixelFormat::kYUV420P;  // planar: not handled by the resampler
  // Re-size the buffer so isValid() passes for YUV420P before the format check.
  src.pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(8, 6, PixelFormat::kYUV420P));
  EXPECT_FALSE(rectifyFrame(src, identityMap(8, 6)).has_value());
}

TEST(ImageRectifierTest, InvalidMapReturnsNullopt) {
  const DecodedFrame src = makeRgbFrame(8, 6, 1, 2, 3);
  EXPECT_FALSE(rectifyFrame(src, UndistortMap{}).has_value());
}

TEST(UndistortRemapTest, MapRecordsSourceDimensions) {
  // The map bakes in the source resolution; recording it lets the per-camera
  // cache detect a stale map when the decoded frame size changes.
  const auto ci = cameraFrontCalibration();
  const auto map = computeUndistortMap(ci, 960, 640, static_cast<int>(ci.width), static_cast<int>(ci.height));
  ASSERT_TRUE(map.valid());
  EXPECT_EQ(map.src_width, 960);
  EXPECT_EQ(map.src_height, 640);
}

TEST(UndistortRemapTest, DifferentSourceSizeGivesDifferentSampling) {
  // Same camera, two decoded resolutions -> the source-sampling coords scale with
  // the decoded size. Reusing a map built for the wrong size (the bug B3 fixes)
  // would sample the wrong source region; this is why the cache must key on size.
  const auto ci = cameraFrontCalibration();
  const auto small = computeUndistortMap(ci, 480, 320, static_cast<int>(ci.width), static_cast<int>(ci.height));
  const auto big = computeUndistortMap(ci, 960, 640, static_cast<int>(ci.width), static_cast<int>(ci.height));
  ASSERT_TRUE(small.valid());
  ASSERT_TRUE(big.valid());
  const size_t idx = static_cast<size_t>(big.out_height / 2) * big.out_width + big.out_width / 2;
  // The 2x-larger source maps the same output pixel to ~2x the source coordinate.
  EXPECT_NEAR(big.src_x[idx], small.src_x[idx] * 2.0F, 1.0F);
  EXPECT_NEAR(big.src_y[idx], small.src_y[idx] * 2.0F, 1.0F);
}

}  // namespace
}  // namespace PJ
