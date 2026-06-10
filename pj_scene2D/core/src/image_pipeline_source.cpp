// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/image_pipeline_source.h"

#include <fmt/format.h>

#include <algorithm>
#include <any>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_scene2d_core/image_rectifier.h"

namespace PJ {

namespace {

// Once-per-source/error warning sink. Core stays Qt-free per CMakeLists;
// decoder/parser failure surfaces must reach the operator without requiring
// env-var gating, but playback can revisit the same failing frame many times.
template <typename... Args>
void warnOnce(const std::string& key, fmt::format_string<Args...> fmt_str, Args&&... args) {
  static std::mutex mutex;
  static std::unordered_set<std::string> emitted;
  {
    std::lock_guard lock(mutex);
    if (!emitted.insert(key).second) {
      return;
    }
  }
  fmt::print(stderr, "[ImagePipelineSource] ");
  fmt::print(stderr, fmt_str, std::forward<Args>(args)...);
  fmt::print(stderr, "\n");
}

[[nodiscard]] std::string warningKey(std::string_view source_key, std::string_view reason) {
  std::string key;
  key.reserve(source_key.size() + reason.size() + 1);
  key.append(source_key);
  key.push_back(':');
  key.append(reason);
  return key;
}

std::string sourceLabel(const ObjectStore* store, ObjectTopicId topic) {
  if (store == nullptr) {
    return fmt::format("topic_id={}", topic.id);
  }
  const auto& descriptor = store->descriptor(topic);
  return fmt::format("dataset={} topic='{}' topic_id={}", descriptor.dataset_id, descriptor.topic_name, topic.id);
}

struct RawEncodingInfo {
  PixelFormat format;
  uint32_t bytes_per_pixel;
};

std::optional<RawEncodingInfo> rawEncodingInfo(std::string_view encoding) noexcept {
  if (encoding == "rgb8") {
    return RawEncodingInfo{PixelFormat::kRGB888, 3};
  }
  if (encoding == "rgba8") {
    return RawEncodingInfo{PixelFormat::kRGBA8888, 4};
  }
  if (encoding == "bgr8") {
    return RawEncodingInfo{PixelFormat::kBGR888, 3};
  }
  if (encoding == "bgra8") {
    return RawEncodingInfo{PixelFormat::kBGRA8888, 4};
  }
  if (encoding == "mono8") {
    return RawEncodingInfo{PixelFormat::kMono8, 1};
  }
  if (encoding == "mono16" || encoding == "16UC1") {
    return RawEncodingInfo{PixelFormat::kMono16, 2};
  }
  return std::nullopt;
}

std::optional<DecodedFrame> imageToDecodedFrame(const sdk::Image& img, const RawEncodingInfo& info, int64_t pts) {
  if (img.width == 0 || img.height == 0 || img.data.empty()) {
    return std::nullopt;
  }

  const size_t row_bytes = static_cast<size_t>(img.width) * info.bytes_per_pixel;
  const size_t expected = row_bytes * img.height;
  auto pixels = std::make_shared<std::vector<uint8_t>>();

  if (img.row_step == 0 || img.row_step == row_bytes) {
    if (img.data.size() < expected) {
      return std::nullopt;
    }
    pixels->assign(img.data.data(), img.data.data() + expected);
  } else if (img.row_step >= row_bytes && img.data.size() >= static_cast<size_t>(img.row_step) * img.height) {
    // size_t multiply: row_step*height in 32-bit can wrap to a tiny value and let
    // a too-small buffer past the guard, OOB-reading in the per-row copy below.
    pixels->resize(expected);
    for (uint32_t r = 0; r < img.height; ++r) {
      const auto* src = img.data.data() + static_cast<size_t>(r) * img.row_step;
      auto* dst = pixels->data() + static_cast<size_t>(r) * row_bytes;
      std::memcpy(dst, src, row_bytes);
    }
  } else {
    return std::nullopt;
  }

  DecodedFrame out;
  out.pixels = std::move(pixels);
  out.width = static_cast<int>(img.width);
  out.height = static_cast<int>(img.height);
  out.format = info.format;
  out.pts = pts;
  return out;
}

bool isJpegEncoding(std::string_view encoding) noexcept {
  return encoding == "jpeg" || encoding == "jpg";
}

bool isPngEncoding(std::string_view encoding) noexcept {
  return encoding == "png" || encoding == "compressedDepth";
}

bool startsWithIhdrChunkType(const uint8_t* data, size_t size) noexcept {
  return data != nullptr && size >= 4 && data[0] == 'I' && data[1] == 'H' && data[2] == 'D' && data[3] == 'R';
}

std::shared_ptr<std::vector<uint8_t>> imageDataBytes(const sdk::Image& img, std::string_view topic_name) {
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  if (img.data.empty()) {
    return bytes;
  }

  if (img.encoding == "compressedDepth" && startsWithIhdrChunkType(img.data.data(), img.data.size())) {
    // Some ROS compressedDepth streams carry a plain PNG but advertise the
    // compressedDepth format. Parser-level compatibility can then expose the
    // stream starting at IHDR. Repair only this canonical compressedDepth
    // shape; CDR/message-envelope parsing still belongs to the parser plugin.
    static constexpr uint8_t kPngPrefix[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    };
    bytes->reserve(sizeof(kPngPrefix) + img.data.size());
    bytes->insert(bytes->end(), kPngPrefix, kPngPrefix + sizeof(kPngPrefix));
    bytes->insert(bytes->end(), img.data.data(), img.data.data() + img.data.size());
    (void)topic_name;  // intentional: repair path is silent once the codec call succeeds
    return bytes;
  }

  bytes->assign(img.data.data(), img.data.data() + img.data.size());
  return bytes;
}

std::optional<BayerPattern> bayerPatternFor(std::string_view encoding) noexcept {
  if (encoding == "bayer_rggb8") {
    return BayerPattern::kRGGB;
  }
  if (encoding == "bayer_grbg8") {
    return BayerPattern::kGRBG;
  }
  if (encoding == "bayer_gbrg8") {
    return BayerPattern::kGBRG;
  }
  if (encoding == "bayer_bggr8") {
    return BayerPattern::kBGGR;
  }
  return std::nullopt;
}

bool hasPngOrJpegSignature(const uint8_t* data, size_t size) noexcept {
  if (data == nullptr) {
    return false;
  }
  const bool jpeg = size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
  // Full 8-byte PNG signature (\x89PNG\r\n\x1a\n), not just the first four bytes:
  // a 4-byte prefix match leaves a 1-in-2^32 chance that a raw/Bayer buffer is
  // mistaken for a PNG container, and a failed sniff used to drop the frame.
  const bool png = size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
                   data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
  return jpeg || png;
}

// Reduce a decoded frame to a single-channel kMono8 buffer by taking one channel.
// A grayscale PNG decodes to kRGB888 with R==G==B, so any channel recovers the
// original samples exactly. A grayscale JPEG is lossy: R/G/B may differ slightly
// and the value only approximates the original sample, so JPEG wrapping is
// best-effort display recovery, not byte-exact. Raw mono8 passes through unchanged.
std::optional<DecodedFrame> toMono8Mosaic(const DecodedFrame& frame) {
  if (frame.isNull() || frame.width <= 0 || frame.height <= 0) {
    return std::nullopt;
  }
  const size_t pixel_count = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
  size_t stride = 0;
  switch (frame.format) {
    case PixelFormat::kMono8:
      stride = 1;
      break;
    case PixelFormat::kRGB888:
    case PixelFormat::kBGR888:
      stride = 3;
      break;
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGRA8888:
      stride = 4;
      break;
    case PixelFormat::kMono16:
    case PixelFormat::kYUV420P:
    case PixelFormat::kNV12:
      return std::nullopt;
  }
  const auto& src = *frame.pixels;
  if (src.size() < pixel_count * stride) {
    return std::nullopt;
  }
  auto out = std::make_shared<std::vector<uint8_t>>(pixel_count);
  for (size_t i = 0; i < pixel_count; ++i) {
    (*out)[i] = src[i * stride];
  }
  DecodedFrame mono;
  mono.pixels = std::move(out);
  mono.width = frame.width;
  mono.height = frame.height;
  mono.format = PixelFormat::kMono8;
  mono.pts = frame.pts;
  return mono;
}

// Decode a canonical sdk::Image whose encoding names raw pixel semantics
// (rgb8/bgr8/mono8/mono16/16UC1) or a Bayer CFA (bayer_*). Some transports wrap
// the flat byte buffer in an 8-bit grayscale PNG (lossless, width = stride) or
// JPEG (lossy — recovered samples only approximate the originals). When a
// container signature is present we decompress and recover the flat bytes before
// reinterpreting at the logical geometry; if that decode fails (e.g. a raw buffer
// whose first bytes coincidentally look like a container), we fall back to
// interpreting the original bytes as raw rather than dropping the frame. Bayer
// mosaics are then demosaiced to RGB888.
std::optional<DecodedFrame> decodeRawOrBayerImage(
    const sdk::Image& img, int64_t pts, std::string_view source_key, const AutoImageCodec& auto_codec,
    const NormalizeMono16& normalize) {
  if (img.width == 0 || img.height == 0 || img.data.empty()) {
    warnOnce(
        warningKey(source_key, "empty-raw-image"), "{} raw/bayer image has empty data/geometry encoding={} size={}x{}",
        source_key, img.encoding, img.width, img.height);
    return std::nullopt;
  }

  sdk::Image flat = img;
  std::shared_ptr<std::vector<uint8_t>> recovered;
  if (hasPngOrJpegSignature(img.data.data(), img.data.size())) {
    DecodedFrame staged;
    staged.pixels = std::make_shared<std::vector<uint8_t>>(img.data.data(), img.data.data() + img.data.size());
    // On any failure, leave `flat == img` and fall through to the raw path: the
    // signature may be a coincidence in a genuinely raw buffer (or a corrupt
    // container), and the geometry check below drops it if the bytes don't fit.
    if (auto decompressed = auto_codec.decode(staged); !decompressed.has_value()) {
      warnOnce(
          warningKey(source_key, "container-decode"), "{} container decode failed encoding={}: {}; falling back to raw",
          source_key, img.encoding, decompressed.error());
    } else if (auto mono = toMono8Mosaic(*decompressed); !mono.has_value()) {
      warnOnce(
          warningKey(source_key, "container-flatten"),
          "{} could not recover flat bytes encoding={}; falling back to raw", source_key, img.encoding);
    } else {
      recovered = mono->pixels;
      flat.data = Span<const uint8_t>(recovered->data(), recovered->size());
    }
  }

  // Bayer mosaics: stage the CFA samples as a mono8 frame, then demosaic to RGB.
  if (auto pattern = bayerPatternFor(flat.encoding); pattern.has_value()) {
    auto mosaic = imageToDecodedFrame(flat, RawEncodingInfo{PixelFormat::kMono8, 1}, pts);
    if (!mosaic.has_value()) {
      warnOnce(
          warningKey(source_key, "bayer-staging"), "{} bayer staging failed encoding={} size={}x{}", source_key,
          flat.encoding, flat.width, flat.height);
      return std::nullopt;
    }
    auto rgb = BayerDecode(*pattern).decode(*mosaic);
    if (!rgb.has_value()) {
      warnOnce(
          warningKey(source_key, "bayer-demosaic"), "{} bayer demosaic failed encoding={}: {}", source_key,
          flat.encoding, rgb.error());
      return std::nullopt;
    }
    rgb->pts = pts;
    return std::move(*rgb);
  }

  // Fixed-layout raw encodings.
  const auto info = rawEncodingInfo(flat.encoding);
  if (!info.has_value()) {
    warnOnce(
        warningKey(source_key, "unsupported-raw-encoding"), "{} unsupported raw encoding={}", source_key,
        flat.encoding);
    return std::nullopt;
  }
  auto decoded = imageToDecodedFrame(flat, *info, pts);
  if (!decoded.has_value()) {
    warnOnce(
        warningKey(source_key, "raw-conversion"), "{} raw image conversion failed encoding={} size={}x{}", source_key,
        flat.encoding, flat.width, flat.height);
    return std::nullopt;
  }
  auto normalized = normalize.decode(*decoded);
  if (!normalized.has_value()) {
    warnOnce(
        warningKey(source_key, "raw-normalization"), "{} raw normalization failed: {}", source_key, normalized.error());
    return std::nullopt;
  }
  normalized->pts = pts;
  return std::move(*normalized);
}

}  // namespace

ImagePipelineSource::ImagePipelineSource(
    ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex,
    std::shared_ptr<void> parser_keepalive)
    : store_(store),
      topic_(topic),
      source_key_(sourceLabel(store, topic)),
      parser_(parser),
      parser_mutex_(std::move(parser_mutex)),
      parser_keepalive_(std::move(parser_keepalive)) {
  worker_ = std::thread(&ImagePipelineSource::workerLoop, this);
}

ImagePipelineSource::ImagePipelineSource(
    ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline)
    : store_(store), topic_(topic), source_key_(sourceLabel(store, topic)), pipeline_(std::move(pipeline)) {
  worker_ = std::thread(&ImagePipelineSource::workerLoop, this);
}

ImagePipelineSource::ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, CanonicalImageCodec /*tag*/)
    : store_(store), topic_(topic), source_key_(sourceLabel(store, topic)), canonical_image_codec_(true) {
  worker_ = std::thread(&ImagePipelineSource::workerLoop, this);
}

