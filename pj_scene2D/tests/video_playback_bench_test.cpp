// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/// Playback-fluidity benchmark for the streaming video path. Env-gated: set
/// PJ_BENCH_MP4=<file.mp4> to run, otherwise the suite skips (CI-safe).
///
/// Three measurements against the real file:
///  A. Raw sequential decode throughput (StreamingVideoDecoder::decodeAt at
///     frame-step targets, no cancellation) — can decode+convert sustain the
///     video's native rate at all?
///  B. Playback simulation through StreamingVideoSource at 60 Hz ticks —
///     the production scheduling (latest-wins + cancel-on-new-request).
///  C. Same simulation at 30 Hz — if delivery recovers here while B collapses,
///     the per-tick latency sits past the 60 Hz budget and every in-flight
///     decode is being cancelled (the re-seek spiral), rather than the decoder
///     being fundamentally too slow.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/streaming_video_source.h"
#include "test_mp4_demux.h"

namespace PJ {
namespace {

constexpr int64_t kNsPerSec = 1'000'000'000;

struct LoadedVideo {
  ObjectStore store;
  ObjectTopicId topic;
  size_t packet_count = 0;
  size_t keyframe_count = 0;
  Timestamp t_min = 0;
  Timestamp t_max = 0;
  std::string codec;
};

LoadedVideo* loadVideo(const std::string& path) {
  auto video = new LoadedVideo();
  auto topic_or = video->store.registerTopic({.dataset_id = 1, .topic_name = "video", .metadata_json = "{}"});
  if (!topic_or.has_value()) {
    return nullptr;
  }
  video->topic = *topic_or;
  const auto packets = test::extractAnnexBPackets(path);
  for (const auto& packet : packets) {
    video->store.pushOwned(video->topic, packet.dts, packet.data);
    if (packet.keyframe) {
      ++video->keyframe_count;
    }
  }
  video->packet_count = packets.size();
  video->codec = test::mp4VideoFormat(path);
  auto [t_min, t_max] = video->store.timeRange(video->topic);
  video->t_min = t_min;
  video->t_max = t_max;
  return video;
}

struct LatencyStats {
  double avg_ms = 0;
  double p50_ms = 0;
  double p95_ms = 0;
  double max_ms = 0;
};

LatencyStats summarize(std::vector<double>& samples_ms) {
  LatencyStats stats;
  if (samples_ms.empty()) {
    return stats;
  }
  std::sort(samples_ms.begin(), samples_ms.end());
  double sum = 0;
  for (double sample : samples_ms) {
    sum += sample;
  }
  stats.avg_ms = sum / static_cast<double>(samples_ms.size());
  stats.p50_ms = samples_ms[samples_ms.size() / 2];
  stats.p95_ms = samples_ms[(samples_ms.size() * 95) / 100];
  stats.max_ms = samples_ms.back();
  return stats;
}

class VideoPlaybackBench : public ::testing::Test {
 protected:
  static LoadedVideo* video_;

  static void SetUpTestSuite() {
    const auto path = sdk::getEnv("PJ_BENCH_MP4");
    if (!path.has_value()) {
      return;
    }
    video_ = loadVideo(*path);
    if (video_ != nullptr && video_->packet_count > 0) {
      const double duration_s = static_cast<double>(video_->t_max - video_->t_min) / kNsPerSec;
      fprintf(
          stderr, "[bench] %s: codec=%s packets=%zu keyframes=%zu (GOP ~%zu) duration=%.1fs\n", path->c_str(),
          video_->codec.c_str(), video_->packet_count, video_->keyframe_count,
          video_->packet_count / std::max<size_t>(video_->keyframe_count, 1), duration_s);
    }
  }

  static void TearDownTestSuite() {
    delete video_;
    video_ = nullptr;
  }

  void SetUp() override {
    if (video_ == nullptr || video_->packet_count == 0) {
      GTEST_SKIP() << "set PJ_BENCH_MP4=<file.mp4> to run the playback benchmark";
    }
  }

