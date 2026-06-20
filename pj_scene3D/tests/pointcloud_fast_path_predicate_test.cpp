// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_scene3d_core/pointcloud_convert.h"

namespace {

using PJ::Span;
using pj::scene3d::checkFastPath;
using pj::scene3d::kGlFloat;
using pj::scene3d::kGlShort;
using pj::scene3d::kGlUnsignedInt;
using pj::scene3d::kGlUnsignedShort;
using pj::scene3d::readScalarAt;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

struct CloudFixture {
  std::vector<uint8_t> bytes;
  PointCloud cloud;

  CloudFixture(uint32_t width, uint32_t height, uint32_t point_step, std::vector<PointField> fields)
      : bytes(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(point_step),
            0) {
    cloud.width = width;
    cloud.height = height;
    cloud.point_step = point_step;
    cloud.row_step = width * point_step;
    cloud.is_bigendian = false;
    cloud.frame_id = "lidar";
    cloud.fields = std::move(fields);
    rebind();
  }

  CloudFixture(const CloudFixture&) = delete;
  CloudFixture& operator=(const CloudFixture&) = delete;

  CloudFixture(CloudFixture&& other) noexcept : bytes(std::move(other.bytes)), cloud(std::move(other.cloud)) {
    rebind();
  }

  CloudFixture& operator=(CloudFixture&& other) noexcept {
    bytes = std::move(other.bytes);
    cloud = std::move(other.cloud);
    rebind();
    return *this;
  }

  void rebind() {
    cloud.data = Span<const uint8_t>(bytes.data(), bytes.size());
  }
};

std::vector<PointField> xyzFields(
    DT datatype = DT::kFloat32, uint32_t x_offset = 0, uint32_t y_offset = 4, uint32_t z_offset = 8) {
  return {
      {"x", x_offset, datatype, 1},
      {"y", y_offset, datatype, 1},
      {"z", z_offset, datatype, 1},
  };
}

void putInt16(std::vector<uint8_t>& bytes, std::size_t offset, int16_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

TEST(PointCloudFastPathPredicate, AcceptsContiguousFloat32XyzWithoutScalar) {
  CloudFixture fixture(
      /*width=*/2, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kFloat32, 1}});

  const auto layout = checkFastPath(fixture.cloud, "");
  ASSERT_TRUE(layout.has_value());
  EXPECT_EQ(layout->stride, 16u);
  EXPECT_EQ(layout->xyz_offset, 0u);
  EXPECT_EQ(layout->scalar_offset, 0u);
  EXPECT_EQ(layout->scalar_gl_type, 0u);
  EXPECT_FALSE(layout->has_scalar);

  const auto x_axis_layout = checkFastPath(fixture.cloud, "x");
  ASSERT_TRUE(x_axis_layout.has_value());
  EXPECT_FALSE(x_axis_layout->has_scalar);
  EXPECT_EQ(x_axis_layout->scalar_gl_type, 0u);

  const auto y_axis_layout = checkFastPath(fixture.cloud, "y");
  ASSERT_TRUE(y_axis_layout.has_value());
  EXPECT_FALSE(y_axis_layout->has_scalar);
  EXPECT_EQ(y_axis_layout->scalar_gl_type, 0u);

  const auto z_axis_layout = checkFastPath(fixture.cloud, "z");
  ASSERT_TRUE(z_axis_layout.has_value());
  EXPECT_FALSE(z_axis_layout->has_scalar);
  EXPECT_EQ(z_axis_layout->scalar_gl_type, 0u);
}

TEST(PointCloudFastPathPredicate, RejectsUnsupportedSpatialLayouts) {
  EXPECT_FALSE(checkFastPath(CloudFixture(1, 1, 24, xyzFields(DT::kFloat64, 0, 8, 16)).cloud, "").has_value());
  EXPECT_FALSE(checkFastPath(CloudFixture(1, 1, 6, xyzFields(DT::kInt16, 0, 2, 4)).cloud, "").has_value());

  CloudFixture big_endian(1, 1, 12, xyzFields());
  big_endian.cloud.is_bigendian = true;
  EXPECT_FALSE(checkFastPath(big_endian.cloud, "").has_value());

  CloudFixture missing_z(1, 1, 8, {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}});
  EXPECT_FALSE(checkFastPath(missing_z.cloud, "").has_value());

  EXPECT_FALSE(checkFastPath(CloudFixture(1, 1, 16, xyzFields(DT::kFloat32, 0, 8, 12)).cloud, "").has_value());

  CloudFixture row_padded(/*width=*/2, /*height=*/2, /*point_step=*/12, xyzFields());
  row_padded.cloud.row_step = row_padded.cloud.width * row_padded.cloud.point_step + 4;
  EXPECT_FALSE(checkFastPath(row_padded.cloud, "").has_value());
}