ImagePipelineSource::~ImagePipelineSource() {
  // Flip running_ UNDER request_mutex_: the worker checks `!running_` inside its
  // wait predicate while holding the same mutex, so storing it unlocked here can
  // slip into the gap after the worker evaluates the predicate (running_ still
  // true) but before it blocks — then notify_one() wakes nobody and the worker
  // sleeps forever, hanging join(). (Root cause of the scene2d_dock_widget_test
  // intermittent deadlock.)
  {
    std::lock_guard lock(request_mutex_);
    running_.store(false);
  }
  request_cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void ImagePipelineSource::setTimestamp(int64_t ts_ns) {
  // Mark that a decode has been requested: from here the worker may read
  // camera_info_by_frame_, so setCameraInfoMap() must refuse late injection.
  timestamp_requested_ = true;
  if (ts_ns == last_requested_ts_) {
    return;
  }
  last_requested_ts_ = ts_ns;

  {
    std::lock_guard lock(request_mutex_);
    requested_ts_ = ts_ns;
    has_request_ = true;
  }
  request_cv_.notify_one();
}

void ImagePipelineSource::invalidate() {
  // Force the next setTimestamp() to re-request a decode even at the same time
  // (the composite was rebuilt and our last frame was consumed). The worker
  // re-delivers via the frame-ready callback, which schedules a repaint.
  last_requested_ts_ = std::numeric_limits<int64_t>::min();
  // Also clear the worker-side dedup: without this, decodeAt() resolves to the
  // same entry as last time and early-returns nullopt, so the worker never
  // produces a frame and the repaint never fires (the "black until play" bug).
  std::lock_guard lock(request_mutex_);
  force_redecode_ = true;
}

std::optional<MediaFrame> ImagePipelineSource::takeFrame() {
  std::lock_guard lock(result_mutex_);
  if (!result_frame_.has_value()) {
    return std::nullopt;
  }
  MediaFrame mf;
  mf.base = std::move(*result_frame_);
  result_frame_.reset();
  return mf;
}

void ImagePipelineSource::setFrameReadyCallback(std::function<void()> cb) {
  std::lock_guard lock(callback_mutex_);
  on_frame_ready_ = std::move(cb);
}

void ImagePipelineSource::workerLoop() {
  while (running_.load()) {
    int64_t ts = 0;
    bool force = false;
    {
      std::unique_lock lock(request_mutex_);
      request_cv_.wait(lock, [this] { return has_request_ || !running_.load(); });
      if (!running_.load()) {
        break;
      }
      ts = requested_ts_;
      has_request_ = false;
      force = force_redecode_;
      force_redecode_ = false;
    }

    // invalidate() asked for a fresh decode even at an unchanged entry: clear the
    // dedup so decodeAt() doesn't early-return nullopt on the same frame.
    if (force) {
      last_entry_ts_ = INT64_MIN;
    }
    // Exception barrier (§R5 / ARCHITECTURE §10.5): a throwing parser plugin or a
    // std::bad_alloc on pathological geometry must not escape the std::thread
    // callable (that calls std::terminate). Catch at the boundary, warn once, and
    // keep the worker alive to serve the next request.
    std::optional<DecodedFrame> result;
    try {
      result = decodeAt(ts);
    } catch (const std::exception& ex) {
      warnOnce(warningKey(source_key_, "decode-exception"), "{} decode threw: {}", source_key_, ex.what());
    } catch (...) {
      warnOnce(warningKey(source_key_, "decode-exception"), "{} decode threw a non-std exception", source_key_);
    }
    if (result.has_value() && !result->isNull()) {
      {
        std::lock_guard lock(result_mutex_);
        result_frame_ = std::move(*result);
      }
      // Notify outside result_mutex_ so the callback can read the result via
      // takeFrame() without contending. The callback is itself locked behind
      // callback_mutex_ for safe replacement during a running worker.
      std::function<void()> cb_copy;
      {
        std::lock_guard lock(callback_mutex_);
        cb_copy = on_frame_ready_;
      }
      if (cb_copy) {
        cb_copy();
      }
    }

    // Pick up any newer request that arrived during decode — keeps latency
    // bounded to one decode after the last setTimestamp.
    {
      std::lock_guard lock(request_mutex_);
      if (has_request_) {
        continue;
      }
    }
  }
}

std::optional<DecodedFrame> ImagePipelineSource::decodeAt(int64_t ts_ns) {
  // store_ non-null is a constructor precondition; downstream code unconditionally
  // dereferences it (entryTimestamps, at, indexAt). Keep the topic-name lookup
  // and the indexAt call consistent — both depend on the same precondition.
  const std::string topic_name = store_->descriptor(topic_).topic_name;
  auto index = store_->indexAt(topic_, ts_ns);
  if (!index.has_value()) {
    // Expected on scrub-before-data; silent.
    return std::nullopt;
  }

  {
    const auto timestamps = store_->entryTimestamps(topic_);
    if (*index >= timestamps.size()) {
      return std::nullopt;
    }
    if (timestamps[*index] == last_entry_ts_) {
      return std::nullopt;
    }
  }

  auto entry = store_->at(topic_, *index);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    warnOnce(
        warningKey(source_key_, "empty-resolved-entry"),
        "{} request_ts={} index={} empty-resolved-entry (viewer will show no frame)", source_key_, ts_ns, *index);
    return std::nullopt;
  }
  if (entry->timestamp == last_entry_ts_) {
    return std::nullopt;
  }
  last_entry_ts_ = entry->timestamp;

  if (parser_ != nullptr) {
    // entry->payload already is a PayloadView (bytes + anchor); pass it through
    // verbatim so the anchor's lifetime extends across the parser call.
    sdk::PayloadView payload = entry->payload;
    // MessageParser plugins aren't thread-safe (fastcdr et al. keep stateful
    // scratch). When two workers share one parser instance — same image topic
    // dropped twice, so SessionManager hands back its singleton — the shared
    // mutex prevents concurrent parseObject and scratch-buffer corruption. Held
    // for the parseObject call only; the following any_cast/decode work on the
    // returned BuiltinObject without touching the parser.
    auto invokeParser = [&] {
      if (parser_mutex_) {
        std::lock_guard<std::mutex> lock(*parser_mutex_);
        return parser_->parseObject(entry->timestamp, payload);
      }
      return parser_->parseObject(entry->timestamp, payload);
    };
    auto object_or = invokeParser();
    if (!object_or.has_value()) {
      warnOnce(warningKey(source_key_, "parseObject"), "{} parseObject failed: {}", source_key_, object_or.error());
      return std::nullopt;
    }
    const PJ::Timestamp effective_ts = object_or->ts.value_or(entry->timestamp);
    if (sdk::typeOf(object_or->object) != sdk::BuiltinObjectType::kImage) {
      warnOnce(
          warningKey(source_key_, "wrong-object-kind"), "{} parseObject returned wrong object_kind={}", source_key_,
          static_cast<int>(sdk::typeOf(object_or->object)));
      return std::nullopt;
    }
    const auto* img = std::any_cast<sdk::Image>(&object_or->object);
    if (img == nullptr) {
      warnOnce(
          warningKey(source_key_, "any-cast-image"), "{} any_cast<sdk::Image> failed (parser contract violation)",
          source_key_);
      return std::nullopt;
    }
    auto df = decodeCanonicalImage(*img, effective_ts, topic_name);
    if (df.has_value()) {
      df->frame_id = img->frame_id;  // carry the source frame so the viewer can find its CameraInfo.
      rectifyIfCalibrated(*df);
    }
    return df;
  }

  if (canonical_image_codec_) {
    // Each entry's bytes are a serialized sdk::Image (pj_base pj_image_v1 codec).
    // Deserialize per frame, then run the shared canonical-image decode path.
    auto img = deserializeImage(entry->payload.bytes.data(), entry->payload.bytes.size());
    if (!img.has_value()) {
      warnOnce(
          warningKey(source_key_, "canonical-deserialize"), "{} deserializeImage failed: {}", source_key_, img.error());
      return std::nullopt;
    }
    auto df = decodeCanonicalImage(*img, entry->timestamp, topic_name);
    if (df.has_value()) {
      df->frame_id = img->frame_id;
      rectifyIfCalibrated(*df);
    }
    return df;
  }

  if (pipeline_ == nullptr) {
    warnOnce(
        warningKey(source_key_, "no-parser-no-pipeline"),
        "{} no parser and no pipeline - viewer cannot decode this topic", source_key_);
    return std::nullopt;
  }

  auto result = pipeline_->decode(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!result.has_value()) {
    warnOnce(warningKey(source_key_, "pipeline-decode"), "{} pipeline decode failed: {}", source_key_, result.error());
    return std::nullopt;
  }
  result->pts = entry->timestamp;
  return std::move(*result);
}

