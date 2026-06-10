// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/scene_pipeline_source.h"

#include <cstdio>
#include <utility>

namespace PJ {

ScenePipelineSource::ScenePipelineSource(
    ObjectStore* store, ObjectTopicId topic, std::unique_ptr<ISceneDecoder> decoder)
    : store_(store), topic_(topic), decoder_(std::move(decoder)) {}

void ScenePipelineSource::setTimestamp(int64_t ts_ns) {
  if (ts_ns == last_ts_) {
    return;  // same timestamp — reuse pending result, skip redundant decode
  }
  last_ts_ = ts_ns;

  // No annotation at this time: clear the overlays IF something is currently shown.
  // Coalesced via last_emitted_empty_ so an already-clear layer emits nothing.
  auto markCleared = [this] {
    pending_scene_.reset();
    pending_clear_ = !last_emitted_empty_;
  };

  // Copy bytes out of the store before decoding — keep no series-mutex held
  // through the decode (lock-discipline pattern documented in
  // ObjectStore::entryTimestamps).
  auto entry = store_->latestAt(topic_, ts_ns);
  if (!entry.has_value() || entry->payload.anchor == nullptr || entry->payload.bytes.empty()) {
    markCleared();
    return;
  }

  auto result = decoder_->decode(entry->payload.bytes.data(), entry->payload.bytes.size());
  if (result.has_value()) {
    pending_scene_ = std::move(*result);
    pending_clear_ = false;
  } else {
    fprintf(
        stderr, "[ScenePipelineSource] decode failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
        result.error().c_str());
    markCleared();
  }
}

std::optional<MediaFrame> ScenePipelineSource::takeFrame() {
  if (pending_scene_.has_value()) {
    MediaFrame mf;
    mf.overlays.push_back(std::move(*pending_scene_));
    pending_scene_.reset();
    last_emitted_empty_ = false;
    return mf;
  }
  if (pending_clear_) {
    // Empty-overlay frame: the compositor replaces this layer's overlays with an
    // empty set, clearing the previously-shown annotations.
    pending_clear_ = false;
    last_emitted_empty_ = true;
    return MediaFrame{};
  }
  return std::nullopt;
}

}  // namespace PJ
