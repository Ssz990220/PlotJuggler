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

ColorLayout detectColorLayout(const PointCloud& src) {
  ColorLayout layout;
  // 1. Canonical packed color: a single uint32 field named "rgba" (with alpha) or
  //    "rgb" (alpha forced opaque). Its 4 bytes are R,G,B,A in increasing address.
  for (const std::string_view name : {std::string_view("rgba"), std::string_view("rgb")}) {
    const PointField* packed = findField(src.fields, name);
    if (packed != nullptr && packed->datatype == PointField::Datatype::kUint32) {
      layout.valid = true;
      layout.r_offset = packed->offset;
      layout.g_offset = packed->offset + 1;
      layout.b_offset = packed->offset + 2;
      layout.has_alpha = (name == "rgba");
      layout.a_offset = packed->offset + 3;
      return layout;
    }
  }
  // 2. Separate uint8 channels (the raw, un-normalized foxglove layout).
  const PointField* red = findField(src.fields, "red");
  const PointField* green = findField(src.fields, "green");
  const PointField* blue = findField(src.fields, "blue");
  const PointField* alpha = findField(src.fields, "alpha");
  const auto is_u8 = [](const PointField* field) {
    return field != nullptr && field->datatype == PointField::Datatype::kUint8;
  };
  if (is_u8(red) && is_u8(green) && is_u8(blue)) {
    layout.valid = true;
    layout.r_offset = red->offset;
    layout.g_offset = green->offset;
    layout.b_offset = blue->offset;
    layout.has_alpha = is_u8(alpha);
    layout.a_offset = layout.has_alpha ? alpha->offset : 0;
    return layout;
  }
  return layout;  // no recognizable color field
}

ConvertedPointCloud convertCanonical(const PointCloud& src, std::string_view scalar_field, bool extract_rgba) {
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
  // RGB-direct mode (extract_rgba) takes precedence over scalar extraction: it reads
  // the cloud's color field instead of a colormap scalar. Validate every color byte we
  // will read against point_step, the same OOB guard applied to x/y/z above.
  ColorLayout color{};
  if (extract_rgba) {
    color = detectColorLayout(src);
    if (color.valid) {
      const auto byte_fits = [&](uint32_t offset) {
        return static_cast<uint64_t>(offset) + 1u <= static_cast<uint64_t>(src.point_step);
      };
      if (!byte_fits(color.r_offset) || !byte_fits(color.g_offset) || !byte_fits(color.b_offset) ||
          (color.has_alpha && !byte_fits(color.a_offset))) {
        return out;  // a malformed color offset is the same OOB hazard as x/y/z
      }
    }
  }

  const PointField* sf = nullptr;
  if (!extract_rgba && !scalar_field.empty()) {
    sf = findField(src.fields, scalar_field);
    if (sf != nullptr && !fieldFitsInPoint(*sf, src.point_step)) {
      return out;  // a malformed scalar offset is the same OOB hazard as x/y/z
    }
  }

  auto read_coord = [](const PointField* field, const uint8_t* base) -> float {
    if (field->datatype == PointField::Datatype::kFloat32) {
      return readFloat32At(base + field->offset);
    }
    if (field->datatype == PointField::Datatype::kFloat64) {
      return static_cast<float>(readFloat64At(base + field->offset));
    }
    return 0.0f;
  };

  const bool want_scalar = sf != nullptr;
  const bool want_rgba = extract_rgba && color.valid;
  out.cloud.positions.reserve(point_count);
  if (want_scalar) {
    out.cloud.scalar.reserve(point_count);
    out.cloud.scalar_field_name = std::string(scalar_field);
  }
  if (want_rgba) {
    out.cloud.rgba.reserve(point_count);
  }

  // Single pass over the point buffer: positions, optional scalar, and the finite-point
  // AABB all read from the same `base` so large clouds touch each point once (L.50).
  for (std::size_t point_index = 0; point_index < point_count; ++point_index) {
    const uint8_t* base = src.data.data() + point_index * step;
    const glm::vec3 position{read_coord(xf, base), read_coord(yf, base), read_coord(zf, base)};
    out.cloud.positions.push_back(position);
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
      expandAABB(out.bounds, position);
    }
    if (want_scalar) {
      out.cloud.scalar.push_back(readScalarAt(base + sf->offset, sf->datatype));
    }
    if (want_rgba) {
      const uint32_t r = base[color.r_offset];
      const uint32_t g = base[color.g_offset];
      const uint32_t b = base[color.b_offset];
      const uint32_t a = color.has_alpha ? base[color.a_offset] : 255u;
      out.cloud.rgba.push_back(r | (g << 8) | (b << 16) | (a << 24));
    }
  }
  return out;
}

}  // namespace pj::scene3d