void ImagePipelineSource::setCameraInfoMap(std::unordered_map<std::string, sdk::CameraInfo> by_frame_id) {
  // Must run on the main thread before the first setTimestamp(): the worker then
  // reads camera_info_by_frame_ unlocked, relying on that request's request_mutex_
  // barrier for visibility. A call after the first request would race the worker's
  // read (UB on the map), so enforce the contract instead of only documenting it.
  assert(!timestamp_requested_ && "setCameraInfoMap must be called before the first setTimestamp()");
  if (timestamp_requested_) {
    warnOnce(
        warningKey(source_key_, "camera-info-late"),
        "{} setCameraInfoMap called after the first setTimestamp(); ignored to avoid a worker race", source_key_);
    return;
  }
  camera_info_by_frame_ = std::move(by_frame_id);
}

const sdk::CameraInfo* ImagePipelineSource::cameraInfoFor(const std::string& frame_id) {
  if (frame_id.empty()) {
    return nullptr;
  }
  const auto it = camera_info_by_frame_.find(frame_id);
  return it != camera_info_by_frame_.end() ? &it->second : nullptr;
}

void ImagePipelineSource::rectifyIfCalibrated(DecodedFrame& df) {
  if (df.frame_id.empty() || df.width <= 0 || df.height <= 0) {
    return;
  }
  const sdk::CameraInfo* ci = cameraInfoFor(df.frame_id);
  if (ci == nullptr || !isRectifiable(*ci)) {
    return;  // no calibration for this frame -> leave the raw image as decoded.
  }
  // Build (cached per camera) a reverse map that outputs at the native calibration
  // resolution, so the rectified image, the annotation pixel coords, and the
  // overlay's frameSize all share one coordinate space (matches Foxglove). The map
  // bakes in the source resolution (sx/sy scale), so a stream that changes decoded
  // size mid-session must rebuild it — otherwise it would sample with stale scale.
  auto it = undistort_by_frame_.find(df.frame_id);
  if (it == undistort_by_frame_.end() || it->second.src_width != df.width || it->second.src_height != df.height) {
    UndistortMap built =
        computeUndistortMap(*ci, df.width, df.height, static_cast<int>(ci->width), static_cast<int>(ci->height));
    it = undistort_by_frame_.insert_or_assign(df.frame_id, std::move(built)).first;
  }
  if (!it->second.valid()) {
    return;
  }
  if (auto rect = rectifyFrame(df, it->second); rect.has_value()) {
    df = std::move(*rect);
  } else {
    // CameraInfo exists but the decoded pixel format isn't one the rectifier
    // resamples (planar/16-bit); the raw frame passes through unrectified.
    warnOnce(
        warningKey(source_key_, "rectify-unsupported-format"),
        "{} frame_id='{}' has calibration but pixel format {} is not rectifiable; showing raw frame", source_key_,
        df.frame_id, static_cast<int>(df.format));
  }
}

