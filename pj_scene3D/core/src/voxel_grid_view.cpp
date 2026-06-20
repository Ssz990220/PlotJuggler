// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/voxel_grid_view.h"

#include <glm/glm.hpp>

#include "pj_scene3d_core/pointcloud_convert.h"     // findField, readScalarAt (shared field helpers)
#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4 (shared Pose->mat4 convention)

namespace pj::scene3d {

namespace {

// Lower-cased copy for the case-insensitive default-field name match.
std::string toLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

// True if `field` is a usable RGB(A) colour channel for the kRgba display path.
bool isColorField(const PJ::sdk::PointField& field) {
  const bool uint8_color =
      field.datatype == PJ::sdk::PointField::Datatype::kUint8 && (field.count == 3 || field.count == 4);
  const bool packed_u32 = field.datatype == PJ::sdk::PointField::Datatype::kUint32 && field.count == 1;
  return uint8_color || packed_u32;
}

}  // namespace

uint64_t voxelCount(const PJ::sdk::VoxelGrid& grid) {
  return static_cast<uint64_t>(grid.column_count) * grid.row_count * grid.slice_count;
}

uint64_t voxelByteOffset(const PJ::sdk::VoxelGrid& grid, uint32_t cx, uint32_t ry, uint32_t sz) {
  return static_cast<uint64_t>(sz) * grid.slice_stride + static_cast<uint64_t>(ry) * grid.row_stride +
         static_cast<uint64_t>(cx) * grid.cell_stride;
}

AABB voxelGridBounds(const PJ::sdk::VoxelGrid& grid) {
  if (voxelCount(grid) == 0) {
    return {};
  }
  const glm::vec3 extent{
      static_cast<float>(grid.column_count) * static_cast<float>(grid.cell_size.x),
      static_cast<float>(grid.row_count) * static_cast<float>(grid.cell_size.y),
      static_cast<float>(grid.slice_count) * static_cast<float>(grid.cell_size.z)};
  const glm::mat4 model = poseToMat4(grid.origin);
  AABB box;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 local{
        (corner & 1) != 0 ? extent.x : 0.0f, (corner & 2) != 0 ? extent.y : 0.0f, (corner & 4) != 0 ? extent.z : 0.0f};
    const glm::vec3 world = glm::vec3(model * glm::vec4(local, 1.0f));
    expandAABB(box, world);
  }
  return box;
}

std::vector<float> packScalarField(const PJ::sdk::VoxelGrid& grid, const PJ::sdk::PointField& field) {
  const uint64_t count = voxelCount(grid);
  const uint32_t elem = PJ::sdk::bytesPerElement(field.datatype);
  if (count == 0 || elem == 0) {
    return {};
  }
  std::vector<float> out;
  out.reserve(count);
  const uint8_t* base = grid.data.data();
  const size_t size = grid.data.size();
  for (uint32_t sz = 0; sz < grid.slice_count; ++sz) {
    for (uint32_t ry = 0; ry < grid.row_count; ++ry) {
      for (uint32_t cx = 0; cx < grid.column_count; ++cx) {
        const uint64_t off = voxelByteOffset(grid, cx, ry, sz) + field.offset;
        if (base == nullptr || off + elem > size) {
          out.push_back(0.0f);
          continue;
        }
        out.push_back(readScalarAt(base + off, field.datatype));
      }
    }
  }
  return out;
}

std::vector<uint8_t> packRgbaField(const PJ::sdk::VoxelGrid& grid, const PJ::sdk::PointField& field) {
  const uint64_t count = voxelCount(grid);
  if (count == 0 || !isColorField(field)) {
    return {};
  }
  // Bytes to read per voxel from the source field: uint8xN reads N consecutive
  // bytes; a packed uint32 reads its 4 bytes. Either way the first 3 become RGB
  // and the 4th (present for count==4 / uint32) becomes A, else opaque.
  const bool has_alpha = (field.datatype == PJ::sdk::PointField::Datatype::kUint8 && field.count == 4) ||
                         field.datatype == PJ::sdk::PointField::Datatype::kUint32;
  const uint32_t src_bytes = has_alpha ? 4U : 3U;

  std::vector<uint8_t> out(static_cast<size_t>(count) * 4U, 0U);
  const uint8_t* base = grid.data.data();
  const size_t size = grid.data.size();
  size_t dst = 0;
  for (uint32_t sz = 0; sz < grid.slice_count; ++sz) {
    for (uint32_t ry = 0; ry < grid.row_count; ++ry) {
      for (uint32_t cx = 0; cx < grid.column_count; ++cx, dst += 4U) {
        const uint64_t off = voxelByteOffset(grid, cx, ry, sz) + field.offset;
        if (base == nullptr || off + src_bytes > size) {
          continue;  // leave transparent black
        }
        out[dst + 0] = base[off + 0];
        out[dst + 1] = base[off + 1];
        out[dst + 2] = base[off + 2];
        out[dst + 3] = has_alpha ? base[off + 3] : 255U;
      }
    }
  }
  return out;
}

VoxelValueKind voxelFieldKind(const PJ::sdk::PointField& field) {
  const std::string lname = toLowerAscii(field.name);
  const bool color_name = lname == "rgb" || lname == "rgba" || lname == "color" || lname == "colour" ||
                          lname == "colors" || lname == "colours";
  return (color_name && isColorField(field)) ? VoxelValueKind::kRgba : VoxelValueKind::kScalar;
}

VoxelFieldSelection chooseDefaultField(const std::vector<PJ::sdk::PointField>& fields) {
  if (fields.empty()) {
    return {};
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    if (voxelFieldKind(fields[i]) == VoxelValueKind::kRgba) {
      return {static_cast<int>(i), VoxelValueKind::kRgba};
    }
  }
  return {0, VoxelValueKind::kScalar};
}

}  // namespace pj::scene3d
