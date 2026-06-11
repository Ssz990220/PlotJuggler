#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/expected.hpp"

// Decode adapters: turn a canonical CompressedPointCloud (Draco / Cloudini) into a
// canonical PointCloud the existing render path understands. SELF-DESCRIBING formats
// only — the point layout is recovered from the blob. CPU-heavy; widgets run it off
// the UI thread. See pj_scene3D/CLAUDE.md "Decoding boundary".
namespace pj::scene3d {

// Dispatch on `cloud.format` ("draco" | "cloudini", case-insensitive). Returns an
// error for an unsupported format, empty data, or a malformed/corrupt blob. Never
// throws. `frame_id` and `timestamp_ns` are copied through from the input.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeCompressedPointCloud(const PJ::sdk::CompressedPointCloud& cloud);

// Format-specific decoders. Exposed for focused testing; production code goes
// through decodeCompressedPointCloud().
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeCloudini(const PJ::sdk::CompressedPointCloud& cloud);
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeDraco(const PJ::sdk::CompressedPointCloud& cloud);

}  // namespace pj::scene3d
