#pragma once

#include <memory>

#include "pj_scene2d_core/codec_pipeline.h"

namespace PJ {

/// JPEG → RGB888 via turbojpeg.
class JpegCodec : public CodecStage {
 public:
  JpegCodec();
  ~JpegCodec() override;
  JpegCodec(const JpegCodec&) = delete;
  JpegCodec& operator=(const JpegCodec&) = delete;
  JpegCodec(JpegCodec&&) = delete;
  JpegCodec& operator=(JpegCodec&&) = delete;

  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  void* tj_ = nullptr;
};

/// PNG → RGB888 (8-bit) or RGBA8888 (with alpha) or Mono16 (16-bit grayscale).
/// Preserves Mono16 for downstream stages (e.g. DepthToGrayscale).
class PngCodec : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

/// Mono16 depth → RGB888 grayscale. Normalizes to [min, max] range.
/// Input must be kMono16 with pixels->size() >= width*height*2.
class DepthToGrayscale : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

/// Pass-through for display-ready frames, Mono16 → RGB888 grayscale when needed.
class NormalizeMono16 : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  DepthToGrayscale mono16_to_grayscale_;
};

/// JPEG/PNG byte decoder cascade. The output may still be Mono16.
class ImageDecodeCascade : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  JpegCodec jpeg_;
  PngCodec png_;
};

/// JPEG/PNG auto-dispatch followed by Mono16 normalization when needed.
class AutoImageCodec : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  ImageDecodeCascade decode_;
  NormalizeMono16 normalize_;
};

/// Bayer color-filter-array pattern, named by the top-left 2x2 tile.
enum class BayerPattern : uint8_t {
  kRGGB,
  kGRBG,
  kGBRG,
  kBGGR,
};

/// Bayer mosaic → RGB888 demosaic. Input must be a kMono8 frame whose pixels
/// hold the raw CFA mosaic (one sample per pixel). The constructor selects the
/// CFA pattern. Missing channels are filled by averaging the same-channel
/// samples in the 3x3 neighbourhood (clamped at edges).
class BayerDecode : public CodecStage {
 public:
  explicit BayerDecode(BayerPattern pattern) : pattern_(pattern) {}

  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;

 private:
  BayerPattern pattern_;
};

/// Mono8 class IDs → RGB888 false-color. Each class ID (0-255) maps to
/// a distinct hue. Input must be kMono8 with pixels->size() >= width*height.
class SegmentationPalette : public CodecStage {
 public:
  Expected<DecodedFrame> decode(const DecodedFrame& input) const override;
};

// --- Pipeline builders ---

std::unique_ptr<CodecPipeline> makeJpegPipeline();  ///< JpegCodec only (raw JPEG input)

}  // namespace PJ
