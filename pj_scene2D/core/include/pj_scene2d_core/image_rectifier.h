#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <optional>

#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/undistort_remap.h"

namespace PJ {

/// Rectify (lens-undistort) a decoded frame by bilinearly resampling it through a
/// precomputed reverse map. The output frame is `map.out_width x map.out_height`
/// with the same PixelFormat as the input; out-of-bounds source samples render
/// black.
///
/// Supports the interleaved 8-bit formats our image codecs emit (RGB888 / RGBA8888
/// / BGR888 / BGRA8888 / Mono8 — JPEG decodes to RGB888). For planar or multi-byte
/// formats (YUV420P / NV12 / Mono16) it returns `std::nullopt`, signalling the
/// caller to keep the original (unrectified) frame.
[[nodiscard]] std::optional<DecodedFrame> rectifyFrame(const DecodedFrame& src, const UndistortMap& map);

}  // namespace PJ
