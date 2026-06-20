// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_scene3d_core/pointcloud_convert.h"

namespace {

using PJ::Span;
using pj::scene3d::AABB;
using pj::scene3d::AttribLayout;
using pj::scene3d::checkFastPath;
using pj::scene3d::convertCanonical;
using pj::scene3d::findField;
using pj::scene3d::readScalarAt;
using pj::scene3d::scanBoundsAndScalarRange;
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

std::vector<PointField> xyzFields() {
  return {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
}

template <typename Value>
void putValue(std::vector<uint8_t>& bytes, std::size_t offset, Value value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void putPoint(CloudFixture& fixture, std::size_t point_index, float x, float y, float z) {
  const std::size_t base = point_index * fixture.cloud.point_step;
  putValue(fixture.bytes, base + 0, x);
  putValue(fixture.bytes, base + 4, y);
  putValue(fixture.bytes, base + 8, z);
}

void expectAabbEq(const AABB& actual, const AABB& expected) {
  EXPECT_EQ(actual.valid, expected.valid);
  EXPECT_EQ(actual.min.x, expected.min.x);
  EXPECT_EQ(actual.min.y, expected.min.y);
  EXPECT_EQ(actual.min.z, expected.min.z);
  EXPECT_EQ(actual.max.x, expected.max.x);
  EXPECT_EQ(actual.max.y, expected.max.y);
  EXPECT_EQ(actual.max.z, expected.max.z);
}

void expectScannedBoundsMatchConvertCanonical(const PointCloud& cloud, const AttribLayout& layout) {
  const auto scan = scanBoundsAndScalarRange(cloud, layout, nullptr);
  const auto converted = convertCanonical(cloud, "");
  expectAabbEq(scan.bounds, converted.bounds);
  EXPECT_FALSE(scan.scalar_range.has_value());
}

std::optional<std::pair<float, float>> manualScalarRange(const PointCloud& cloud, const PointField& sf) {
  std::optional<std::pair<float, float>> range;
  const std::size_t point_count = static_cast<std::size_t>(cloud.width) * static_cast<std::size_t>(cloud.height);
  for (std::size_t point_index = 0; point_index < point_count; ++point_index) {
    const uint8_t* base = cloud.data.data() + point_index * cloud.point_step;
    const float value = readScalarAt(base + sf.offset, sf.datatype);
    if (!std::isfinite(value)) {
      continue;
    }
    if (range.has_value()) {
      range->first = std::min(range->first, value);
      range->second = std::max(range->second, value);
    } else {
      range = std::pair<float, float>{value, value};
    }
  }
  return range;
}

void expectScalarRangeMatchesManual(CloudFixture& fixture, std::string_view scalar_name) {
  const PointField* sf = findField(fixture.cloud.fields, scalar_name);
  ASSERT_NE(sf, nullptr);
  const auto layout = checkFastPath(fixture.cloud, scalar_name);
  ASSERT_TRUE(layout.has_value());

  const auto scan = scanBoundsAndScalarRange(fixture.cloud, *layout, sf);
  const auto manual = manualScalarRange(fixture.cloud, *sf);
  ASSERT_EQ(scan.scalar_range.has_value(), manual.has_value());
  if (manual.has_value()) {
    EXPECT_EQ(scan.scalar_range->first, manual->first);
    EXPECT_EQ(scan.scalar_range->second, manual->second);
  }
}

TEST(PointCloudBoundsScanEquivalence, BoundsMatchConvertCanonicalForNormalCloud) {
  CloudFixture fixture(/*width=*/3, /*height=*/1, /*point_step=*/12, xyzFields());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  putPoint(fixture, 0, 1.0f, 2.0f, 3.0f);
  putPoint(fixture, 1, -4.0f, 5.0f, 6.0f);
  putPoint(fixture, 2, nan, 100.0f, 100.0f);

  const auto layout = checkFastPath(fixture.cloud, "");
  ASSERT_TRUE(layout.has_value());
  expectScannedBoundsMatchConvertCanonical(fixture.cloud, *layout);
}

TEST(PointCloudBoundsScanEquivalence, BoundsMatchConvertCanonicalForSingleFinitePoint) {
  CloudFixture fixture(/*width=*/2, /*height=*/1, /*point_step=*/12, xyzFields());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  putPoint(fixture, 0, nan, nan, nan);
  putPoint(fixture, 1, 7.0f, 8.0f, 9.0f);

  const auto layout = checkFastPath(fixture.cloud, "");
  ASSERT_TRUE(layout.has_value());
  expectScannedBoundsMatchConvertCanonical(fixture.cloud, *layout);
}

TEST(PointCloudBoundsScanEquivalence, BoundsMatchConvertCanonicalForAllNanCloud) {
  CloudFixture fixture(/*width=*/2, /*height=*/1, /*point_step=*/12, xyzFields());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  putPoint(fixture, 0, nan, nan, nan);
  putPoint(fixture, 1, nan, nan, nan);

  const auto layout = checkFastPath(fixture.cloud, "");
  ASSERT_TRUE(layout.has_value());
  expectScannedBoundsMatchConvertCanonical(fixture.cloud, *layout);
}

TEST(PointCloudBoundsScanEquivalence, BoundsMatchConvertCanonicalForEmptyCloud) {
  CloudFixture fixture(/*width=*/0, /*height=*/1, /*point_step=*/12, xyzFields());
  const AttribLayout layout{.stride = 12, .xyz_offset = 0};
  expectScannedBoundsMatchConvertCanonical(fixture.cloud, layout);
}

TEST(PointCloudBoundsScanEquivalence, ScalarRangeMatchesManualReadScalarLoopForInt16) {
  CloudFixture fixture(
      /*width=*/3, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"intensity", 12, DT::kInt16, 1}});
  putPoint(fixture, 0, 0.0f, 0.0f, 0.0f);
  putPoint(fixture, 1, 1.0f, 1.0f, 1.0f);
  putPoint(fixture, 2, 2.0f, 2.0f, 2.0f);
  putValue<int16_t>(fixture.bytes, 12, -10);
  putValue<int16_t>(fixture.bytes, 28, 25);
  putValue<int16_t>(fixture.bytes, 44, 3);

  expectScalarRangeMatchesManual(fixture, "intensity");
}

TEST(PointCloudBoundsScanEquivalence, ScalarRangeMatchesManualReadScalarLoopForUint16) {
  CloudFixture fixture(
      /*width=*/3, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {"ring", 12, DT::kUint16, 1}});
  putPoint(fixture, 0, 0.0f, 0.0f, 0.0f);
  putPoint(fixture, 1, 1.0f, 1.0f, 1.0f);
  putPoint(fixture, 2, 2.0f, 2.0f, 2.0f);
  putValue<uint16_t>(fixture.bytes, 12, 2);
  putValue<uint16_t>(fixture.bytes, 28, 65500);
  putValue<uint16_t>(fixture.bytes, 44, 100);

