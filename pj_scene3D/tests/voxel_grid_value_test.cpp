// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/voxel_grid_value.h"

#include <gtest/gtest.h>

#include <cmath>

namespace pj::scene3d {
namespace {

TEST(VoxelDrawMode, All) {
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kAll, 0.0f, 0.0f, 0.0f, 0.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kAll, -5.0f, 0.0f, 0.0f, 0.0f));
}

TEST(VoxelDrawMode, NonZero) {
  EXPECT_FALSE(voxelShouldDraw(VoxelDrawMode::kNonZero, 0.0f, 0.0f, 0.0f, 0.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kNonZero, 0.001f, 0.0f, 0.0f, 0.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kNonZero, -1.0f, 0.0f, 0.0f, 0.0f));
}

TEST(VoxelDrawMode, ThresholdInclusiveLowerBound) {
  // Draw where value >= threshold; the boundary value itself is drawn.
  EXPECT_FALSE(voxelShouldDraw(VoxelDrawMode::kThreshold, 49.0f, 50.0f, 0.0f, 0.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kThreshold, 50.0f, 50.0f, 0.0f, 0.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kThreshold, 51.0f, 50.0f, 0.0f, 0.0f));
}

TEST(VoxelDrawMode, RangeInclusiveBothEnds) {
  EXPECT_FALSE(voxelShouldDraw(VoxelDrawMode::kRange, 0.9f, 0.0f, 1.0f, 2.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kRange, 1.0f, 0.0f, 1.0f, 2.0f));
  EXPECT_TRUE(voxelShouldDraw(VoxelDrawMode::kRange, 2.0f, 0.0f, 1.0f, 2.0f));
  EXPECT_FALSE(voxelShouldDraw(VoxelDrawMode::kRange, 2.1f, 0.0f, 1.0f, 2.0f));
}

TEST(VoxelNormalize, MapsRangeToUnitInterval) {
  EXPECT_FLOAT_EQ(voxelNormalize(0.0f, 0.0f, 100.0f), 0.0f);
  EXPECT_FLOAT_EQ(voxelNormalize(50.0f, 0.0f, 100.0f), 0.5f);
  EXPECT_FLOAT_EQ(voxelNormalize(100.0f, 0.0f, 100.0f), 1.0f);
}

TEST(VoxelNormalize, ClampsOutsideRange) {
  EXPECT_FLOAT_EQ(voxelNormalize(-10.0f, 0.0f, 100.0f), 0.0f);
  EXPECT_FLOAT_EQ(voxelNormalize(200.0f, 0.0f, 100.0f), 1.0f);
}

TEST(VoxelNormalize, DegenerateSpanIsZeroNeverNaN) {
  EXPECT_FLOAT_EQ(voxelNormalize(5.0f, 1.0f, 1.0f), 0.0f);  // zero span
  EXPECT_FLOAT_EQ(voxelNormalize(5.0f, 2.0f, 1.0f), 0.0f);  // inverted span
  const float nan = std::nanf("");
  EXPECT_FALSE(std::isnan(voxelNormalize(5.0f, 0.0f, nan)));
}

}  // namespace
}  // namespace pj::scene3d
