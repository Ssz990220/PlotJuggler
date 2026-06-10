// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/h264_utils.h"

#include "video_codec_utils_internal.h"

namespace PJ {

bool isH264Keyframe(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 4) {
    return false;
  }

  for (size_t hdr = nextNalHeader(data, size, 0); hdr < size; hdr = nextNalHeader(data, size, hdr)) {
    uint8_t nal_type = data[hdr] & 0x1F;
    if (nal_type == 5) {  // IDR slice
      return true;
    }
  }
  return false;
}

}  // namespace PJ
