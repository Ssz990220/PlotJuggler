#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/cancel_token.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

class EntryThumbnailCache;
class MessageParserPluginBase;
class StreamingVideoDecoder;

/// MediaSource for streaming video from ObjectStore. Wraps
/// StreamingVideoDecoder on a dedicated worker thread.
///
/// setTimestamp() posts a request to the worker; takeFrame() polls
/// for the latest decoded result. Latest-wins semantics — if a new
/// timestamp arrives while the worker is busy, it picks up the new
/// request after completing the current decode.
///
/// Thread safety: setTimestamp() and takeFrame() must be called from
/// the main thread only. The worker thread is internal.
///
/// Ownership: `store` is NOT owned (must outlive this object).
/// See ARCHITECTURE.md §5.3.
class StreamingVideoSource : public MediaSource {
 public:
  /// Raw-bytes constructor: each ObjectStore entry already holds the raw
  /// Annex-B NAL stream for one frame (the simulated-stream / round-trip path).
  /// @param store  ObjectStore containing video entries (not owned)
  /// @param topic  Topic ID with H.264 VideoFrame entries
  StreamingVideoSource(ObjectStore* store, ObjectTopicId topic);

  /// Parser-mode constructor: each ObjectStore entry holds a wrapping canonical
  /// PJ.VideoFrame / Foxglove CompressedVideo message. The decoder installs a
  /// NAL extractor that, on each entry, locks `parser_mutex` (MessageParser
  /// plugins are not thread-safe — fastcdr et al. keep stateful scratch), calls
  /// `parser->parseObject`, any_casts the result to sdk::VideoFrame, and returns
  /// the contained `data` span. The span aliases the entry's buffer (kept alive
  /// by the resolved entry across extract+decode) — no copy of the H.264 blob.
  /// @param store         ObjectStore containing video entries (not owned)
  /// @param topic         Topic ID with VideoFrame message entries
  /// @param parser        MessageParser for this topic (not owned; may be null,
  ///                      in which case extraction always fails and no frame
  ///                      surfaces — mirrors ImagePipelineSource).
  /// @param parser_mutex  Shared mutex serialising parseObject across consumers
  ///                      of this parser singleton (may be null).
  /// @param parser_keepalive  Opaque shared owner of the parser handle (parser
  ///                      instance + plugin DSO). Held for this source's whole
  ///                      lifetime so the parser can't be torn down / dlclosed
  ///                      under an in-flight parseObject on the worker thread.
  ///                      Get it from SessionManager::parserKeepaliveForObjectTopic.
  StreamingVideoSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  ~StreamingVideoSource() override;

  StreamingVideoSource(const StreamingVideoSource&) = delete;
  StreamingVideoSource& operator=(const StreamingVideoSource&) = delete;
  StreamingVideoSource(StreamingVideoSource&&) = delete;
  StreamingVideoSource& operator=(StreamingVideoSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

  /// Install a notification fired (FROM the worker thread) every time the worker
  /// has deposited a fresh decoded frame, i.e. takeFrame() now has data. Pass
  /// nullptr to clear. Mirrors ImagePipelineSource::setFrameReadyCallback: the
  /// decode is asynchronous, so a single takeFrame() right after setTimestamp()
  /// races the worker; consumers (the GUI dock) hook this to re-poll once the
  /// frame is ready instead of leaving a stopped scrub frozen on the previous
  /// frame. The callback runs on the worker thread — implementers must hop to
  /// their own thread (e.g. QMetaObject::invokeMethod(..., Qt::QueuedConnection)).
  /// Must be called from the same (main) thread as setTimestamp()/takeFrame().
  void setFrameReadyCallback(std::function<void()> cb);

  [[nodiscard]] bool isInitialized() const;

 private:
  void workerLoop();
  // Publish a decoded frame to the result channel and fire the frame-ready
  // callback. Used by the worker for both the instant thumbnail preview and the
  // settled full-res frame.
  void depositFrame(DecodedFrame frame);

  std::unique_ptr<StreamingVideoDecoder> decoder_;

  // Keeps the MessageParser handle (parser instance + plugin DSO) mapped for as
  // long as this source — and thus the decode worker that calls parseObject — is
  // alive. Dropped only after the worker is joined in the destructor, so the
  // parser can never be torn down (dlclose) underneath an in-flight parseObject.
  // Null in raw-bytes mode. See SessionManager::parserKeepaliveForObjectTopic.
  std::shared_ptr<void> parser_keepalive_;

  // Scrub-preview thumbnail cache (file-backed topics only; null for streaming
  // and raw-bytes modes). Declared AFTER parser_keepalive_ so it is destroyed —
  // and its build thread joined — BEFORE the parser keepalive drops: the build
  // thread decodes keyframes through the (DSO-kept-alive) parser.
  std::unique_ptr<EntryThumbnailCache> thumbnail_cache_;

  // Request channel (main → worker)
  std::mutex request_mutex_;
  std::condition_variable request_cv_;
  int64_t requested_ts_ = INT64_MIN;
  int64_t last_requested_ts_ = INT64_MIN;
  bool has_request_ = false;
  // Cancels the worker's in-flight decode when a newer setTimestamp() arrives,
  // so a slow (4K) GOP decode is preempted rather than finished-then-discarded.
  // Guarded by request_mutex_; the worker installs a fresh token per request.
  CancelTokenPtr cancel_token_;
  std::atomic<bool> running_{true};

  // Result channel (worker → main)
  std::mutex result_mutex_;
  std::optional<DecodedFrame> result_frame_;

  // Frame-ready notification (worker → consumer). Guarded for safe replacement
  // while the worker runs; invoked outside result_mutex_ after each deposit.
  std::mutex callback_mutex_;
  std::function<void()> on_frame_ready_;

  std::thread worker_;
};

}  // namespace PJ
