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
#include <string>
#include <thread>
#include <unordered_map>

#include "pj_base/builtin/camera_info.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/codec_pipeline.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/undistort_remap.h"

namespace PJ {

class MessageParserPluginBase;
namespace sdk {
struct Image;
}  // namespace sdk

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
  /// @param parser_keepalive  Opaque shared owner of the parser handle (parser
  ///   instance + plugin DSO). Held for this source's whole lifetime so the
  ///   parser can't be torn down / dlclosed under an in-flight parseObject on the
  ///   worker thread. Get it from SessionManager::parserKeepaliveForObjectTopic.
  ImagePipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  /// @param pipeline  Codec pipeline for decode (owned, moved in)
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline);

  /// Tag selecting the per-frame canonical-image route: each ObjectStore entry's
  /// bytes are a serialized sdk::Image (pj_base pj_image_v1 codec). Used by
  /// producers (e.g. the mosaico toolbox) that push canonical Image blobs via
  /// pushOwnedObject and register the topic with metadata image_codec=pj_image_v1
  /// — there is no MessageParser, so the source deserializes each blob itself.
  struct CanonicalImageCodec {};

  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, CanonicalImageCodec tag);

  ~ImagePipelineSource() override;

  ImagePipelineSource(const ImagePipelineSource&) = delete;
  ImagePipelineSource& operator=(const ImagePipelineSource&) = delete;
  ImagePipelineSource(ImagePipelineSource&&) = delete;
  ImagePipelineSource& operator=(ImagePipelineSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

  /// Install a notification fired (from the worker thread) every time
  /// takeFrame() has new data to return. Pass nullptr to clear.
  void setFrameReadyCallback(std::function<void()> cb);

  /// Provide the camera calibration used to rectify decoded frames, keyed by
  /// CameraInfo.frame_id. The owner (which has the session's parser registry)
  /// parses each "<ns>/camera_info" topic and passes the result here. Must be
  /// called on the main thread before the first setTimestamp(); empty disables
  /// rectification (frames pass through unmodified).
  void setCameraInfoMap(std::unordered_map<std::string, sdk::CameraInfo> by_frame_id);

 private:
  void workerLoop();
  std::optional<DecodedFrame> decodeAt(int64_t ts_ns);

  // Decode one canonical sdk::Image (from a parser or a deserialized blob) into a
  // display frame: raw/bayer encodings via the raw-or-bayer path (incl. grayscale
  // PNG-wrapped recovery), otherwise the jpeg/png/auto compressed cascade.
  std::optional<DecodedFrame> decodeCanonicalImage(const sdk::Image& img, int64_t pts);

  // Lens-undistort a decoded frame in place when calibration exists for its
  // frame_id, emitting it at the camera's native (calibrated) resolution so 2D
  // annotation overlays authored in that space line up. No-op without calibration.
  //
  // ASSUMPTION (not a universal law): a usable CameraInfo for this frame implies
  // the image is still in raw sensor space and its annotations are authored in the
  // native *rectified* space (the Waymo / Foxglove / ROS image_proc convention).
  // CameraInfo describes the lens, NOT whether THIS stream was already rectified,
  // so a producer that logs a pre-rectified image alongside its CameraInfo would
  // be rectified twice. See docs/TECHNICAL_NOTES.md §12 for the rationale and the
  // known blind spot.
  void rectifyIfCalibrated(DecodedFrame& df);

  // Return the injected CameraInfo whose frame_id matches, or nullptr when none.
  // A plain lookup into the map set by setCameraInfoMap(); worker-thread-only.
  const sdk::CameraInfo* cameraInfoFor(const std::string& frame_id);

  // I/O config and codec state — read/written by worker thread only after
  // construction finishes. The main thread treats them as read-only after
  // the constructor returns.
  ObjectStore* store_;
  ObjectTopicId topic_;
  std::string source_key_;
  MessageParserPluginBase* parser_ = nullptr;
  std::shared_ptr<std::mutex> parser_mutex_;
  // Keeps the parser handle (instance + plugin DSO) alive for this source's whole
  // lifetime — see SessionManager::parserKeepaliveForObjectTopic. Dropped after
  // the worker joins (destructor body), so parseObject can't run on a freed/
  // dlclosed parser during teardown. Null when no parser is used.
  std::shared_ptr<void> parser_keepalive_;
  std::unique_ptr<CodecPipeline> pipeline_;
  bool canonical_image_codec_ = false;
  JpegCodec jpeg_codec_;
  PngCodec png_codec_;
  NormalizeMono16 normalize_mono16_;
  AutoImageCodec auto_image_codec_;
  int64_t last_entry_ts_ = INT64_MIN;

  // Rectification state. camera_info_by_frame_ is injected once (main thread,
  // before first decode) via setCameraInfoMap; each camera's reverse map is then
  // built on first use and reused thereafter (worker-thread-only).
  std::unordered_map<std::string, sdk::CameraInfo> camera_info_by_frame_;
  std::unordered_map<std::string, UndistortMap> undistort_by_frame_;
  // Set true by the first setTimestamp() (main thread). Guards setCameraInfoMap:
  // once a decode has been requested the worker may read camera_info_by_frame_
  // unlocked, so a later injection would be a data race — it is refused instead.
  bool timestamp_requested_ = false;

  // Request channel (main → worker).
  std::mutex request_mutex_;
  std::condition_variable request_cv_;
  int64_t requested_ts_ = INT64_MIN;
  int64_t last_requested_ts_ = INT64_MIN;  // main-thread-only dedup
  bool has_request_ = false;
  // Set by invalidate() (under request_mutex_), consumed by the worker: clears
  // the worker-side last_entry_ts_ dedup so the next decode re-runs even on the
  // same entry (a composite rebuild needs a fresh frame at the unchanged time).
  bool force_redecode_ = false;
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
