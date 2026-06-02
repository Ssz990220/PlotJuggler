// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/file_video_source.h"

#include "pj_scene2d_core/ffmpeg_backend.h"

namespace PJ {

FileVideoSource::FileVideoSource() : backend_(std::make_unique<FfmpegBackend>()) {}

FileVideoSource::~FileVideoSource() = default;

Expected<std::unique_ptr<FileVideoSource>> FileVideoSource::open(const std::string& path) {
  auto source = std::unique_ptr<FileVideoSource>(new FileVideoSource());

  // Wire internal frame callback to capture latest decoded frame
  source->backend_->setFrameCallback([source_ptr = source.get()](const DecodedFrame& frame) {
    std::lock_guard lock(source_ptr->frame_mutex_);
    source_ptr->pending_frame_ = frame;
  });

  if (!source->backend_->open(path)) {
    return unexpected("failed to open video: " + path);
  }

  return source;
}

void FileVideoSource::setTimestamp(int64_t ts_ns) {
  // Map tracker (epoch) ns → file-relative ns. epoch_anchor_ns_ defaults to 0
  // (legacy file-from-zero), so unanchored video sees ts_ns unchanged.
  int64_t file_relative_ns = ts_ns - epoch_anchor_ns_;
  // Clamp into the clip window when present. Producers that share an MP4
  // across many clips (LeRobot v3.0) set [clip_start_ns_, clip_end_ns_] so
  // the tracker cannot seek past the episode's slice in either direction.
  if (clip_start_ns_.has_value() && file_relative_ns < *clip_start_ns_) {
    file_relative_ns = *clip_start_ns_;
  }
  if (clip_end_ns_.has_value() && file_relative_ns > *clip_end_ns_) {
    file_relative_ns = *clip_end_ns_;
  }
  double seconds = static_cast<double>(file_relative_ns) / 1'000'000'000.0;
  backend_->seek(seconds);
}

void FileVideoSource::setEpochAnchorNs(int64_t anchor_ns) {
  epoch_anchor_ns_ = anchor_ns;
}

void FileVideoSource::setClipWindowNs(std::optional<int64_t> start_ns, std::optional<int64_t> end_ns) {
  clip_start_ns_ = start_ns;
  clip_end_ns_ = end_ns;
}

std::optional<MediaFrame> FileVideoSource::takeFrame() {
  // processEvents fires the frame callback on the caller's thread,
  // which populates pending_frame_ under frame_mutex_.
  backend_->processEvents();

  std::lock_guard lock(frame_mutex_);
  if (!pending_frame_.has_value()) {
    return std::nullopt;
  }
  MediaFrame mf;
  mf.base = std::move(*pending_frame_);
  pending_frame_.reset();
  return mf;
}

double FileVideoSource::duration() const {
  return backend_->duration();
}

double FileVideoSource::position() const {
  return backend_->position();
}

void FileVideoSource::setPaused(bool paused) {
  backend_->setPaused(paused);
}

bool FileVideoSource::isPaused() const {
  return backend_->isPaused();
}

void FileVideoSource::stepForward() {
  backend_->stepForward();
}

void FileVideoSource::stepBackward() {
  backend_->stepBackward();
}

void FileVideoSource::setPositionCallback(VideoBackend::PositionCallback cb) {
  backend_->setPositionCallback(std::move(cb));
}

void FileVideoSource::setDurationCallback(VideoBackend::DurationCallback cb) {
  backend_->setDurationCallback(std::move(cb));
}

void FileVideoSource::setFileLoadedCallback(VideoBackend::FileLoadedCallback cb) {
  backend_->setFileLoadedCallback(std::move(cb));
}

}  // namespace PJ
