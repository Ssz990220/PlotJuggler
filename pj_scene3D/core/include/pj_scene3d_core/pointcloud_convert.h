// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/pointcloud.h"

namespace pj::scene3d {

// Result of convertCanonical(): the decoded render geometry plus the source-frame
// AABB of its finite points, accumulated in the same pass that fills `cloud`. The
// box is `valid == false` when no finite point was read (empty/all-NaN cloud), so a
// caller can use it directly as the layer's world bounds.
struct ConvertedPointCloud {
  DecodedPointCloud cloud;
  AABB bounds;
};

// Decode a canonical PointCloud into render-ready positions plus an optional
// colorize-by-field scalar, reading the raw little-endian point buffer per the
// cloud's own field layout. `scalar_field` selects the field to extract into
// `cloud.scalar` (empty -> no scalar). Positions/scalars are in the cloud's OWN
// source frame; the fixed-frame TF is applied later in the shader, never here.
//
// This is the renderer's last line of defense against a malformed/hostile cloud:
// the function validates the buffer size and every read field's offset against
// point_step BEFORE indexing, and returns an EMPTY result (empty positions, invalid
// bounds) on any rejection — big-endian, zero/oversized geometry, a too-small data
// buffer, a missing x/y/z field, or any field whose `offset + bytesPerElement*count`
// exceeds `point_step`. The layer logs a generic warning when positions come back
// empty, so no logging happens here (this unit is Qt-free core).
ConvertedPointCloud convertCanonical(const PJ::sdk::PointCloud& src, std::string_view scalar_field);

// --- Raw byte readers (exposed for direct unit testing) -------------------------
// All assemble little-endian; the cloud's `is_bigendian` is rejected upstream in
// convertCanonical, so these never see big-endian input. `data` must point at a
// span with at least the datatype's width of readable bytes (callers in
// convertCanonical guarantee this via the offset validation).

// IEEE-754 float32 at `data` (4 bytes consumed).
[[nodiscard]] float readFloat32At(const uint8_t* data);

// IEEE-754 float64 at `data` (8 bytes consumed).
[[nodiscard]] double readFloat64At(const uint8_t* data);

// One field element at `data` widened to float, sign-extending the signed integer
// datatypes (int8/int16/int32 decode negative values correctly). kFloat64 is
// truncated to float; kUnknown yields 0.
[[nodiscard]] float readScalarAt(const uint8_t* data, PJ::sdk::PointField::Datatype datatype);

// First field named `name`, or nullptr if absent.
[[nodiscard]] const PJ::sdk::PointField* findField(
    const std::vector<PJ::sdk::PointField>& fields, std::string_view name);

}  // namespace pj::scene3d
