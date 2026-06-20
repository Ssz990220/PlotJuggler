// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// How a VoxelGrid field is interpreted for display. The renderer uploads the
// selected field into a 3D texture as either a single float channel (scalar,
// then colormapped) or RGBA8 bytes (a direct colour channel).
enum class VoxelValueKind : uint8_t {
  kScalar,  ///< One numeric field → normalized to [0,1] and colormapped.
  kRgba,    ///< A 3/4-component colour field → drawn directly.
};

// Which field of a VoxelGrid drives the display, and how to read it. `index` is
// -1 when no field is usable (empty `fields`).
struct VoxelFieldSelection {
  int index = -1;
  VoxelValueKind kind = VoxelValueKind::kScalar;
};

// Total drawable voxel count (column * row * slice). 0 if any dimension is 0.
[[nodiscard]] uint64_t voxelCount(const PJ::sdk::VoxelGrid& grid);

// Byte offset of voxel (cx, ry, sz) within `grid.data`:
//   sz*slice_stride + ry*row_stride + cx*cell_stride
// (the SDK's documented dense Z-Y-X layout; x varies fastest). Does NOT add a
// field offset — callers add `field.offset` themselves.
[[nodiscard]] uint64_t voxelByteOffset(const PJ::sdk::VoxelGrid& grid, uint32_t cx, uint32_t ry, uint32_t sz);

// Source-frame axis-aligned bounds: the 8 corners of the local lattice box
// (extent = count*cell_size along each axis, lower corner at the grid origin)
// transformed by the origin Pose, then AABB'd. Invalid box for an empty or
// degenerate grid. Rotation IS honoured (unlike the planar OccupancyGrid bounds)
// because a voxel grid is volumetric and may be arbitrarily oriented.
[[nodiscard]] AABB voxelGridBounds(const PJ::sdk::VoxelGrid& grid);

// Densely pack `field`'s scalar value for every voxel into a float array of size
// voxelCount(), in the SAME Z-Y-X order the render pass uploads to a 3D texture
// (index = (sz*row_count + ry)*column_count + cx). A voxel whose bytes fall
// outside `data` packs as 0 (defensive against a short/garbage payload). Empty
// when the grid is degenerate or `field.datatype` has no size. `field.count > 1`
// reads the first element.
[[nodiscard]] std::vector<float> packScalarField(const PJ::sdk::VoxelGrid& grid, const PJ::sdk::PointField& field);

// Densely pack `field` as RGBA8 (size voxelCount()*4), same Z-Y-X order. Accepts
// a uint8 field with count 3 (rgb, alpha→255) or 4 (rgba), or a single uint32
// (its 4 bytes read in memory order as r,g,b,a). Out-of-bounds voxels pack as
// transparent black. Empty on a degenerate grid or an unsupported field layout.
[[nodiscard]] std::vector<uint8_t> packRgbaField(const PJ::sdk::VoxelGrid& grid, const PJ::sdk::PointField& field);

// The display kind for one field: kRgba only for a colour-named field with a
// uint8(count 3/4) or uint32(count 1) layout; otherwise kScalar. Used both to
// classify the auto-picked field and a field the user explicitly selects.
[[nodiscard]] VoxelValueKind voxelFieldKind(const PJ::sdk::PointField& field);

// Pick the default field + value kind: a field named rgb/rgba/color/colour with a
// uint8(count 3/4) or uint32(count 1) layout → kRgba; otherwise the first field →
// kScalar. index = -1 when `fields` is empty.
[[nodiscard]] VoxelFieldSelection chooseDefaultField(const std::vector<PJ::sdk::PointField>& fields);

}  // namespace pj::scene3d
