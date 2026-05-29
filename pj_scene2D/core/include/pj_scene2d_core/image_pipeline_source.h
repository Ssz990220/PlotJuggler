#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/codec_pipeline.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

class MessageParserPluginBase;

/// MediaSource for image topics (JPEG, PNG, depth, segmentation).
///
/// Production object topics use the parser-driven constructor: raw bytes from
/// ObjectStore are translated by the bound MessageParser into canonical
/// sdk::Image, then decoded by Image::encoding. Demo utilities may still use
/// the pipeline-driven constructor when they push already-prepared image bytes.
///
/// Decode runs on a dedicated worker thread (mirrors StreamingVideoSource).
/// setTimestamp() posts a request to the worker and returns immediately;
/// takeFrame() polls the latest decoded result. New requests during decode
/// overwrite the pending target — latest-target-wins coalescing keeps the
/// queue at depth one regardless of slider drag rate.
///
/// Notification: when a new frame is ready, the worker invokes the callback
/// installed via setFrameReadyCallback() (called FROM the worker thread, so
/// implementers must hop back to the GUI thread themselves — typically with
/// QMetaObject::invokeMethod(..., Qt::QueuedConnection)).
///
/// Thread safety: setTimestamp(), takeFrame(), and setFrameReadyCallback()
/// must be called from the same (main) thread. The worker thread is internal.
///
/// Ownership: `store` and `parser` are NOT owned. `pipeline` is owned.
/// Precondition: `store` is non-null and outlives this source.
/// See ARCHITECTURE.md §5.1.
class ImagePipelineSource : public MediaSource {
 public:
  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  /// @param parser  Parser to drive canonical-image decode (not owned)
  /// @param parser_mutex  Shared mutex serialising parseObject across consumers
  ///   of the same parser. MessageParser plugins aren't thread-safe (stateful
  ///   scratch), so sources sharing a parser pointer MUST share a mutex. Pass
  ///   nullptr only when the parser is private to this source (e.g. unit tests).
  ImagePipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex = nullptr);

  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  /// @param pipeline  Codec pipeline for decode (owned, moved in)
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline);

  ~ImagePipelineSource() override;

  ImagePipelineSource(const ImagePipelineSource&) = delete;
  ImagePipelineSource& operator=(const ImagePipelineSource&) = delete;
  ImagePipelineSource(ImagePipelineSource&&) = delete;
  ImagePipelineSource& operator=(ImagePipelineSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;

  /// Install a notification fired (from the worker thread) every time
  /// takeFrame() has new data to return. Pass nullptr to clear.
  void setFrameReadyCallback(std::function<void()> cb);

 private:
  void workerLoop();
  std::optional<DecodedFrame> decodeAt(int64_t ts_ns);

  // I/O config and codec state — read/written by worker thread only after
  // construction finishes. The main thread treats them as read-only after
  // the constructor returns.
  ObjectStore* store_;
  ObjectTopicId topic_;
  std::string source_key_;
  MessageParserPluginBase* parser_ = nullptr;
  std::shared_ptr<std::mutex> parser_mutex_;
  std::unique_ptr<CodecPipeline> pipeline_;
  JpegCodec jpeg_codec_;
  PngCodec png_codec_;
  NormalizeMono16 normalize_mono16_;
  AutoImageCodec auto_image_codec_;
  int64_t last_entry_ts_ = INT64_MIN;

  // Request channel (main → worker).
  std::mutex request_mutex_;
  std::condition_variable request_cv_;
  int64_t requested_ts_ = INT64_MIN;
  int64_t last_requested_ts_ = INT64_MIN;  // main-thread-only dedup
  bool has_request_ = false;
  std::atomic<bool> running_{true};

  // Result channel (worker → main).
  std::mutex result_mutex_;
  std::optional<DecodedFrame> result_frame_;

  // Frame-ready notification.
  std::mutex callback_mutex_;
  std::function<void()> on_frame_ready_;

  // Declared LAST so it joins before any field it touches goes away. The
  // explicit join in the destructor body is the primary safety net; this
  // ordering is defence in depth for incomplete construction.
  std::thread worker_;
};

}  // namespace PJ
