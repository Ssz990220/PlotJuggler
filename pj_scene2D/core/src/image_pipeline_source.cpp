#include "pj_scene2d_core/image_pipeline_source.h"

#include <fmt/format.h>

#include <algorithm>
#include <any>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/image.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

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
  } else if (img.row_step >= row_bytes && img.data.size() >= img.row_step * img.height) {
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

}  // namespace

ImagePipelineSource::ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser)
    : store_(store), topic_(topic), source_key_(sourceLabel(store, topic)), parser_(parser) {
  worker_ = std::thread(&ImagePipelineSource::workerLoop, this);
}

ImagePipelineSource::ImagePipelineSource(
    ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline)
    : store_(store), topic_(topic), source_key_(sourceLabel(store, topic)), pipeline_(std::move(pipeline)) {
  worker_ = std::thread(&ImagePipelineSource::workerLoop, this);
}

ImagePipelineSource::~ImagePipelineSource() {
  running_.store(false);
  request_cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void ImagePipelineSource::setTimestamp(int64_t ts_ns) {
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
    {
      std::unique_lock lock(request_mutex_);
      request_cv_.wait(lock, [this] { return has_request_ || !running_.load(); });
      if (!running_.load()) {
        break;
      }
      ts = requested_ts_;
      has_request_ = false;
    }

    auto result = decodeAt(ts);
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
    auto object_or = parser_->parseObject(entry->timestamp, entry->payload);
    if (!object_or.has_value()) {
      warnOnce(warningKey(source_key_, "parseObject"), "{} parseObject failed: {}", source_key_, object_or.error());
      return std::nullopt;
    }
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

    if (auto raw = rawEncodingInfo(img->encoding); raw.has_value()) {
      auto decoded = imageToDecodedFrame(*img, *raw, entry->timestamp);
      if (!decoded.has_value()) {
        warnOnce(
            warningKey(source_key_, "raw-conversion"), "{} raw image conversion failed encoding={} size={}x{}",
            source_key_, img->encoding, img->width, img->height);
        return std::nullopt;
      }
      auto normalized = normalize_mono16_.decode(*decoded);
      if (!normalized.has_value()) {
        warnOnce(
            warningKey(source_key_, "raw-normalization"), "{} raw normalization failed: {}", source_key_,
            normalized.error());
        return std::nullopt;
      }
      normalized->pts = entry->timestamp;
      return std::move(*normalized);
    }

    if (img->data.empty()) {
      warnOnce(
          warningKey(source_key_, "empty-canonical-image"), "{} canonical image has empty data encoding={}",
          source_key_, img->encoding);
      return std::nullopt;
    }
    DecodedFrame staged;
    staged.pixels = imageDataBytes(*img, topic_name);
    staged.pts = entry->timestamp;

    Expected<DecodedFrame> decoded = unexpected("unsupported image encoding: " + img->encoding);
    if (isJpegEncoding(img->encoding)) {
      decoded = jpeg_codec_.decode(staged);
    } else if (isPngEncoding(img->encoding)) {
      decoded = png_codec_.decode(staged);
    } else {
      decoded = auto_image_codec_.decode(staged);
    }
    if (!decoded.has_value()) {
      warnOnce(
          warningKey(source_key_, "compressed-decode"), "{} compressed decode failed encoding={}: {}", source_key_,
          img->encoding, decoded.error());
      return std::nullopt;
    }
    decoded->pts = entry->timestamp;
    auto normalized = normalize_mono16_.decode(*decoded);
    if (!normalized.has_value()) {
      warnOnce(
          warningKey(source_key_, "compressed-normalization"), "{} compressed normalization failed: {}", source_key_,
          normalized.error());
      return std::nullopt;
    }
    normalized->pts = entry->timestamp;
    return std::move(*normalized);
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

}  // namespace PJ
