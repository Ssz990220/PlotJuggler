#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/video_backend.h"

namespace PJ {

class FfmpegBackend;

/// MediaSource for file-based video (MP4, MKV). Wraps FfmpegBackend,
/// which handles seeking, decode threading, thumbnail cache, and
/// direction-aware scrub internally.
///
/// The main thread calls setTimestamp() to seek and takeFrame() to
/// poll for decoded frames. takeFrame() calls processEvents()
/// internally, so no external poll timer is needed.
class FileVideoSource : public MediaSource {
 public:
  /// Open a video file. Returns error if open fails.
  static Expected<std::unique_ptr<FileVideoSource>> open(const std::string& path);

  ~FileVideoSource() override;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

  // --- Additional API beyond MediaSource (for slider/transport UI) ---

  [[nodiscard]] double duration() const;  ///< Total duration in seconds
  [[nodiscard]] double position() const;  ///< Current playback position in seconds
  void setPaused(bool paused);            ///< Pause/resume playback
  [[nodiscard]] bool isPaused() const;    ///< True if paused (starts paused after open)
  void stepForward();                     ///< Advance by one frame
  void stepBackward();                    ///< Go back by one frame

  /// Subtract this wall-clock anchor from every setTimestamp() value before
  /// seeking the file. Producers that emit sdk::AssetVideo with a populated
  /// time_origin_ns pass that value here so the global tracker (epoch ns)
  /// maps to file-relative ns. Default 0 means "tracker time is already
  /// file-relative" — legacy file-from-zero behavior.
  void setEpochAnchorNs(int64_t anchor_ns);

  /// In-file playback window for assets that share their MP4 with other
  /// clips (e.g. one episode out of a concatenated LeRobot v3.0 video).
  /// When set, setTimestamp() clamps the file-relative seek position to
  /// [start_ns, end_ns]. Either bound may be `nullopt` to leave that side
  /// unclamped. Default (both absent) → unrestricted whole-file playback.
  void setClipWindowNs(std::optional<int64_t> start_ns, std::optional<int64_t> end_ns);

  /// Callbacks fired from takeFrame() (via processEvents) on the main thread.
  void setPositionCallback(VideoBackend::PositionCallback cb);
  void setDurationCallback(VideoBackend::DurationCallback cb);
  void setFileLoadedCallback(VideoBackend::FileLoadedCallback cb);

 private:
  FileVideoSource();

  std::unique_ptr<FfmpegBackend> backend_;

  // Latest frame from the backend's FrameCallback
  std::mutex frame_mutex_;
  std::optional<DecodedFrame> pending_frame_;

  // Wall-clock anchor subtracted in setTimestamp() before the backend seek.
  // Zero for unanchored video; producers populate via setEpochAnchorNs().
  int64_t epoch_anchor_ns_ = 0;

  // In-file clip window in ns; absent bounds mean "no clamp on that side".
  // Used by consumers that share a file across many clips (LeRobot v3.0).
  std::optional<int64_t> clip_start_ns_;
  std::optional<int64_t> clip_end_ns_;
};

}  // namespace PJ