std::optional<DecodedFrame> ImagePipelineSource::decodeCanonicalImage(
    const sdk::Image& img, int64_t pts, std::string_view topic_name) {
  // Raw pixel layouts and Bayer mosaics (incl. grayscale-PNG-wrapped buffers)
  // reinterpret at the logical geometry; everything else is a self-describing
  // compressed container handled by the jpeg/png/auto cascade.
  if (rawEncodingInfo(img.encoding).has_value() || bayerPatternFor(img.encoding).has_value()) {
    return decodeRawOrBayerImage(img, pts, source_key_, auto_image_codec_, normalize_mono16_);
  }

  if (img.data.empty()) {
    warnOnce(
        warningKey(source_key_, "empty-canonical-image"), "{} canonical image has empty data encoding={}", source_key_,
        img.encoding);
    return std::nullopt;
  }
  DecodedFrame staged;
  staged.pixels = imageDataBytes(img, topic_name);
  staged.pts = pts;

  Expected<DecodedFrame> decoded = unexpected("unsupported image encoding: " + img.encoding);
  if (isJpegEncoding(img.encoding)) {
    decoded = jpeg_codec_.decode(staged);
  } else if (isPngEncoding(img.encoding)) {
    decoded = png_codec_.decode(staged);
  } else {
    decoded = auto_image_codec_.decode(staged);
  }
  if (!decoded.has_value()) {
    warnOnce(
        warningKey(source_key_, "compressed-decode"), "{} compressed decode failed encoding={}: {}", source_key_,
        img.encoding, decoded.error());
    return std::nullopt;
  }
  decoded->pts = pts;
  auto normalized = normalize_mono16_.decode(*decoded);
  if (!normalized.has_value()) {
    warnOnce(
        warningKey(source_key_, "compressed-normalization"), "{} compressed normalization failed: {}", source_key_,
        normalized.error());
    return std::nullopt;
  }
  normalized->pts = pts;
  return std::move(*normalized);
}

}  // namespace PJ
