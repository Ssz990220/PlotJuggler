// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/depth_range.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace PJ {
namespace {

TEST(DepthPercentileRange, EmptyOrAllInvalidIsNullopt) {
  EXPECT_FALSE(depthPercentileRange(nullptr, 0).has_value());
  const std::vector<float> zeros(100, 0.0f);  // 0 == no-data sentinel
  EXPECT_FALSE(depthPercentileRange(zeros.data(), zeros.size()).has_value());
  const std::vector<float> nans(10, std::numeric_limits<float>::quiet_NaN());
  EXPECT_FALSE(depthPercentileRange(nans.data(), nans.size()).has_value());
}

TEST(DepthPercentileRange, IgnoresZeroNanInf) {
  std::vector<float> data = {
      0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 2.0f, 4.0f};
  const auto r = depthPercentileRange(data.data(), data.size(), 0.0f, 1.0f);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->min, 2.0f, 1e-5f);  // min of the two valid samples
  EXPECT_NEAR(r->max, 4.0f, 1e-5f);  // max of the two valid samples
}

TEST(DepthPercentileRange, PercentilesClipOutliers) {
  // 1..100 metres; the 2nd/98th percentile must exclude the extreme tails.
  std::vector<float> data;
  data.reserve(100);
  for (int i = 1; i <= 100; ++i) {
    data.push_back(static_cast<float>(i));
  }
  const auto r = depthPercentileRange(data.data(), data.size(), 0.02f, 0.98f);
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->min, 1.0f);    // clipped away from the bottom outlier
  EXPECT_LT(r->max, 100.0f);  // clipped away from the top outlier
  EXPECT_LT(r->min, r->max);
}

TEST(DepthPercentileRange, FlatImageIsNotDegenerate) {
  const std::vector<float> flat(50, 3.0f);
  const auto r = depthPercentileRange(flat.data(), flat.size());
  ASSERT_TRUE(r.has_value());
  EXPECT_LT(r->min, r->max);  // upper bound nudged so far > near
}

}  // namespace
}  // namespace PJ
