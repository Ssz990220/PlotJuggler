// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <fmt/format.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "pj_scene2d_core/ffmpeg_decoder.h"
#include "pj_scene2d_core/video_codec_utils.h"

namespace PJ {

namespace {

// Identity extractor: the entry's bytes already are the raw Annex-B NAL stream
// for one frame (the round-trip / simulated-stream path), which is H.264.
// Aliases the entry's own buffer — no copy.
Expected<StreamingVideoDecoder::ExtractedFrame> identityNalExtractor(const ResolvedObjectEntry& entry) {
  // No wrapping message to read a PTS from, so presentation == decode order here
  // (this path is the raw-bytes/round-trip case, which has no B-frame reorder).
  return StreamingVideoDecoder::ExtractedFrame{entry.payload.bytes, "h264", entry.timestamp};
}

}  // namespace

StreamingVideoDecoder::StreamingVideoDecoder() : nal_extractor_(identityNalExtractor) {}

StreamingVideoDecoder::~StreamingVideoDecoder() = default;

void StreamingVideoDecoder::attach(ObjectStore* store, ObjectTopicId topic) {
  attach(store, topic, NalExtractor{});
}

void StreamingVideoDecoder::attach(ObjectStore* store, ObjectTopicId topic, NalExtractor extractor) {
  reset();
  store_ = store;
  topic_ = topic;
  nal_extractor_ = extractor ? std::move(extractor) : NalExtractor(identityNalExtractor);
}

Expected<DecodedFrame> StreamingVideoDecoder::decodeAt(Timestamp ts, const CancelTokenPtr& cancel) {
  if (store_ == nullptr) {
    return unexpected("not attached");
  }

  updateKeyframeIndex();
  if (codec_id_ == AV_CODEC_ID_NONE && !codec_format_.empty()) {
    return unexpected("unsupported video codec: '" + codec_format_ + "'");
  }

  // Resolve the frame to PRESENT at `ts`: the one with the greatest PTS <= ts.
  // The ObjectStore is keyed by DTS (decode order); presenting by PTS is what makes
  // B-frame video play in order (with no B-frames PTS == DTS, so this is identity).
  auto target = presentationTargetAt(ts);
  if (!target.has_value()) {
    return unexpected("no entry at timestamp");
  }
  const Timestamp target_ts = target->first;    // PTS — the presentation key we serve by
  const Timestamp target_dts = target->second;  // DTS — the decode-order store key
  auto target_idx_opt = store_->indexAt(topic_, target_dts);
  if (!target_idx_opt.has_value()) {
    return unexpected("entry not available");
  }
  const size_t target_idx = *target_idx_opt;

  // Same frame re-requested (display polling faster than the clock / push rate).
  if (initialized_ && last_served_ts_ == target_ts && !last_frame_.isNull()) {
    return last_frame_;
  }

  // Forward request whose frame was already decoded during an earlier look-ahead:
  // serve straight from the reorder buffer, no codec work.
  if (initialized_ && last_served_ts_.has_value() && target_ts >= *last_served_ts_) {
    if (auto served = takeReadyFrame(target_ts); served.has_value()) {
      return *served;
    }
  }

  // Continue forward when the codec is live (not drained) and positioned
  // at-or-before the target; otherwise seek to the nearest preceding keyframe.
  // last_served_ts_ is in PTS space, so during forward playback (PTS monotonic)
  // this stays a cheap continuation even with B-frames.
  const bool can_forward = initialized_ && !ended_ && last_sent_ts_.has_value() && last_served_ts_.has_value() &&
                           target_ts >= *last_served_ts_;
  if (can_forward) {
    // Resume after the last packet fed. last_sent_ts_ is the DTS feed cursor;
    // re-resolve by timestamp (not a cached index) so front-eviction in live mode
    // cannot leave a stale cursor.
    auto fed_idx = store_->indexAt(topic_, *last_sent_ts_);
    const size_t next_idx = fed_idx.has_value() ? *fed_idx + 1 : 0;
    return serveForward(next_idx, target_idx, target_ts, cancel);
  }

  // --- Seek path: backward seek, first decode, or post-drain ---
  // Seeking is a decode-order operation: find the keyframe before the target's DTS.
  if (keyframe_timestamps_.empty()) {
    return unexpected("no keyframe yet");
  }
  auto kf_ts = findKeyframeBefore(target_dts);
  if (!kf_ts.has_value()) {
    return unexpected("no keyframe before target");
  }
  auto kf_idx_opt = store_->indexAt(topic_, *kf_ts);
  if (!kf_idx_opt.has_value()) {
    return unexpected("keyframe evicted — target undecodable");
  }
  size_t kf_idx = *kf_idx_opt;

  if (!initialized_) {
    auto kf_entry = store_->at(topic_, kf_idx);
    if (!kf_entry.has_value()) {
      return unexpected("keyframe entry not available");
    }
    // Open the decoder from the keyframe entry's codec (VideoFrame.format). The
    // bitstream parameter sets ride in-band on the keyframe, so init needs only
    // the codec id; the bytes are decoded below by serveForward.
    auto nal = nal_extractor_(*kf_entry);
    if (!nal.has_value()) {
      return unexpected("keyframe extraction failed: " + nal.error());
    }
    if (!initDecoder(nal->format, nal->bytes)) {
      return unexpected(
          "failed to open decoder for codec: '" + (nal->format.empty() ? std::string("h264") : nal->format) + "'");
    }
  }

  decoder_->flush();
  ready_frames_.clear();
  last_served_ts_.reset();
  last_sent_ts_.reset();
  ended_ = false;

  // Cheaply skip-decode the GOP frames strictly before the target — they are never
  // displayed for this request, and decodeSkip avoids materialising them.
  // serveForward then materialises from the target onward (paying only the bounded
  // reorder-depth of extra frames needed to flush the target out).
  for (size_t i = kf_idx; i < target_idx; ++i) {
    if (cancel != nullptr && cancel->isCancelled()) {
      return unexpected("cancelled");
    }
    auto entry = store_->at(topic_, i);
    if (!entry.has_value()) {
      return unexpected(fmt::format("entry evicted mid-GOP (index {})", i));
    }
    last_sent_ts_ = entry->timestamp;
    auto nal = nal_extractor_(*entry);
    if (!nal.has_value()) {
      return unexpected(fmt::format("frame extraction failed (index {}): {}", i, nal.error()));
    }
    decoder_->decodeSkip(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp);
  }

  return serveForward(target_idx, target_idx, target_ts, cancel);
}

std::optional<DecodedFrame> StreamingVideoDecoder::takeReadyFrame(Timestamp ts) {
  auto it = ready_frames_.find(ts);
  if (it == ready_frames_.end()) {
    return std::nullopt;
  }
  DecodedFrame frame = std::move(it->second);
  // Forward playback never revisits an earlier frame without a seek, so drop every
  // buffered frame at or before the one we are serving (keeps the buffer bounded).
  ready_frames_.erase(ready_frames_.begin(), std::next(it));
  last_served_ts_ = ts;
  last_frame_ = frame;
  return frame;
}

Expected<DecodedFrame> StreamingVideoDecoder::serveForward(
    size_t start_idx, size_t target_idx, Timestamp target_ts, const CancelTokenPtr& cancel) {
  const size_t count = store_->entryCount(topic_);
  // target_ts is the PTS we serve by; target_idx is the target's decode-order index
  // (passed in — it cannot be re-derived from a PTS against the DTS-keyed store).

  // A cancel may fire after the target packet has already been fed (its frame still
  // buffered, un-received). Leaving last_sent_ts_ past the target would make the
  // next forward request skip it ("forward decode produced no frame" for a valid
  // timestamp). Reset the forward-continuation state so the next decodeAt re-seeks
  // (which flushes the abandoned pipeline) and decodes the target cleanly.
  auto on_cancelled = [this]() -> Expected<DecodedFrame> {
    last_sent_ts_.reset();
    ready_frames_.clear();
    return unexpected("cancelled");
  };

  // Receive every queued output BEFORE each send (materialising only frames at
  // or after the target — earlier ones are the reorder-depth prefix, already
  // past). Pumping to EAGAIN first guarantees the send below cannot itself hit
  // EAGAIN, whose legacy recovery (sendPacket) silently DISCARDS a queued frame:
  // under load (big frames keeping the codec's worker threads busy) that frame
  // is the next request's display frame, lost forever — a one-frame "produced
  // no frame" hiccup early in playback.
  const auto want = [target_ts](int64_t frame_pts) { return frame_pts >= target_ts; };
  auto pump = [&]() -> std::optional<DecodedFrame> {
    while (true) {
      auto frame = decoder_->receiveFiltered(want);
      if (!frame.has_value()) {
        return std::nullopt;  // EAGAIN (queue empty) or transient: feed more
      }
      if (frame->isNull()) {
        continue;  // pre-target frame, cheaply skipped
      }
      ready_frames_.insert_or_assign(frame->pts, std::move(*frame));
      if (auto served = takeReadyFrame(target_ts); served.has_value()) {
        return served;
      }
    }
  };

  for (size_t i = start_idx; i < count; ++i) {
    // Preempt a long forward (GOP) decode when a newer scrub target cancelled this
    // token. Checked before each feed so last_sent_ts_ reflects only fed packets.
    if (cancel != nullptr && cancel->isCancelled()) {
      return on_cancelled();
    }
    if (auto served = pump(); served.has_value()) {
      return *served;
    }
    auto entry = store_->at(topic_, i);
    if (!entry.has_value()) {
      // A hole strictly before the target breaks the decode chain; past the target
      // we simply stop feeding and drain whatever the buffer already holds.
      if (i <= target_idx) {
        return unexpected(fmt::format("entry evicted mid-GOP (index {})", i));
      }
      break;
    }
    last_sent_ts_ = entry->timestamp;
    // `entry` outlives the send call, so the extractor's aliased Span (into the
    // entry bytes or an anchor) stays valid while FFmpeg copies it.
    auto nal = nal_extractor_(*entry);
    if (!nal.has_value()) {
      return unexpected(fmt::format("frame extraction failed (index {}): {}", i, nal.error()));
    }
    decoder_->sendOnly(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp);
  }

  // Final pump: the target may have surfaced off the last sends.
  if (auto served = pump(); served.has_value()) {
    return *served;
  }

  // Reached the tip without the target surfacing — drain the reorder/threading
  // buffer (NULL packet) to flush the final frame(s). Draining ends the codec
  // stream, so the next decodeAt must take the (flushing) seek path.
  for (auto& f : decoder_->drain()) {
    if (!f.isNull() && f.pts >= target_ts) {
      ready_frames_.insert_or_assign(f.pts, std::move(f));
    }
  }
  ended_ = true;
  if (auto served = takeReadyFrame(target_ts); served.has_value()) {
    return *served;
  }
  return unexpected("forward decode produced no frame");
}

std::vector<Timestamp> StreamingVideoDecoder::keyframeTimestamps() {
  updateKeyframeIndex();
  return keyframe_timestamps_;
}

bool StreamingVideoDecoder::sampledUsesKeyframeOnly(Timestamp interval_ns) {
  if (store_ == nullptr || interval_ns <= 0) {
    return false;
  }
  updateKeyframeIndex();
  // Need at least two keyframes to reason about spacing; a single keyframe over a
  // long span cannot cover a 1/interval cadence, so the forward pass wins.
  if (keyframe_timestamps_.size() < 2) {
    return false;
  }
  // Keyframe-only is correct ONLY when every interval-wide window already holds a
  // keyframe — then it yields the SAME cadence as the forward pass for a fraction
  // of the work (each keyframe decodes in isolation). Guarantee that by requiring
  // the leading gap, every consecutive gap, and the trailing gap to be <= interval.
  const auto range = store_->timeRange(topic_);
  if (keyframe_timestamps_.front() - range.first > interval_ns) {
    return false;
  }
  if (range.second - keyframe_timestamps_.back() > interval_ns) {
    return false;
  }
  for (size_t i = 1; i < keyframe_timestamps_.size(); ++i) {
    if (keyframe_timestamps_[i] - keyframe_timestamps_[i - 1] > interval_ns) {
      return false;
    }
  }
  return true;
}

void StreamingVideoDecoder::decodeSampled(Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  if (store_ == nullptr || interval_ns <= 0 || store_->entryCount(topic_) == 0) {
    return;
  }
  // Adaptive dispatch: when the source already carries keyframes at least as dense
  // as the requested cadence, decode only ~1 keyframe per interval (same cadence,
  // near-instant). Otherwise fall back to the forward pass.
  if (sampledUsesKeyframeOnly(interval_ns)) {
    decodeSampledKeyframes(interval_ns, sink);
  } else {
    decodeSampledForward(interval_ns, sink);
  }
}

void StreamingVideoDecoder::decodeSampledKeyframes(
    Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  // Precondition (sampledUsesKeyframeOnly): keyframe_timestamps_ is fresh and
  // covers the cadence. Decode ~1 keyframe per interval; each is self-contained,
  // so we flush and decode a single packet per pick — no GOP walk.
  std::optional<Timestamp> next_sample;
  for (Timestamp kf_ts : keyframe_timestamps_) {
    if (next_sample.has_value() && kf_ts < *next_sample) {
      continue;  // this interval bucket already has a sample
    }
    auto idx = store_->indexAt(topic_, kf_ts);
    if (!idx.has_value()) {
      continue;
    }
    auto entry = store_->at(topic_, *idx);
    if (!entry.has_value()) {
      continue;
    }
    auto nal = nal_extractor_(*entry);
    if (!nal.has_value()) {
      continue;
    }
    if (!initialized_ && !initDecoder(nal->format, nal->bytes)) {
      return;
    }
    // Flush so the keyframe decodes in isolation, then drain if frame-threading /
    // reorder latency held it back (a lone IDR often surfaces only on drain).
    decoder_->flush();
    DecodedFrame frame;
    auto decoded = decoder_->decode(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp, nullptr);
    if (decoded.has_value() && !decoded->isNull()) {
      frame = std::move(*decoded);
    } else {
      auto drained = decoder_->drain();
      if (!drained.empty()) {
        frame = std::move(drained.back());
      }
    }
    if (frame.isNull() || frame.format != PixelFormat::kYUV420P) {
      continue;
    }
    if (!sink(frame)) {
      return;  // resolution gate / budget reached
    }
    next_sample = kf_ts + interval_ns;
  }
}

void StreamingVideoDecoder::decodeSampledForward(
    Timestamp interval_ns, const std::function<bool(const DecodedFrame&)>& sink) {
  const size_t count = store_->entryCount(topic_);

  std::optional<Timestamp> next_sample;
  // Pure predicate: should THIS decoded frame (by true output PTS) be surfaced?
  // True for the first frame and whenever PTS crosses the next sample boundary.
  // `next_sample` advances only after a frame is actually sunk (below), so the
  // decoder pays the download+convert cost on exactly the sampled frames.
  auto want = [&](int64_t frame_pts) -> bool { return !next_sample.has_value() || frame_pts >= *next_sample; };

  // One forward pass over every entry. Every packet advances the codec, but
  // decodeFiltered materializes pixels only for frames `want` accepts — the rest
  // come back null and cost just send+receive.
  for (size_t i = 0; i < count; ++i) {
    auto entry = store_->at(topic_, i);
    if (!entry.has_value()) {
      break;  // gap — stop the pass
    }
    auto nal = nal_extractor_(*entry);
    if (!nal.has_value()) {
      continue;
    }
    if (!initialized_ && !initDecoder(nal->format, nal->bytes)) {
      return;
    }
    auto frame = decoder_->decodeFiltered(nal->bytes.data(), nal->bytes.size(), nal->pts, entry->timestamp, want);
    if (!frame.has_value()) {
      continue;  // EAGAIN / transient error — keep feeding packets
    }
    if (frame->isNull() || frame->format != PixelFormat::kYUV420P) {
      continue;  // decoded but not a sampled frame
    }
    if (!sink(*frame)) {
      return;  // budget reached
    }
    next_sample = frame->pts + interval_ns;  // advance only on a real sample
  }

  // Tail: drain materializes the reorder/threading buffer (a handful of frames),
  // so apply the same sampling gate here before sinking.
  for (auto& frame : decoder_->drain()) {
    if (frame.isNull() || frame.format != PixelFormat::kYUV420P) {
      continue;
    }
    if (!want(frame.pts)) {
      continue;
    }
    if (!sink(frame)) {
      return;
    }
    next_sample = frame.pts + interval_ns;
  }
}

void StreamingVideoDecoder::reset() {
  decoder_.reset();
  initialized_ = false;
  keyframe_timestamps_.clear();
  pts_to_dts_.clear();
  pruned_floor_.reset();
  last_scanned_ts_.reset();
  last_sent_ts_.reset();
  ready_frames_.clear();
  last_served_ts_.reset();
  ended_ = false;
  last_frame_ = {};
  codec_id_ = 0;
  codec_format_.clear();
}

bool StreamingVideoDecoder::isInitialized() const {
  return initialized_;
}

void StreamingVideoDecoder::updateKeyframeIndex() {
  if (store_ == nullptr) {
    return;
  }

  size_t count = store_->entryCount(topic_);
  if (count == 0) {
    return;
  }

  // Prune keyframe + presentation entries that have been evicted. Only runs when
  // the store's front (range.first, DTS) has actually advanced — so the O(n)
  // pts_to_dts_ scan stays off the per-frame path for file-backed sources (no
  // eviction), where decodeAt() calls this every frame.
  const Timestamp evict_floor = store_->timeRange(topic_).first;
  if (!pruned_floor_.has_value() || evict_floor > *pruned_floor_) {
    pruned_floor_ = evict_floor;
    auto kf_it = std::lower_bound(keyframe_timestamps_.begin(), keyframe_timestamps_.end(), evict_floor);
    keyframe_timestamps_.erase(keyframe_timestamps_.begin(), kf_it);
    // pts_to_dts_ is keyed by PTS, so prune by the evicted DTS value, not the key.
    for (auto it = pts_to_dts_.begin(); it != pts_to_dts_.end();) {
      if (it->second < evict_floor) {
        it = pts_to_dts_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Scan entries we haven't seen yet. Track by timestamp (not count)
  // because retention can keep count constant while new entries replace old.
  size_t start = 0;
  if (last_scanned_ts_.has_value()) {
    auto idx_opt = store_->indexAt(topic_, *last_scanned_ts_);
    if (idx_opt.has_value()) {
      start = *idx_opt + 1;
    }
  }

  for (size_t i = start; i < count; ++i) {
    auto entry = store_->at(topic_, i);
    if (!entry.has_value()) {
      continue;
    }
    // The keyframe scan inspects the raw NAL/OBU bytes just like the decode path,
    // so it must see the extracted bitstream (not the wrapping message). `entry`
    // is alive for the duration of the inspection. The first extracted frame also
    // latches the topic's codec (from VideoFrame.format), which drives the
    // codec-dispatched keyframe oracle.
    auto nal = nal_extractor_(*entry);
    if (nal.has_value()) {
      resolveCodec(nal->format);
      if (isVideoKeyframe(codec_id_, nal->bytes.data(), nal->bytes.size())) {
        keyframe_timestamps_.push_back(entry->timestamp);
      }
      // Record the frame's real PTS -> its DTS store key for presentation lookup.
      pts_to_dts_[nal->pts] = entry->timestamp;
    }
    last_scanned_ts_ = entry->timestamp;
  }
}

std::optional<Timestamp> StreamingVideoDecoder::findKeyframeBefore(Timestamp ts) const {
  if (keyframe_timestamps_.empty()) {
    return std::nullopt;
  }
  auto it = std::upper_bound(keyframe_timestamps_.begin(), keyframe_timestamps_.end(), ts);
  if (it == keyframe_timestamps_.begin()) {
    return std::nullopt;
  }
  --it;
  return *it;
}

std::optional<std::pair<Timestamp, Timestamp>> StreamingVideoDecoder::presentationTargetAt(Timestamp ts) const {
  if (pts_to_dts_.empty()) {
    return std::nullopt;
  }
  // Greatest PTS <= ts: the frame on screen at presentation time `ts`.
  auto it = pts_to_dts_.upper_bound(ts);
  if (it == pts_to_dts_.begin()) {
    // No frame presents at-or-before `ts`. With B-frames the smallest PTS sits a
    // reorder-depth AFTER the stream's first DTS (the IDR's negative encoder-delay
    // DTS), so a request at the very start of the DTS-based timeline falls into
    // that gap — clamp to the first presentable frame (it stays at begin()). Only a
    // request genuinely before the stream's start returns nothing.
    if (store_ == nullptr || ts < store_->timeRange(topic_).first) {
      return std::nullopt;
    }
  } else {
    --it;
  }
  return std::make_pair(it->first, it->second);
}

void StreamingVideoDecoder::resolveCodec(std::string_view format) {
  if (codec_id_ != AV_CODEC_ID_NONE) {
    return;  // already latched
  }
  // An empty format (the raw-bytes / identity path, or a producer that omitted
  // it) defaults to H.264 — the legacy assumption of this decoder.
  codec_format_ = format.empty() ? std::string("h264") : std::string(format);
  codec_id_ = videoCodecIdFromFormat(codec_format_);
}

int StreamingVideoDecoder::computeReorderDelay() const {
  // The decoder's required reorder depth (FFmpeg `has_b_frames`/`video_delay`):
  // the k-th frame in presentation order can only be emitted after its packet —
  // at decode-order index i_k — has been fed, so the output lags the input by
  // max_k(i_k - k). Computed from the presentation index (pts -> entry key) the
  // keyframe scan already built; a stream without B-frames yields 0. This mirrors
  // what a container demuxer reports in AVCodecParameters::video_delay.
  int delay = 0;
  int presentation_rank = 0;
  for (const auto& [pts, entry_ts] : pts_to_dts_) {
    auto idx = store_->indexAt(topic_, entry_ts);
    if (idx.has_value()) {
      delay = std::max(delay, static_cast<int>(*idx) - presentation_rank);
    }
    ++presentation_rank;
  }
  // H.264/HEVC cap the DPB reorder depth at 16; clamp so a pathological index
  // (e.g. mid-eviction skew) cannot impose an absurd output delay.
  return std::min(delay, 16);
}

bool StreamingVideoDecoder::initDecoder(std::string_view format, Span<const uint8_t> keyframe_bytes) {
  resolveCodec(format);
  AVCodecParameters* params = makeVideoCodecParams(codec_id_);
  if (params == nullptr) {
    return false;  // unknown/unbuilt codec
  }
  // Prime extradata from the keyframe's in-band parameter sets so the decoder
  // opens configured like a demuxer-fed one (dimensions/profile known up front).
  primeKeyframeParamSets(params, keyframe_bytes.data(), keyframe_bytes.size());
  // CRITICAL for B-frame streams: tell the decoder its output must lag its input
  // by the stream's reorder depth. Opened with video_delay = 0, libavcodec's
  // H.264 decoder can start emitting too early and then silently DISCARD the
  // stream's first B-frame(s) as "already in the past" (observed on
  // screen-recorder files: one early frame vanishes, so the presentation-ordered
  // serve fails with "forward decode produced no frame" at that timestamp).
  // In-band SPS parsing does NOT reliably correct this (no VUI bitstream
  // restriction info); the container-derived value is authoritative, and the
  // presentation index reproduces it exactly.
  params->video_delay = computeReorderDelay();

  decoder_ = std::make_unique<FfmpegDecoder>();
  bool ok = decoder_->open(params);
  avcodec_parameters_free(&params);

  if (!ok) {
    decoder_.reset();
    return false;
  }

  initialized_ = true;
  return true;
}

}  // namespace PJ
