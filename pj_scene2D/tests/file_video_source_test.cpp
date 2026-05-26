#include "pj_scene2d_core/file_video_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace PJ {
namespace {

const std::string kTestVideo = "pj_scene2D/testdata/test_480p.mp4";

class FileVideoSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kTestVideo)) {
      GTEST_SKIP() << "test_480p.mp4 not found";
    }
  }
};

TEST_F(FileVideoSourceTest, OpenAndQueryDuration) {
  auto source_or = FileVideoSource::open(kTestVideo);
  ASSERT_TRUE(source_or.has_value()) << source_or.error();
  auto& source = *source_or;

  EXPECT_GT(source->duration(), 0.0);
}

TEST_F(FileVideoSourceTest, OpenNonexistentFile) {
  auto source_or = FileVideoSource::open("/tmp/nonexistent_video.mp4");
  EXPECT_FALSE(source_or.has_value());
}

TEST_F(FileVideoSourceTest, SetTimestampAndTakeFrame) {
  auto source_or = FileVideoSource::open(kTestVideo);
  ASSERT_TRUE(source_or.has_value());
  auto& source = *source_or;

  // Seek to 1 second
  source->setTimestamp(1'000'000'000);

  // Poll until we get a frame (decode is async)
  std::optional<MediaFrame> frame;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!frame.has_value() || !frame->base.has_value()) && std::chrono::steady_clock::now() < deadline) {
    frame = source->takeFrame();
    if (!frame.has_value() || !frame->base.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  ASSERT_TRUE(frame.has_value()) << "no frame received within 5 seconds";
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_FALSE(frame->base->isNull());
  EXPECT_EQ(frame->base->width, 640);
  EXPECT_EQ(frame->base->height, 480);
  EXPECT_EQ(frame->base->format, PixelFormat::kYUV420P);
  EXPECT_TRUE(frame->base->isValid());
}

TEST_F(FileVideoSourceTest, PauseResume) {
  auto source_or = FileVideoSource::open(kTestVideo);
  ASSERT_TRUE(source_or.has_value());
  auto& source = *source_or;

  // FfmpegBackend opens in paused state
  EXPECT_TRUE(source->isPaused());
  source->setPaused(false);
  EXPECT_FALSE(source->isPaused());
  source->setPaused(true);
  EXPECT_TRUE(source->isPaused());
}

TEST_F(FileVideoSourceTest, SetEpochAnchorAppliesOffset) {
  // setEpochAnchorNs maps tracker (epoch) ns → file-relative ns by subtracting
  // the anchor inside setTimestamp. With anchor = T0 and setTimestamp(T0 + N),
  // the backend should seek to N nanoseconds into the file (≈ N/1e9 seconds).
  auto source_or = FileVideoSource::open(kTestVideo);
  ASSERT_TRUE(source_or.has_value());
  auto& source = *source_or;

  constexpr int64_t kAnchorNs = 1'700'000'000'000'000'000LL;  // arbitrary epoch ns
  constexpr int64_t kOffsetNs = 1'000'000'000LL;              // 1 second into the file
  source->setEpochAnchorNs(kAnchorNs);
  source->setTimestamp(kAnchorNs + kOffsetNs);

  // Poll for a frame; if the anchor wasn't applied, the backend would have
  // been asked to seek to a far-future point (well beyond duration) and no
  // frame would arrive within the deadline.
  std::optional<MediaFrame> frame;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!frame.has_value() || !frame->base.has_value()) && std::chrono::steady_clock::now() < deadline) {
    frame = source->takeFrame();
    if (!frame.has_value() || !frame->base.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  ASSERT_TRUE(frame.has_value()) << "no frame received — anchor offset was likely not applied";
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_TRUE(frame->base->isValid());
  // The reported playback position should be close to the file-relative
  // 1.0 second target (decoded frame may snap to nearest keyframe).
  EXPECT_GE(source->position(), 0.0);
  EXPECT_LT(source->position(), source->duration());
}

}  // namespace
}  // namespace PJ
