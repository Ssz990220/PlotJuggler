// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Direct unit tests for the raw PointCloud byte readers in pointcloud_convert.
// These parse arbitrary little-endian field layouts straight from untrusted bytes,
// so the coverage targets sign extension, float64 truncation, and — crucially — the
// offset/buffer validation that is the renderer's last line of defense against an
// out-of-bounds heap read on a malformed/hostile cloud (M.23/M.24).

#include "pj_scene3d_core/pointcloud_convert.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "pj_base/span.hpp"

namespace {

using PJ::Span;
using pj::scene3d::convertCanonical;
using pj::scene3d::ConvertedPointCloud;
using pj::scene3d::readFloat32At;
using pj::scene3d::readFloat64At;
using pj::scene3d::readScalarAt;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

// --- Little-endian byte writers -------------------------------------------------

void putBytes(std::vector<uint8_t>& buffer, const void* src, std::size_t count) {
  const auto* bytes = static_cast<const uint8_t*>(src);
  buffer.insert(buffer.end(), bytes, bytes + count);
}

template <typename Scalar>
std::vector<uint8_t> leBytes(Scalar value) {
  // The host running the test suite is little-endian (x86); std::bit-pattern of the
  // native value already matches the wire layout the readers assemble byte-by-byte.
  std::vector<uint8_t> out(sizeof(Scalar));
  std::memcpy(out.data(), &value, sizeof(Scalar));
  return out;
}

// Wrap an owned byte buffer into a PointCloud, anchoring the bytes so the Span stays
// valid for the cloud's lifetime.
PointCloud makeCloud(uint32_t width, uint32_t point_step, std::vector<PointField> fields, std::vector<uint8_t> bytes) {
  auto owned = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  PointCloud cloud;
  cloud.width = width;
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = point_step * width;
  cloud.is_bigendian = false;
  cloud.frame_id = "lidar";
  cloud.fields = std::move(fields);
  cloud.data = Span<const uint8_t>(owned->data(), owned->size());
  cloud.anchor = owned;
  return cloud;
}

// --- Raw scalar reader: sign extension and float decode -------------------------

TEST(PointCloudConvertScalar, Int8SignExtends) {
  const auto bytes = leBytes<int8_t>(-5);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt8), -5.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint8), 251.0f);  // same byte read unsigned
}

TEST(PointCloudConvertScalar, Int16SignExtends) {
  const auto bytes = leBytes<int16_t>(-1234);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt16), -1234.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint16), 64302.0f);  // 65536 - 1234
}

TEST(PointCloudConvertScalar, Int32SignExtends) {
  const auto bytes = leBytes<int32_t>(-70000);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kInt32), -70000.0f);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUint32), 4294897296.0f);  // 2^32 - 70000
}

TEST(PointCloudConvertScalar, Float32And64) {
  const auto f32 = leBytes<float>(3.5f);
  EXPECT_FLOAT_EQ(readFloat32At(f32.data()), 3.5f);
  EXPECT_FLOAT_EQ(readScalarAt(f32.data(), DT::kFloat32), 3.5f);

  const auto f64 = leBytes<double>(2.718281828459045);
  EXPECT_DOUBLE_EQ(readFloat64At(f64.data()), 2.718281828459045);
  // float64 -> float32 truncation: the scalar reader narrows to float.
  EXPECT_FLOAT_EQ(readScalarAt(f64.data(), DT::kFloat64), static_cast<float>(2.718281828459045));
}

TEST(PointCloudConvertScalar, UnknownDatatypeYieldsZero) {
  const std::vector<uint8_t> bytes(8, 0xFF);
  EXPECT_FLOAT_EQ(readScalarAt(bytes.data(), DT::kUnknown), 0.0f);
}

// --- convertCanonical: happy path -----------------------------------------------

TEST(PointCloudConvert, DecodesXyzAndIntScalarWithSignExtension) {
  // Layout per point (point_step = 16): x f32, y f32, z f32, intensity int16 (+ 2 pad).
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 12, DT::kInt16, 1},
  };
  std::vector<uint8_t> bytes;
  auto push_point = [&](float x, float y, float z, int16_t scalar) {
    putBytes(bytes, leBytes(x).data(), 4);
    putBytes(bytes, leBytes(y).data(), 4);
    putBytes(bytes, leBytes(z).data(), 4);
    putBytes(bytes, leBytes(scalar).data(), 2);
    bytes.insert(bytes.end(), {0, 0});  // 2 bytes padding to reach point_step 16
  };
  push_point(1.0f, 2.0f, 3.0f, -100);
  push_point(-4.0f, 5.0f, 6.0f, 200);

  const PointCloud cloud = makeCloud(/*width=*/2, /*point_step=*/16, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");

  ASSERT_EQ(out.cloud.positions.size(), 2u);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].x, 1.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].y, 2.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].z, 3.0f);
  EXPECT_FLOAT_EQ(out.cloud.positions[1].x, -4.0f);

  ASSERT_EQ(out.cloud.scalar.size(), 2u);
  EXPECT_FLOAT_EQ(out.cloud.scalar[0], -100.0f);  // signed read, not 65436
  EXPECT_FLOAT_EQ(out.cloud.scalar[1], 200.0f);
  EXPECT_EQ(out.cloud.scalar_field_name, "intensity");
  EXPECT_EQ(out.cloud.frame_id, "lidar");

  // AABB accumulated in the same pass spans both points (L.50).
  ASSERT_TRUE(out.bounds.valid);
  EXPECT_FLOAT_EQ(out.bounds.min.x, -4.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.x, 1.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.z, 6.0f);
}

