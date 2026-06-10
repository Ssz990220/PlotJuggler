#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/thumbnail_codec.h"

namespace PJ {

/// Background builder + cache of HD-JPEG scrub thumbnails for an ObjectStore
/// VideoFrame topic. On its own thread it runs a single forward decode pass and
/// surfaces ~1 frame per adaptive interval (total bounded by a tile budget),
/// encodes each to a small JPEG (cap 1280), and serves the nearest-at-or-before
/// tile on lookup() — giving a scrub drag instant feedback while the real GOP
/// decode settles the exact frame. The pass decodes every frame (codec state is
/// sequential) but pays the HW-download + downscale only on the surfaced ones —
/// see StreamingVideoDecoder::decodeSampled.
///
/// Window-parameterized + prune-capable so the SAME builder serves a bounded
/// file (build once over the whole timeline) or, later, a paused-stream retained
/// window (build over [start,end], prune() as frames evict). Today only the file
/// path is wired.
///
/// Threading: buildAsync() spawns one worker; lookup()/prune()/size() are safe
/// concurrently with it. The dtor stops+joins. The supplied extractor is used
/// ONLY on the build thread — give the cache its OWN extractor (its keepalive
/// slot is single-threaded; do not share the playback decoder's). See
/// makeVideoFrameNalExtractor.
class EntryThumbnailCache {
 public:
  struct Config {
    int max_width = kThumbnailMaxWidth;         ///< downscale cap (px); source <= this -> skip (no benefit)
    int quality = kThumbnailJpegQuality;        ///< JPEG quality
    std::size_t max_tiles = 80;                 ///< total thumbnail budget (drives the adaptive interval)
    std::size_t max_bytes = 48u * 1024 * 1024;  ///< hard JPEG cache ceiling
  };

  /// Convenience form with the default Config (used by the file path).
  EntryThumbnailCache(ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor);
  /// Construct with an explicit Config.
  EntryThumbnailCache(
      ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor, Config cfg);
  ~EntryThumbnailCache();

  EntryThumbnailCache(const EntryThumbnailCache&) = delete;
  EntryThumbnailCache& operator=(const EntryThumbnailCache&) = delete;
  EntryThumbnailCache(EntryThumbnailCache&&) = delete;
  EntryThumbnailCache& operator=(EntryThumbnailCache&&) = delete;

  /// Kick off a background build over [start_ns, end_ns]. If end_ns < start_ns
  /// (the default), uses the topic's full current keyframe span. Cancels any
  /// prior build. Returns immediately.
  void buildAsync(Timestamp start_ns = 0, Timestamp end_ns = -1);

  /// Stop + join the build thread (idempotent).
  void stop();

  /// Nearest thumbnail at-or-before ts_ns, decompressed to a YUV420P frame.
  /// nullopt if the cache is empty or ts precedes the first tile.
  [[nodiscard]] std::optional<DecodedFrame> lookup(Timestamp ts_ns) const;

  /// Drop tiles with timestamp < before_ns (streaming eviction). No-op for files.
  void prune(Timestamp before_ns);

  [[nodiscard]] std::size_t size() const;

 private:
  void buildThread(Timestamp start_ns, Timestamp end_ns);

  struct Tile {
    Timestamp ts;
    std::vector<std::uint8_t> jpeg;
    int width;
    int height;
  };

  ObjectStore* store_;
  ObjectTopicId topic_;
  StreamingVideoDecoder::NalExtractor extractor_;
  Config cfg_;

  mutable std::mutex mutex_;  // guards tiles_ + bytes_
  std::vector<Tile> tiles_;   // sorted ascending by ts
  std::size_t bytes_ = 0;

  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace PJ