TEST(PointCloudFastPathPredicate, AcceptsNativeScalarTypes) {
  CloudFixture uint16_scalar(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kUint16, 1}});
  const auto uint16_layout = checkFastPath(uint16_scalar.cloud, "intensity");
  ASSERT_TRUE(uint16_layout.has_value());
  EXPECT_TRUE(uint16_layout->has_scalar);
  EXPECT_EQ(uint16_layout->scalar_offset, 12u);
  EXPECT_EQ(uint16_layout->scalar_gl_type, kGlUnsignedShort);

  CloudFixture uint32_scalar(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"ring", 12, DT::kUint32, 1}});
  const auto uint32_layout = checkFastPath(uint32_scalar.cloud, "ring");
  ASSERT_TRUE(uint32_layout.has_value());
  EXPECT_TRUE(uint32_layout->has_scalar);
  EXPECT_EQ(uint32_layout->scalar_gl_type, kGlUnsignedInt);

  CloudFixture int16_scalar(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kInt16, 1}});
  putInt16(int16_scalar.bytes, 12, -1234);
  EXPECT_FLOAT_EQ(readScalarAt(int16_scalar.cloud.data.data() + 12, DT::kInt16), -1234.0f);

  const auto int16_layout = checkFastPath(int16_scalar.cloud, "intensity");
  ASSERT_TRUE(int16_layout.has_value());
  EXPECT_TRUE(int16_layout->has_scalar);
  EXPECT_EQ(int16_layout->scalar_gl_type, kGlShort);
}

TEST(PointCloudFastPathPredicate, RejectsUnsupportedScalarTypes) {
  CloudFixture float64_scalar(
      /*width=*/1, /*height=*/1, /*point_step=*/24,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 16, DT::kFloat64, 1}});
  EXPECT_FALSE(checkFastPath(float64_scalar.cloud, "intensity").has_value());

  CloudFixture unknown_scalar(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kUnknown, 1}});
  EXPECT_FALSE(checkFastPath(unknown_scalar.cloud, "intensity").has_value());

  CloudFixture missing_scalar(/*width=*/1, /*height=*/1, /*point_step=*/12, xyzFields());
  EXPECT_FALSE(checkFastPath(missing_scalar.cloud, "intensity").has_value());
}

TEST(PointCloudFastPathPredicate, AcceptsFloat32Scalar) {
  CloudFixture fixture(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kFloat32, 1}});

  const auto layout = checkFastPath(fixture.cloud, "intensity");
  ASSERT_TRUE(layout.has_value());
  EXPECT_TRUE(layout->has_scalar);
  EXPECT_EQ(layout->scalar_gl_type, kGlFloat);
}

TEST(PointCloudFastPathPredicate, RgbModeAcceptsPackedRgbaField) {
  // Canonical packed rgba: one uint32 field (4 contiguous bytes R,G,B,A) -> fast path with
  // the colour attribute at the field offset; scalar stays off (RGB ignores the colormap).
  CloudFixture fixture(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"rgba", 12, DT::kUint32, 1}});

  const auto layout = checkFastPath(fixture.cloud, "", /*want_rgba=*/true);
  ASSERT_TRUE(layout.has_value());
  EXPECT_TRUE(layout->has_color);
  EXPECT_EQ(layout->color_offset, 12u);
  EXPECT_FALSE(layout->has_scalar);

  // Packed "rgb" (no alpha) is equally bindable — the shader paints only .rgb.
  CloudFixture rgb(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"rgb", 12, DT::kUint32, 1}});
  const auto rgb_layout = checkFastPath(rgb.cloud, "", /*want_rgba=*/true);
  ASSERT_TRUE(rgb_layout.has_value());
  EXPECT_TRUE(rgb_layout->has_color);
  EXPECT_EQ(rgb_layout->color_offset, 12u);
}

TEST(PointCloudFastPathPredicate, RgbModeRejectsNonContiguousAndMissingColour) {
  // No colour field at all -> nullopt (falls back to the CPU path / non-RGB rendering).
  CloudFixture none(
      /*width=*/1, /*height=*/1, /*point_step=*/12,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}});
  EXPECT_FALSE(checkFastPath(none.cloud, "", /*want_rgba=*/true).has_value());

  // Separate, non-contiguous r/g/b uint8 channels -> a single vec4 attrib can't read them;
  // fall back to convertCanonical's CPU rgba packer.
  CloudFixture scattered(
      /*width=*/1, /*height=*/1, /*point_step=*/24,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"red", 12, DT::kUint8, 1},
       {"green", 16, DT::kUint8, 1},
       {"blue", 20, DT::kUint8, 1}});
  EXPECT_FALSE(checkFastPath(scattered.cloud, "", /*want_rgba=*/true).has_value());
}

TEST(PointCloudFastPathPredicate, RgbFastPathColourMatchesConvertCanonical) {
  // The fast path binds the wire's 4 colour bytes at color_offset directly; the CPU fallback packs
  // them via convertCanonical(extract_rgba). On a little-endian host the wire uint32 at
  // color_offset must equal the packed value — the proof the fast path is pixel-identical.
  CloudFixture fixture(
      /*width=*/1, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"rgba", 12, DT::kUint32, 1}});
  fixture.bytes[12] = 0x10;  // R
  fixture.bytes[13] = 0x20;  // G
  fixture.bytes[14] = 0x30;  // B
  fixture.bytes[15] = 0xff;  // A

  const auto layout = checkFastPath(fixture.cloud, "", /*want_rgba=*/true);
  ASSERT_TRUE(layout.has_value());
  ASSERT_TRUE(layout->has_color);

  uint32_t wire_rgba = 0;
  std::memcpy(&wire_rgba, fixture.cloud.data.data() + layout->color_offset, sizeof(wire_rgba));

  const auto converted = pj::scene3d::convertCanonical(fixture.cloud, "", /*extract_rgba=*/true);
  ASSERT_EQ(converted.cloud.rgba.size(), 1u);
  EXPECT_EQ(wire_rgba, converted.cloud.rgba[0]);  // fast-path bytes == CPU-packed rgba
  EXPECT_EQ(wire_rgba & 0xFFu, 0x10u);            // R is the low byte (byte0)
}

}  // namespace
