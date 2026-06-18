#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/async_frame_worker.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

class MessageParserPluginBase;

namespace sdk {
struct DepthImage;
}  // namespace sdk

/// MediaSource for depth image topics, rendered as a GPU-colormapped pixel frame.
///
/// Depth arrives as a canonical sdk::Image with a depth encoding (16UC1 mm, 32FC1
/// m, or compressedDepth PNG) — there is no kDepthImage producer. Decoding runs on
/// a dedicated worker thread (AsyncFrameWorker): the compressedDepth PNG inflate is
/// far too heavy for the UI thread (it dominated the profile). setTimestamp() posts
/// a request and returns immediately; the worker resolves the sdk::Image, converts
/// it to metric depth, and emits a RAW float32 depth frame (PixelFormat::kDepthR32F)
/// plus DepthColorParams — the colormap/range/invert are applied later on the GPU.
/// takeFrame() polls the latest decoded frame; setFrameReadyCallback() fires (on the
/// worker thread) when a new one is ready so the view can schedule a repaint.
///
/// Thread safety: setTimestamp(), takeFrame(), invalidate(), setFrameReadyCallback()
/// and the setters are main-thread-only. The colormap config the worker reads is
/// guarded by params_mutex_.
class DepthPipelineSource : public MediaSource {
 public:
  /// Canonical-blob ctor: each store entry is a serialized pj_image_v1 sdk::Image.
  /// @param store  ObjectStore to query (not owned)
  /// @param topic  Topic ID with serialized depth-encoded sdk::Image payloads
  DepthPipelineSource(ObjectStore* store, ObjectTopicId topic);

  /// Parser ctor: each store entry is a raw message decoded via the topic's
  /// MessageParser into an sdk::Image. Mirrors ImagePipelineSource's parser ctor —
  /// get the arguments from SessionManager::parserBindingForObjectTopic. `parser`
  /// is not owned; `parser_keepalive` keeps its instance + plugin DSO alive.
  DepthPipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  ~DepthPipelineSource() override;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

  /// Installs the callback the decode worker fires (FROM the worker thread) after
  /// each decode, so the view can schedule a repaint. Pass nullptr to clear.
  void setFrameReadyCallback(std::function<void()> cb);

  /// Select the colormap by id (== pj_widgets `Colormap` value == GPU LUT row).
  /// The core stays Qt-free and treats the id as opaque; the named enum lives in
  /// pj_widgets (the UI layer), keeping this Qt-free core free of that dependency.
  void setColormap(uint8_t colormap_id);
  void setInvert(bool invert);  ///< Mirror the colormap (t -> 1 - t).
  void setRange(float near_m, float far_m);
  void setOpacity(float opacity);

 private:
  /// Starts the decode worker (shared by both ctors). The worker thread calls
  /// decodeAt() per request; a forced (invalidate()) request re-decodes the
  /// current entry by resetting the dedup.
  void startWorker();

  /// Worker-thread entry: resolve + decode the store entry nearest `ts_ns` into a
  /// raw depth frame. Returns nullopt for no data, an unchanged entry (dedup), or
  /// an undecodable payload. Touches store_/parser_/last_entry_ts_ — worker-only.
  [[nodiscard]] std::optional<DecodedFrame> decodeAt(int64_t ts_ns);
  [[nodiscard]] std::optional<DecodedFrame> decodeDepthImage(const sdk::DepthImage& depth, int64_t pts) const;

  ObjectStore* store_;
  ObjectTopicId topic_;
  MessageParserPluginBase* parser_ = nullptr;  ///< raw-message decoder; null in canonical-blob mode
  std::shared_ptr<std::mutex> parser_mutex_;   ///< serializes the (non-thread-safe) parser
  std::shared_ptr<void> parser_keepalive_;     ///< keeps the parser instance + plugin DSO alive
  bool canonical_ = false;                     ///< true: store entries are pj_image_v1 blobs

  // Colormap config: written by the setters (main thread), read by the decode
  // worker in decodeDepthImage() — guarded by params_mutex_.
  mutable std::mutex params_mutex_;
  uint8_t colormap_ = 0;  ///< colormap id (pj_widgets Colormap == LUT row); 0 = turbo
  bool invert_ = false;
  float near_m_ = 0.0f;
  float far_m_ = 4.0f;
  float opacity_ = 1.0f;

  int64_t last_entry_ts_ = INT64_MIN;  ///< dedup of the last decoded entry — worker-thread only

  // Decodes off the UI thread. Declared last and stop()ed first in the destructor
  // so the worker's closure never touches an already-destroyed member.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
