#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// Build the 4x4 YUV→RGB color-conversion matrix for a given color space + range.
///
/// The returned matrix is **column-major** (the std140 `mat4` layout the media
/// shader expects) and is applied as `rgb = (M * vec4(y, u - 0.5, v - 0.5, 1)).rgb`
/// — i.e. the shader pre-centres chroma at 0.5 and the constant 4th column carries
/// every range offset/correction. This keeps the shader unchanged across all
/// (space, range) combinations: only the matrix differs.
///
/// `kBt709` + `kFull` reproduces the historical hardcoded BT.709 full-range matrix
/// exactly (4th column all zero), so existing full-range content is unaffected.
/// Limited range bakes in the 255/219 luma and 255/224 chroma scales plus the
/// 16/255 luma and 128/255 chroma offsets, so limited-range video no longer renders
/// with washed-out blacks/whites.
[[nodiscard]] std::array<float, 16> buildYuvMatrix(YuvColorSpace space, YuvColorRange range) noexcept;

}  // namespace PJ
