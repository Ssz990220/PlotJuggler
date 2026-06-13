// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/pointcloud_convert.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>

namespace pj::scene3d {

using PJ::sdk::bytesPerElement;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;

float readFloat32At(const uint8_t* data) {
  const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return std::bit_cast<float>(bits);
}

double readFloat64At(const uint8_t* data) {
  uint64_t bits = 0;
  for (int byte_index = 0; byte_index < 8; ++byte_index) {
    bits |= static_cast<uint64_t>(data[byte_index]) << (8 * byte_index);
  }
  return std::bit_cast<double>(bits);
}

float readScalarAt(const uint8_t* data, PointField::Datatype datatype) {
  using DT = PointField::Datatype;
  switch (datatype) {
    case DT::kInt8:
      return static_cast<float>(std::bit_cast<int8_t>(*data));
    case DT::kUint8:
      return static_cast<float>(*data);
    case DT::kInt16: {
      const uint16_t bits = static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
      return static_cast<float>(std::bit_cast<int16_t>(bits));
    }
    case DT::kUint16:
      return static_cast<float>(
          static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8));
    case DT::kInt32: {
      const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      return static_cast<float>(std::bit_cast<int32_t>(bits));
    }
    case DT::kUint32: {
      const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      return static_cast<float>(bits);
    }
    case DT::kFloat32:
      return readFloat32At(data);
    case DT::kFloat64:
      return static_cast<float>(readFloat64At(data));
    case DT::kUnknown:
    default:
      return 0.0f;
  }
}

const PointField* findField(const std::vector<PointField>& fields, std::string_view name) {
  const auto it = std::find_if(
      fields.begin(), fields.end(), [name](const PointField& field) { return std::string_view(field.name) == name; });
  return it == fields.end() ? nullptr : &*it;
}

namespace {

// True when reading `field` stays inside one point: offset + width*count <= point_step.
// Computed in uint64 so the offset+size sum cannot overflow uint32 for an adversarial
// header. A zero-width datatype (kUnknown) fails this (0-byte element is never readable).
bool fieldFitsInPoint(const PointField& field, uint32_t point_step) {
  const uint64_t width = static_cast<uint64_t>(bytesPerElement(field.datatype));
  if (width == 0) {
    return false;
  }
  const uint64_t field_end =
      static_cast<uint64_t>(field.offset) + width * static_cast<uint64_t>(std::max<uint32_t>(field.count, 1u));
  return field_end <= static_cast<uint64_t>(point_step);
}

}  // namespace

ConvertedPointCloud convertCanonical(const PointCloud& src, std::string_view scalar_field) {
  ConvertedPointCloud out;
  out.cloud.frame_id = src.frame_id;

  if (src.is_bigendian) {
    return out;  // big-endian assembly unsupported; drop
  }
  const std::size_t point_count = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height);
  if (point_count == 0 || src.point_step == 0 || src.data.empty()) {
    return out;
  }
  const std::size_t step = src.point_step;
  if (src.data.size() < point_count * step) {
    return out;  // declared geometry exceeds the backing buffer
  }
  const PointField* xf = findField(src.fields, "x");
  const PointField* yf = findField(src.fields, "y");
  const PointField* zf = findField(src.fields, "z");
  if (xf == nullptr || yf == nullptr || zf == nullptr) {
    return out;  // no spatial fields
  }

  // M.23/M.24: validate field offsets against point_step BEFORE indexing the buffer.
  // Field offsets come from the wire/parser verbatim; an offset whose read crosses
  // point_step is an out-of-bounds heap read on the tail point (and far worse for an
  // adversarial uint32 offset). Reject every field we actually read here.
  if (!fieldFitsInPoint(*xf, src.point_step) || !fieldFitsInPoint(*yf, src.point_step) ||
      !fieldFitsInPoint(*zf, src.point_step)) {
    return out;
  }
  const PointField* sf = nullptr;
  if (!scalar_field.empty()) {
    sf = findField(src.fields, scalar_field);
    if (sf != nullptr && !fieldFitsInPoint(*sf, src.point_step)) {
      return out;  // a malformed scalar offset is the same OOB hazard as x/y/z
    }
  }

  auto readCoord = [](const PointField* field, const uint8_t* base) -> float {
    if (field->datatype == PointField::Datatype::kFloat32) {
      return readFloat32At(base + field->offset);
    }
    if (field->datatype == PointField::Datatype::kFloat64) {
      return static_cast<float>(readFloat64At(base + field->offset));
    }
    return 0.0f;
  };

  const bool want_scalar = sf != nullptr;
  out.cloud.positions.reserve(point_count);
  if (want_scalar) {
    out.cloud.scalar.reserve(point_count);
    out.cloud.scalar_field_name = std::string(scalar_field);
  }

  // Single pass over the point buffer: positions, optional scalar, and the finite-point
  // AABB all read from the same `base` so large clouds touch each point once (L.50).
  for (std::size_t point_index = 0; point_index < point_count; ++point_index) {
    const uint8_t* base = src.data.data() + point_index * step;
    const glm::vec3 position{readCoord(xf, base), readCoord(yf, base), readCoord(zf, base)};
    out.cloud.positions.push_back(position);
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
      expandAABB(out.bounds, position);
    }
    if (want_scalar) {
      out.cloud.scalar.push_back(readScalarAt(base + sf->offset, sf->datatype));
    }
  }
  return out;
}

}  // namespace pj::scene3d