  /// Wall-clock-paced playback through StreamingVideoSource at `tick_hz`,
  /// covering `media_seconds` of media. Returns delivered-frame stats.
  void runPlaybackSim(double tick_hz, double media_seconds) {
    StreamingVideoSource source(&video_->store, video_->topic);

    std::atomic<int> deposits{0};
    source.setFrameReadyCallback([&deposits] { deposits.fetch_add(1, std::memory_order_relaxed); });

    const int64_t tick_ns = static_cast<int64_t>(static_cast<double>(kNsPerSec) / tick_hz);
    const int ticks = static_cast<int>(media_seconds * tick_hz);

    int taken = 0;
    std::set<int64_t> distinct_pts;
    auto next_tick = std::chrono::steady_clock::now();
    for (int i = 0; i < ticks; ++i) {
      source.setTimestamp(video_->t_min + static_cast<int64_t>(i) * tick_ns);
      if (auto frame = source.takeFrame(); frame.has_value() && frame->base.has_value() && !frame->base->isNull()) {
        ++taken;
        distinct_pts.insert(frame->base->pts);
      }
      next_tick += std::chrono::nanoseconds(tick_ns);
      std::this_thread::sleep_until(next_tick);
    }
    // Grace drain: collect what the worker finishes right after the last tick.
    for (int i = 0; i < 20; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (auto frame = source.takeFrame(); frame.has_value() && frame->base.has_value() && !frame->base->isNull()) {
        ++taken;
        distinct_pts.insert(frame->base->pts);
      }
    }

    fprintf(
        stderr,
        "[bench] playback @%.0fHz over %.1fs media: ticks=%d produced=%d taken=%d distinct=%zu -> %.1f fps "
        "displayed\n",
        tick_hz, media_seconds, ticks, deposits.load(), taken, distinct_pts.size(),
        static_cast<double>(distinct_pts.size()) / media_seconds);
  }
};

LoadedVideo* VideoPlaybackBench::video_ = nullptr;

TEST_F(VideoPlaybackBench, A_SequentialDecodeThroughput) {
  StreamingVideoDecoder decoder;
  decoder.attach(&video_->store, video_->topic);

  const int64_t frame_step = (video_->t_max - video_->t_min) / static_cast<int64_t>(video_->packet_count - 1);
  const int frames = std::min<int>(240, static_cast<int>(video_->packet_count) - 1);

  std::vector<double> latencies_ms;
  latencies_ms.reserve(static_cast<size_t>(frames));
  int decoded = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    const Timestamp target = video_->t_min + static_cast<int64_t>(i) * frame_step;
    const auto call_start = std::chrono::steady_clock::now();
    auto result = decoder.decodeAt(target, nullptr);
    const auto call_end = std::chrono::steady_clock::now();
    latencies_ms.push_back(std::chrono::duration<double, std::milli>(call_end - call_start).count());
    if (result.has_value() && !result->isNull()) {
      ++decoded;
    }
  }
  const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const LatencyStats stats = summarize(latencies_ms);
  fprintf(
      stderr,
      "[bench] sequential decodeAt: %d/%d frames in %.2fs (%.1f fps) | per-call ms avg=%.2f p50=%.2f p95=%.2f "
      "max=%.2f\n",
      decoded, frames, total_s, static_cast<double>(decoded) / total_s, stats.avg_ms, stats.p50_ms, stats.p95_ms,
      stats.max_ms);
  EXPECT_GT(decoded, 0);
}

TEST_F(VideoPlaybackBench, B_PlaybackSim60Hz) {
  runPlaybackSim(60.0, 4.0);
}

TEST_F(VideoPlaybackBench, C_PlaybackSim30Hz) {
  runPlaybackSim(30.0, 4.0);
}

}  // namespace
}  // namespace PJ
