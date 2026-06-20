#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstring>

namespace pj::scene3d {

// Order-preserving float <-> uint32 bijection for GPU atomic min/max.
//
// GLSL exposes only INTEGER atomics, so a GPU AABB reduction cannot atomicMin /
// atomicMax on floats directly. floatToOrderedKey() maps an IEEE-754 float to a
// uint32 whose UNSIGNED ordering matches the float's numeric ordering for every
// non-NaN value (including +/-0 and +/-inf), so atomicMin / atomicMax over the
// keys reduce the underlying floats correctly; orderedKeyToFloat() inverts the
// map on CPU readback.
//
// The GLSL reducer (pointcloud_aabb_reducer.cpp) mirrors floatToOrderedKey()
// EXACTLY (floatBitsToUint + the same mask) — the two MUST stay in lockstep or a
// GPU-reduced AABB silently diverges from the CPU scan. The shader needs only the
// forward map (it feeds the atomics); the inverse runs CPU-side after readback.
//
// Mapping: a positive float keeps its bit pattern but gets the sign bit SET (so it
// sorts above every negative); a negative float is fully inverted (so a larger
// magnitude sorts lower). NaN is intentionally unhandled — callers pre-filter
// non-finite points, matching the CPU scan's any_finite gate.
inline uint32_t floatToOrderedKey(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t mask = (bits & 0x80000000u) != 0u ? 0xFFFFFFFFu : 0x80000000u;
  return bits ^ mask;
}

inline float orderedKeyToFloat(uint32_t key) {
  const uint32_t mask = (key & 0x80000000u) != 0u ? 0x80000000u : 0xFFFFFFFFu;
  const uint32_t bits = key ^ mask;
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace pj::scene3d
