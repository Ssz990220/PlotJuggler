// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/depth_pipeline_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/depth_image_codec.hpp"
#include "pj_base/builtin/image_annotations.hpp"

namespace PJ {

namespace {

[[nodiscard]] uint8_t toByte(float value) noexcept {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

[[nodiscard]] sdk::ColorRGBA jet(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  const float r = std::clamp(1.5f - std::abs(4.0f * t - 3.0f), 0.0f, 1.0f);
  const float g = std::clamp(1.5f - std::abs(4.0f * t - 2.0f), 0.0f, 1.0f);
  const float b = std::clamp(1.5f - std::abs(4.0f * t - 1.0f), 0.0f, 1.0f);
  return {toByte(r), toByte(g), toByte(b), 255};
}

[[nodiscard]] sdk::ColorRGBA turbo(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  const float r = std::clamp(1.35f * t, 0.0f, 1.0f);
  const float g = std::clamp(1.0f - std::abs(2.0f * t - 1.0f), 0.0f, 1.0f);
  const float b = std::clamp(1.35f * (1.0f - t), 0.0f, 1.0f);
  return {toByte(r), toByte(g), toByte(b), 255};
}

[[nodiscard]] sdk::ColorRGBA colorFor(DepthColormap colormap, float t) noexcept {
  switch (colormap) {
    case DepthColormap::kJet:
      return jet(t);
    case DepthColormap::kTurbo:
      return turbo(t);
  }
  return turbo(t);
}

[[nodiscard]] bool isValidDepth(float value) noexcept {
  return std::isfinite(value) && value > 0.0f;
}

// Per-sample depth readers. Selected once per frame (see resolveDepthFormat),
// never per pixel.
[[nodiscard]] std::optional<float> read16UC1(const uint8_t* src) noexcept {
  const auto mm = static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
  if (mm == 0) {
    return std::nullopt;
  }
  return static_cast<float>(mm) * 0.001f;
}

[[nodiscard]] std::optional<float> read32FC1(const uint8_t* src) noexcept {
  float meters = 0.0f;
  std::memcpy(&meters, src, sizeof(float));
  if (!isValidDepth(meters)) {
    return std::nullopt;
  }
  return meters;
}

// Resolved depth encoding: stride and sample reader paired so they cannot drift
// apart. Resolved once from the encoding string before the decode loop.
struct DepthFormat {
  size_t bytes_per_pixel = 0;
  std::optional<float> (*read)(const uint8_t*) noexcept = nullptr;
};

[[nodiscard]] std::optional<DepthFormat> resolveDepthFormat(std::string_view encoding) noexcept {
  if (encoding == "16UC1") {
    return DepthFormat{2, &read16UC1};
  }
  if (encoding == "32FC1") {
    return DepthFormat{4, &read32FC1};
  }
  return std::nullopt;
}

}  // namespace

DepthPipelineSource::DepthPipelineSource(ObjectStore* store, ObjectTopicId topic) : store_(store), topic_(topic) {}

void DepthPipelineSource::setTimestamp(int64_t ts_ns) {
  if (store_ == nullptr) {
    pending_frame_.reset();
    return;
  }

  auto entry = store_->latestAt(topic_, ts_ns);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    pending_frame_.reset();
    return;
  }
  if (entry->timestamp == last_entry_ts_) {
    return;
  }

  auto depth = deserializeDepthImage(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!depth.has_value()) {
    fprintf(
        stderr, "[DepthPipelineSource] deserialize failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
        depth.error().c_str());
    last_entry_ts_ = entry->timestamp;
    pending_frame_.reset();
    return;
  }

  auto decoded = decodeDepthImage(*depth, entry->timestamp);
  last_entry_ts_ = entry->timestamp;
  if (!decoded.has_value() || decoded->isNull()) {
    pending_frame_.reset();
    return;
  }

  MediaFrame frame;
  frame.pixel_layers.push_back(PixelLayer{std::move(*decoded), opacity_});
  pending_frame_ = std::move(frame);
}

std::optional<MediaFrame> DepthPipelineSource::takeFrame() {
  // Hand off the pending frame and clear the mailbox in one step. Taking the
  // whole optional (rather than moving *pending_frame_ into a named MediaFrame)
  // also sidesteps a spurious GCC -O2 -Wmaybe-uninitialized on the moved payload.
  return std::exchange(pending_frame_, std::nullopt);
}

void DepthPipelineSource::setColormap(DepthColormap colormap) noexcept {
  colormap_ = colormap;
  invalidate();
}

void DepthPipelineSource::setRange(float near_m, float far_m) noexcept {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  near_m_ = near_m;
  far_m_ = far_m;
  auto_range_ = false;
  invalidate();
}

void DepthPipelineSource::setAutoRange(bool enabled) noexcept {
  auto_range_ = enabled;
  invalidate();
}

void DepthPipelineSource::setOpacity(float opacity) noexcept {
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  invalidate();
}

std::optional<DecodedFrame> DepthPipelineSource::decodeDepthImage(const sdk::DepthImage& depth, int64_t pts) const {
  // DepthImage float/colormap path; Image/PNG mono16 topics use Mono16ToGrayscale separately.
  const auto format = resolveDepthFormat(depth.encoding);
  if (!format.has_value() || depth.width == 0 || depth.height == 0 || depth.data.empty()) {
    return std::nullopt;
  }
  const size_t bytes_per_pixel = format->bytes_per_pixel;

  const size_t pixel_count = static_cast<size_t>(depth.width) * static_cast<size_t>(depth.height);
  if (depth.data.size() < pixel_count * bytes_per_pixel) {
    return std::nullopt;
  }

  std::vector<float> depths(pixel_count, std::numeric_limits<float>::quiet_NaN());
  float auto_min = std::numeric_limits<float>::max();
  float auto_max = std::numeric_limits<float>::lowest();
  size_t valid_count = 0;

  for (size_t i = 0; i < pixel_count; ++i) {
    const uint8_t* src = depth.data.data() + i * bytes_per_pixel;
    auto meters = format->read(src);
    if (!meters.has_value()) {
      continue;
    }
    depths[i] = *meters;
    auto_min = std::min(auto_min, *meters);
    auto_max = std::max(auto_max, *meters);
    ++valid_count;
  }

  float near_m = auto_range_ && valid_count > 0 ? auto_min : near_m_;
  float far_m = auto_range_ && valid_count > 0 ? auto_max : far_m_;
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  if (!(far_m > near_m)) {
    far_m = near_m + 1.0f;
  }

  auto pixels = std::make_shared<std::vector<uint8_t>>(pixel_count * 4, 0);
  const float inv_range = 1.0f / (far_m - near_m);
  for (size_t i = 0; i < pixel_count; ++i) {
    const float meters = depths[i];
    const size_t out = i * 4;
    if (!isValidDepth(meters)) {
      (*pixels)[out + 3] = 0;
      continue;
    }
    const float t = std::clamp((meters - near_m) * inv_range, 0.0f, 1.0f);
    const sdk::ColorRGBA color = colorFor(colormap_, t);
    (*pixels)[out + 0] = color.r;
    (*pixels)[out + 1] = color.g;
    (*pixels)[out + 2] = color.b;
    (*pixels)[out + 3] = color.a;
  }

  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = static_cast<int>(depth.width);
  frame.height = static_cast<int>(depth.height);
  frame.format = PixelFormat::kRGBA8888;
  frame.pts = depth.timestamp_ns != 0 ? depth.timestamp_ns : pts;
  return frame;
}

void DepthPipelineSource::invalidate() noexcept {
  last_entry_ts_ = INT64_MIN;
}

}  // namespace PJ
