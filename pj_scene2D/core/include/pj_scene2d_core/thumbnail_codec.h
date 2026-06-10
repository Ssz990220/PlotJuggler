#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// Stateless HD-capped JPEG thumbnail codec shared by entry-sourced thumbnail
/// builders. Downscale-to-JPEG and JPEG-to-frame both work on YUV420P (the
/// format every FfmpegDecoder output uses), so no colour conversion is needed —
/// the GPU shader does YUV→RGB. Contexts are created per call: cheap relative to
/// a decode, and these run on a background build thread / on scrub, not per
/// playback frame.

/// Maximum thumbnail width. Anything larger is downscaled (aspect-preserving)
/// so a 4K source yields HD (1280x720-class) thumbnails, never 4K ones.
inline constexpr int kThumbnailMaxWidth = 1280;
inline constexpr int kThumbnailJpegQuality = 80;

struct JpegThumbnail {
  std::vector<uint8_t> jpeg;  ///< Compressed JPEG bytes (empty on failure).
  int width = 0;              ///< Decoded thumbnail width (<= kThumbnailMaxWidth).
  int height = 0;             ///< Decoded thumbnail height.
};

/// Downscale a YUV420P frame to <= `max_width` (even dimensions) and JPEG-encode
/// it. Returns an empty `.jpeg` if `src` is not a valid YUV420P frame or on
/// codec failure.
[[nodiscard]] JpegThumbnail encodeThumbnailJpeg(const DecodedFrame& src, int max_width, int quality);

/// Decompress a JPEG thumbnail (encoded by encodeThumbnailJpeg) back to a
/// YUV420P DecodedFrame of the given dimensions. Returns nullopt on failure.
[[nodiscard]] std::optional<DecodedFrame> decodeThumbnailJpeg(
    const uint8_t* jpeg, size_t jpeg_size, int width, int height);

}  // namespace PJ
