// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/h264_utils.h"

namespace PJ {

namespace {

// Find the next annex-B start code (3-byte 0x000001 or 4-byte 0x00000001).
// Returns the offset of the first byte of the start code, or size if not found.
size_t findStartCode(const uint8_t* data, size_t size, size_t offset) {
  while (offset + 2 < size) {
    if (data[offset] == 0x00 && data[offset + 1] == 0x00) {
      if (data[offset + 2] == 0x01) {
        return offset;
      }
      if (offset + 3 < size && data[offset + 2] == 0x00 && data[offset + 3] == 0x01) {
        return offset;
      }
    }
    ++offset;
  }
  return size;
}

// Given a start code at data[offset], return the offset of the NAL header byte
// (the byte after the start code).
size_t nalHeaderOffset(const uint8_t* data, size_t offset) {
  if (data[offset + 2] == 0x01) {
    return offset + 3;  // 3-byte start code
  }
  return offset + 4;  // 4-byte start code
}

}  // namespace

bool isH264Keyframe(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 4) {
    return false;
  }

  size_t pos = findStartCode(data, size, 0);
  while (pos < size) {
    size_t nal_start = nalHeaderOffset(data, pos);
    if (nal_start < size) {
      uint8_t nal_type = data[nal_start] & 0x1F;
      if (nal_type == 5) {  // IDR slice
        return true;
      }
    }
    pos = findStartCode(data, size, nal_start);
  }
  return false;
}

}  // namespace PJ
