#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_scene2d_core/codec_pipeline.h"

namespace PJ::demo {

class CdrImageStripper : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

std::unique_ptr<CodecPipeline> makeCdrJpegPipeline();
std::unique_ptr<CodecPipeline> makePngDepthPipeline();

}  // namespace PJ::demo
