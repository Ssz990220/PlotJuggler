#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>

namespace PJ {

/// Scan annex-B H.264 NAL units. Returns true if an IDR slice (NAL type 5) is found.
bool isH264Keyframe(const uint8_t* data, size_t size);

}  // namespace PJ