  expectScalarRangeMatchesManual(fixture, "ring");
}

TEST(PointCloudBoundsScanEquivalence, ScalarRangeMatchesManualReadScalarLoopForFloat32) {
  CloudFixture fixture(
      /*width=*/3, /*height=*/1, /*point_step=*/16,
      {{"x", 0, DT::kFloat32, 1},
       {"y", 4, DT::kFloat32, 1},
       {"z", 8, DT::kFloat32, 1},
       {"temperature", 12, DT::kFloat32, 1}});
  const float nan = std::numeric_limits<float>::quiet_NaN();
  putPoint(fixture, 0, 0.0f, 0.0f, 0.0f);
  putPoint(fixture, 1, 1.0f, 1.0f, 1.0f);
  putPoint(fixture, 2, 2.0f, 2.0f, 2.0f);
  putValue<float>(fixture.bytes, 12, -1.5f);
  putValue<float>(fixture.bytes, 28, nan);
  putValue<float>(fixture.bytes, 44, 2.25f);

  expectScalarRangeMatchesManual(fixture, "temperature");
}

TEST(PointCloudBoundsScanEquivalence, NullScalarFieldReturnsNoScalarRange) {
  CloudFixture fixture(/*width=*/1, /*height=*/1, /*point_step=*/12, xyzFields());
  putPoint(fixture, 0, 1.0f, 2.0f, 3.0f);

  const auto layout = checkFastPath(fixture.cloud, "");
  ASSERT_TRUE(layout.has_value());
  const auto scan = scanBoundsAndScalarRange(fixture.cloud, *layout, nullptr);
  EXPECT_FALSE(scan.scalar_range.has_value());
}

}  // namespace
