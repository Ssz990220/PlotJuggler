// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "demo_cdr_codecs.h"

#include <cstring>

#include "pj_scene2d_core/codecs.h"

namespace PJ::demo {

Expected<DecodedFrame> CdrImageStripper::decode(const DecodedFrame& input) const {
  if (input.isNull() || input.pixels->size() < 16) {
    return unexpected("CDR data too small");
  }
  const auto* data = input.pixels->data();
  const auto size = input.pixels->size();

  for (size_t i = 0; i + 3 < size; ++i) {
    const bool is_jpeg = data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF;
    const bool is_png =
        i + 7 < size && data[i] == 0x89 && data[i + 1] == 0x50 && data[i + 2] == 0x4E && data[i + 3] == 0x47;

    if (is_jpeg || is_png) {
      size_t payload_size = size - i;
      if (i >= 4) {
        uint32_t len = 0;
        std::memcpy(&len, data + i - 4, 4);
        if (len > 0 && len <= size - i) {
          payload_size = len;
        }
      }
      DecodedFrame out;
      out.pixels = std::make_shared<std::vector<uint8_t>>(data + i, data + i + payload_size);
      return out;
    }
  }
  return unexpected("no JPEG/PNG marker found in CDR envelope");
}

std::unique_ptr<CodecPipeline> makeCdrJpegPipeline() {
  auto pipeline = std::make_unique<CodecPipeline>();
  pipeline->addStage(std::make_unique<CdrImageStripper>());
  pipeline->addStage(std::make_unique<JpegCodec>());
  return pipeline;
}

std::unique_ptr<CodecPipeline> makePngDepthPipeline() {
  auto pipeline = std::make_unique<CodecPipeline>();
  pipeline->addStage(std::make_unique<PngCodec>());
  pipeline->addStage(std::make_unique<DepthToGrayscale>());
  return pipeline;
}

}  // namespace PJ::demo