TEST(PointCloudConvert, Float64Coordinates) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat64, 1},
      {"y", 8, DT::kFloat64, 1},
      {"z", 16, DT::kFloat64, 1},
  };
  std::vector<uint8_t> bytes;
  putBytes(bytes, leBytes<double>(1.25).data(), 8);
  putBytes(bytes, leBytes<double>(-2.5).data(), 8);
  putBytes(bytes, leBytes<double>(100.0).data(), 8);

  const PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/24, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "");

  ASSERT_EQ(out.cloud.positions.size(), 1u);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].x, 1.25f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].y, -2.5f);
  EXPECT_FLOAT_EQ(out.cloud.positions[0].z, 100.0f);
  EXPECT_TRUE(out.cloud.scalar.empty());
}

TEST(PointCloudConvert, NonFinitePointsExcludedFromBounds) {
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
  };
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<uint8_t> bytes;
  auto push_point = [&](float x, float y, float z) {
    putBytes(bytes, leBytes(x).data(), 4);
    putBytes(bytes, leBytes(y).data(), 4);
    putBytes(bytes, leBytes(z).data(), 4);
  };
  push_point(nan, nan, nan);     // dropped from AABB
  push_point(7.0f, 8.0f, 9.0f);  // sole finite point defines the box

  const PointCloud cloud = makeCloud(/*width=*/2, /*point_step=*/12, std::move(fields), std::move(bytes));
  const ConvertedPointCloud out = convertCanonical(cloud, "");

  ASSERT_EQ(out.cloud.positions.size(), 2u);  // both points still decoded into positions
  ASSERT_TRUE(out.bounds.valid);
  EXPECT_FLOAT_EQ(out.bounds.min.x, 7.0f);
  EXPECT_FLOAT_EQ(out.bounds.max.x, 7.0f);
}

// --- convertCanonical: rejection (returns empty) --------------------------------

TEST(PointCloudConvert, RejectsBigEndian) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(12, 0));
  cloud.is_bigendian = true;

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
  EXPECT_FALSE(out.bounds.valid);
}

TEST(PointCloudConvert, RejectsBufferTooSmallForDeclaredPoints) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  // width=4 points * point_step=12 wants 48 bytes; supply only 24.
  PointCloud cloud = makeCloud(/*width=*/4, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(24, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsMissingSpatialField) {
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}};  // no z
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/8, std::move(fields), std::vector<uint8_t>(8, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsFieldOffsetPastPointStep) {
  // M.23/M.24: z declares a float32 at offset 8 with point_step=10 -> read crosses
  // the point boundary (8 + 4 > 10). Must be rejected BEFORE any read.
  std::vector<PointField> fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/10, std::move(fields), std::vector<uint8_t>(10, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, RejectsScalarOffsetPastPointStep) {
  // x/y/z fit (12 bytes), but the selected scalar at offset 14 reads past point_step=16.
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 14, DT::kFloat32, 1},  // 14 + 4 = 18 > 16
  };
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/16, std::move(fields), std::vector<uint8_t>(16, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");
  EXPECT_TRUE(out.cloud.positions.empty());
}

TEST(PointCloudConvert, MalformedScalarOffsetDoesNotPoisonHugeAdversarialValue) {
  // An adversarial uint32 offset (4 GiB) would index arbitrarily far out of bounds;
  // the offset+size computed in uint64 must still reject without overflowing.
  std::vector<PointField> fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 0xFFFFFFFCu, DT::kFloat32, 1},  // offset+4 wraps uint32 if not widened
  };
  PointCloud cloud = makeCloud(/*width=*/1, /*point_step=*/12, std::move(fields), std::vector<uint8_t>(12, 0));

  const ConvertedPointCloud out = convertCanonical(cloud, "intensity");
  EXPECT_TRUE(out.cloud.positions.empty());
}

}  // namespace
