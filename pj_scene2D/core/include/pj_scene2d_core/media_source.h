#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>

#include "pj_scene2d_core/media_frame.h"

namespace PJ {

/// Uniform frame-delivery interface between decoder backends and
/// MediaViewerWidget.  The main thread calls setTimestamp() when the
/// global time changes and takeFrame() at render rate.
///
/// Concrete implementations:
///   ImagePipelineSource  — worker-backed CodecPipeline + ObjectStore
///   StreamingVideoSource — wraps StreamingVideoDecoder + worker thread
///   DepthPipelineSource  — synchronous DepthImage colormap decode
///   ScenePipelineSource  — synchronous vector overlays decoded from ObjectStore
///   CompositeMediaSource — fans out across N layers, returns one MediaFrame
class MediaSource {
 public:
  virtual ~MediaSource() = default;

  MediaSource() = default;
  MediaSource(const MediaSource&) = delete;
  MediaSource& operator=(const MediaSource&) = delete;
  MediaSource(MediaSource&&) = delete;
  MediaSource& operator=(MediaSource&&) = delete;

  /// Called by main thread when the global time changes.
  /// May decode synchronously (depth/scene) or post to an internal worker
  /// (image/video), depending on the implementation.
  virtual void setTimestamp(int64_t ts_ns) = 0;

  /// Called by main thread at render rate.
  /// Returns the latest MediaFrame (base pixels and/or overlays), or nullopt
  /// if nothing new since the last call.
  virtual std::optional<MediaFrame> takeFrame() = 0;

  /// Clear any "already produced this timestamp" caches so the next
  /// setTimestamp() re-decodes even at an unchanged time. The compositor calls
  /// this after a layer rebuild (add/remove/visibility): the underlying sources
  /// are re-seeded at the same tracker time and would otherwise dedup the
  /// request away, leaving a stale or empty frame until the tracker next moves.
  /// Default: no-op (sources without a timestamp cache need nothing).
  virtual void invalidate() {}
};

}  // namespace PJ
