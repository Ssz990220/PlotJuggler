// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/borrowed_media_source.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace PJ {
namespace {

class RecordingSource final : public MediaSource {
 public:
  int set_calls = 0;
  int take_calls = 0;
  int64_t last_ts = 0;
  std::optional<MediaFrame> next_frame;

  void setTimestamp(int64_t ts_ns) override {
    ++set_calls;
    last_ts = ts_ns;
  }

  std::optional<MediaFrame> takeFrame() override {
    ++take_calls;
    auto out = std::move(next_frame);
    next_frame.reset();
    return out;
  }
};

TEST(BorrowedMediaSourceTest, NullSourceIsSafe) {
  BorrowedMediaSource borrowed;

  borrowed.setTimestamp(123);
  EXPECT_FALSE(borrowed.takeFrame().has_value());
  EXPECT_EQ(borrowed.source(), nullptr);
}

TEST(BorrowedMediaSourceTest, ForwardsCallsToBorrowedSource) {
  RecordingSource source;
  BorrowedMediaSource borrowed(&source);

  MediaFrame pending;
  DecodedFrame base;
  base.width = 640;
  pending.base = base;
  source.next_frame = pending;

  borrowed.setTimestamp(42);
  auto frame = borrowed.takeFrame();

  EXPECT_EQ(source.set_calls, 1);
  EXPECT_EQ(source.take_calls, 1);
  EXPECT_EQ(source.last_ts, 42);
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 640);
}

TEST(BorrowedMediaSourceTest, CanRetargetOrClearBorrowedSource) {
  RecordingSource first;
  RecordingSource second;
  BorrowedMediaSource borrowed(&first);

  borrowed.setTimestamp(10);
  borrowed.setSource(&second);
  borrowed.setTimestamp(20);
  borrowed.setSource(nullptr);
  borrowed.setTimestamp(30);

  EXPECT_EQ(first.set_calls, 1);
  EXPECT_EQ(first.last_ts, 10);
  EXPECT_EQ(second.set_calls, 1);
  EXPECT_EQ(second.last_ts, 20);
  EXPECT_FALSE(borrowed.takeFrame().has_value());
}

}  // namespace
}  // namespace PJ
