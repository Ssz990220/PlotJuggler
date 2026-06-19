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

// How to read a packed per-point RGBA color from a cloud's point buffer, when it
// carries color. `valid == false` means no recognizable color field is present.
// Offsets are byte offsets within one point; convertCanonical validates them
// against point_step before any read.
struct ColorLayout {
  bool valid = false;
  uint32_t r_offset = 0;
  uint32_t g_offset = 0;
  uint32_t b_offset = 0;
  bool has_alpha = false;  // false -> alpha is forced to 255 (opaque)
  uint32_t a_offset = 0;   // meaningful only when has_alpha
};

// Recognize a cloud's per-point color by the canonical convention, in priority order:
//   1. a single packed field named "rgba" or "rgb" of datatype kUint32 — the 4 bytes at
//      its offset are R,G,B,A in increasing address (alpha present iff the name is "rgba");
//   2. separate uint8 channels "red"/"green"/"blue" (+ optional "alpha") — the raw,
//      un-normalized foxglove layout, recognized defensively so the host shows true color
//      even for sources the parser has not collapsed to a packed field yet.
// Returns {valid=false} when neither is present. Offsets are NOT range-checked here;
// convertCanonical validates them against point_step before reading.
[[nodiscard]] ColorLayout detectColorLayout(const PJ::sdk::PointCloud& src);

// Decode a canonical PointCloud into render-ready positions plus an optional
// colorize-by-field scalar, reading the raw little-endian point buffer per the
// cloud's own field layout. `scalar_field` selects the field to extract into
// `cloud.scalar` (empty -> no scalar). Positions/scalars are in the cloud's OWN
// source frame; the fixed-frame TF is applied later in the shader, never here.
//
// When `extract_rgba` is true, the cloud's color field (see detectColorLayout) is
// decoded into `cloud.rgba` (packed R,G,B,A; alpha 255 when the source has none) and
// the scalar is NOT extracted — RGB-direct mode bypasses the colormap. With no color
// field present, `cloud.rgba` stays empty.
//
// This is the renderer's last line of defense against a malformed/hostile cloud:
// the function validates the buffer size and every read field's offset against
// point_step BEFORE indexing, and returns an EMPTY result (empty positions, invalid
// bounds) on any rejection — big-endian, zero/oversized geometry, a too-small data
// buffer, a missing x/y/z field, or any field (including a color channel) whose
// `offset + bytesPerElement*count` exceeds `point_step`. The layer logs a generic
// warning when positions come back empty, so no logging happens here (Qt-free core).
ConvertedPointCloud convertCanonical(
    const PJ::sdk::PointCloud& src, std::string_view scalar_field, bool extract_rgba = false);

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
