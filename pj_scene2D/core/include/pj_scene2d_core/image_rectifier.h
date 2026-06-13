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

/// Rectify using a precomputed `UndistortMapFast` into a reused output frame —
/// the fast fallback path. `out`'s pixel buffer is resized as needed and fully
/// written (out-of-bounds pixels set to black), and its width/height/format/
/// frame_id/pts are filled from `src` and the table, so a caller can keep one
/// `DecodedFrame` across frames and avoid the per-frame allocation `rectifyFrame`
/// does. Output is bit-for-bit equivalent to `rectifyFrame`.
///
/// Returns false (leaving `out` unchanged) when the table is invalid, `src` is
/// invalid, the format is planar/16-bit (unsupported), or `src`'s size does not
/// match the table's `src_width`/`src_height` (the table would index wrong pixels).
[[nodiscard]] bool rectifyFrameFast(const DecodedFrame& src, const UndistortMapFast& fast, DecodedFrame& out);

}  // namespace PJ
