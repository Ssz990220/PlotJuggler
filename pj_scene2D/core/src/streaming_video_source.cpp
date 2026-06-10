// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_source.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "pj_scene2d_core/entry_thumbnail_cache.h"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/video_frame_nal_extractor.h"

namespace PJ {

namespace {
// A scrub (vs contiguous playback) is a backward jump or a forward jump larger
// than this; only then do we publish an instant thumbnail preview.
constexpr int64_t kScrubPreviewThresholdNs = 500'000'000;  // 0.5 s
}  // namespace

StreamingVideoSource::StreamingVideoSource(ObjectStore* store, ObjectTopicId topic)
    : decoder_(std::make_unique<StreamingVideoDecoder>()) {
  decoder_->attach(store, topic);
  worker_ = std::thread(&StreamingVideoSource::workerLoop, this);
}

StreamingVideoSource::StreamingVideoSource(
    ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex,
    std::shared_ptr<void> parser_keepalive)
    : decoder_(std::make_unique<StreamingVideoDecoder>()), parser_keepalive_(std::move(parser_keepalive)) {
  // Each concurrently-running decoder needs its own extractor (the single-thread
  // keepalive slot must not be shared). The playback decoder gets one here.
  decoder_->attach(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));

  // Scrub-preview thumbnails: only for BOUNDED (file-backed) topics. A streaming
  // topic carries a retention budget and is tip-following while live; its
  // thumbnails would be built on pause instead (not yet wired). The cache's own
  // resolution gate further skips low-res sources where on-scrub decode is cheap.
  const int64_t retention_ns = (store != nullptr) ? store->retentionBudget(topic).time_window_ns : -1;
  if (store != nullptr && retention_ns == 0) {
    thumbnail_cache_ =
        std::make_unique<EntryThumbnailCache>(store, topic, makeVideoFrameNalExtractor(parser, parser_mutex));
    thumbnail_cache_->buildAsync();
  }

  worker_ = std::thread(&StreamingVideoSource::workerLoop, this);
}

StreamingVideoSource::~StreamingVideoSource() {
  // Flip running_ UNDER request_mutex_ (same lost-wakeup hazard as
  // ImagePipelineSource): an unlocked store can land between the worker's
  // predicate check and its block, so notify_one() wakes nobody and join() hangs.
  // Also cancel the in-flight token so a long (4K GOP) decode is preempted rather
  // than blocking teardown for a full decode.
  {
    std::lock_guard lock(request_mutex_);
    running_.store(false);
    if (cancel_token_) {
      cancel_token_->cancel();
    }
  }
  request_cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void StreamingVideoSource::setTimestamp(int64_t ts_ns) {
  if (ts_ns == last_requested_ts_) {
    return;
  }
  last_requested_ts_ = ts_ns;

  {
    std::lock_guard lock(request_mutex_);
    requested_ts_ = ts_ns;
    has_request_ = true;
    // Preempt the worker's in-flight decode so it abandons the stale target.
    if (cancel_token_) {
      cancel_token_->cancel();
    }
  }
  request_cv_.notify_one();
}

std::optional<MediaFrame> StreamingVideoSource::takeFrame() {
  std::lock_guard lock(result_mutex_);
  if (!result_frame_.has_value()) {
    return std::nullopt;
  }
  MediaFrame mf;
  mf.base = std::move(*result_frame_);
  result_frame_.reset();
  return mf;
}

void StreamingVideoSource::setFrameReadyCallback(std::function<void()> cb) {
  std::lock_guard lock(callback_mutex_);
  on_frame_ready_ = std::move(cb);
}

bool StreamingVideoSource::isInitialized() const {
  return decoder_->isInitialized();
}

void StreamingVideoSource::depositFrame(DecodedFrame frame) {
  {
    std::lock_guard lock(result_mutex_);
    result_frame_ = std::move(frame);
  }
  // Notify outside result_mutex_ so the callback's takeFrame() doesn't contend.
  // callback_mutex_ guards safe replacement while the worker runs.
  std::function<void()> cb_copy;
  {
    std::lock_guard lock(callback_mutex_);
    cb_copy = on_frame_ready_;
  }
  if (cb_copy) {
    cb_copy();
  }
}

void StreamingVideoSource::workerLoop() {
  // ts of the last successfully decoded full-res frame; lets us tell a scrub
  // (backward / large jump) from contiguous playback so previews fire only on
  // scrubs.
  int64_t last_decoded_ts = INT64_MIN;
  // Dedupes a persistent decode failure to a single log line (the same error
  // would otherwise repeat on every tick/scrub while the condition holds).
  std::string last_decode_error;
  while (running_.load()) {
    int64_t ts = 0;
    CancelTokenPtr token;
    {
      std::unique_lock lock(request_mutex_);
      request_cv_.wait(lock, [this] { return has_request_ || !running_.load(); });
      if (!running_.load()) {
        break;
      }
      ts = requested_ts_;
      has_request_ = false;
      // Fresh token for this decode; a later setTimestamp() cancels it to preempt.
      cancel_token_ = makeCancelToken();
      token = cancel_token_;
    }

    // Exception barrier (§R5 / ARCHITECTURE §10.5): a throwing parser/extractor or
    // a std::bad_alloc inside decode must not escape the std::thread callable (that
    // calls std::terminate). Catch at the boundary, log once, and keep serving.
    try {
      // Instant scrub preview: on a backward seek or a large forward jump, publish
      // the nearest thumbnail right away so the drag has feedback while the (slow,
      // 4K) GOP decode runs. The full-res frame replaces it below when ready.
      if (thumbnail_cache_) {
        const bool is_scrub =
            last_decoded_ts == INT64_MIN || ts < last_decoded_ts || (ts - last_decoded_ts) > kScrubPreviewThresholdNs;
        if (is_scrub) {
          if (auto preview = thumbnail_cache_->lookup(ts); preview.has_value()) {
            depositFrame(std::move(*preview));
          }
        }
      }

      auto result = decoder_->decodeAt(ts, token);
      if (result.has_value() && !result->isNull()) {
        last_decoded_ts = ts;
        last_decode_error.clear();  // recovered — let the next genuine failure log again
        depositFrame(std::move(*result));
      } else if (!result.has_value() && result.error() != "cancelled") {
        // Surface a genuine decode failure (unsupported codec, evicted keyframe,
        // parser/extractor error) instead of leaving a frozen frame with no clue.
        // "cancelled" is the benign latest-wins preemption and is skipped.
        if (result.error() != last_decode_error) {
          last_decode_error = result.error();
          fprintf(
              stderr, "[StreamingVideoSource] decode failed @ts=%lld: %s\n", static_cast<long long>(ts),
              result.error().c_str());
        }
      }
    } catch (const std::exception& ex) {
      if (last_decode_error != ex.what()) {
        last_decode_error = ex.what();
        fprintf(stderr, "[StreamingVideoSource] decode threw @ts=%lld: %s\n", static_cast<long long>(ts), ex.what());
      }
    } catch (...) {
      fprintf(stderr, "[StreamingVideoSource] decode threw a non-std exception @ts=%lld\n", static_cast<long long>(ts));
    }

    // Check for a newer request that arrived during decode
    {
      std::lock_guard lock(request_mutex_);
      if (has_request_) {
        continue;  // immediately process the newer request
      }
    }
  }
}

}  // namespace PJ
