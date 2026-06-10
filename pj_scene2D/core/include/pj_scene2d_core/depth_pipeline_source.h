#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <cstdint>
#include <optional>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

namespace sdk {
struct DepthImage;
}  // namespace sdk

enum class DepthColormap : uint8_t {
  kJet,
  kTurbo,
};

/// MediaSource for canonical sdk::DepthImage topics.
///
/// On setTimestamp(), queries ObjectStore::latestAt(), deserializes the
/// PJ.DepthImage payload, and maps 16UC1 millimeter or 32FC1 meter depth samples
/// to an RGBA colormapped pixel frame.
class DepthPipelineSource : public MediaSource {
 public:
  /// @param store  ObjectStore to query (not owned)
  /// @param topic  Topic ID with serialized sdk::DepthImage payloads
  DepthPipelineSource(ObjectStore* store, ObjectTopicId topic);

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

  void setColormap(DepthColormap colormap) noexcept;
  void setRange(float near_m, float far_m) noexcept;
  void setAutoRange(bool enabled) noexcept;
  void setOpacity(float opacity) noexcept;

 private:
  [[nodiscard]] std::optional<DecodedFrame> decodeDepthImage(const sdk::DepthImage& depth, int64_t pts) const;
  void invalidate() noexcept;

  ObjectStore* store_;
  ObjectTopicId topic_;
  DepthColormap colormap_ = DepthColormap::kTurbo;
  float near_m_ = 0.0f;
  float far_m_ = 10.0f;
  bool auto_range_ = true;
  float opacity_ = 1.0f;
  int64_t last_entry_ts_ = INT64_MIN;
  std::optional<MediaFrame> pending_frame_;
};

}  // namespace PJ
