// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Pins the order-preserving float<->uint32 bijection used by the GPU AABB reducer.
// The GLSL shader mirrors floatToOrderedKey() exactly, so if these properties hold
// on the CPU side and the shader expression matches, atomicMin/atomicMax over the
// keys reduce the underlying floats correctly.

#include "pj_scene3d_core/aabb_gpu_key.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using pj::scene3d::floatToOrderedKey;
using pj::scene3d::orderedKeyToFloat;

// A spread of representative finite values plus +/-inf (callers pre-filter NaN).
std::vector<float> sampleValues() {
  return {
      -std::numeric_limits<float>::infinity(),
      -1.0e30f,
      -123.456f,
      -1.0f,
      -1.0e-30f,
      -0.0f,
      0.0f,
      1.0e-30f,
      1.0f,
      123.456f,
      1.0e30f,
      std::numeric_limits<float>::infinity()};
}

TEST(AabbGpuKey, RoundTripsEveryValue) {
  for (const float v : sampleValues()) {
    EXPECT_EQ(orderedKeyToFloat(floatToOrderedKey(v)), v) << "round-trip failed for " << v;
  }
}

TEST(AabbGpuKey, KeysAreStrictlyIncreasingWithValue) {
  // sampleValues() is sorted ascending (ignoring the -0/+0 tie); the keys must be
  // strictly increasing so atomicMin/atomicMax pick the numerically smallest/largest.
  const std::vector<float> values = sampleValues();
  for (std::size_t i = 1; i < values.size(); ++i) {
    const uint32_t prev = floatToOrderedKey(values[i - 1]);
    const uint32_t curr = floatToOrderedKey(values[i]);
    EXPECT_LT(prev, curr) << "ordering broke between " << values[i - 1] << " and " << values[i];
  }
}

TEST(AabbGpuKey, AllNegativesSortBelowAllPositives) {
  const uint32_t neg = floatToOrderedKey(-1.0e-30f);  // smallest-magnitude negative
  const uint32_t pos = floatToOrderedKey(1.0e-30f);   // smallest-magnitude positive
  EXPECT_LT(neg, pos);
}

TEST(AabbGpuKey, InfinitiesBracketTheRange) {
  const uint32_t neg_inf = floatToOrderedKey(-std::numeric_limits<float>::infinity());
  const uint32_t pos_inf = floatToOrderedKey(std::numeric_limits<float>::infinity());
  for (const float v : {-1.0e30f, -1.0f, 0.0f, 1.0f, 1.0e30f}) {
    const uint32_t key = floatToOrderedKey(v);
    EXPECT_LT(neg_inf, key) << "-inf should be the global minimum key (" << v << ")";
    EXPECT_LT(key, pos_inf) << "+inf should be the global maximum key (" << v << ")";
  }
}

}  // namespace
