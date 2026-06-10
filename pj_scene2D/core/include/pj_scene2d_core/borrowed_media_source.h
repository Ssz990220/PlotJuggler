#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>

#include "pj_scene2d_core/media_source.h"

namespace PJ {

/// Non-owning MediaSource adapter. It forwards MediaSource calls to a borrowed
/// source pointer and returns no data when no source is currently attached.
class BorrowedMediaSource final : public MediaSource {
 public:
  explicit BorrowedMediaSource(MediaSource* source = nullptr) noexcept : source_(source) {}

  void setSource(MediaSource* source) noexcept {
    source_ = source;
  }

  [[nodiscard]] MediaSource* source() const noexcept {
    return source_;
  }

  void setTimestamp(int64_t ts_ns) override {
    if (source_ != nullptr) {
      source_->setTimestamp(ts_ns);
    }
  }

  std::optional<MediaFrame> takeFrame() override {
    if (source_ == nullptr) {
      return std::nullopt;
    }
    return source_->takeFrame();
  }

  void invalidate() override {
    if (source_ != nullptr) {
      source_->invalidate();
    }
  }

 private:
  MediaSource* source_ = nullptr;
};

}  // namespace PJ
