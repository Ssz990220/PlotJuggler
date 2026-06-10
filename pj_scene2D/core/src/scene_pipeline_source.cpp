// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/scene_pipeline_source.h"

#include <cstdio>
#include <utility>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace PJ {

ScenePipelineSource::ScenePipelineSource(
    ObjectStore* store, ObjectTopicId topic, std::unique_ptr<ISceneDecoder> decoder)
    : store_(store), topic_(topic), decoder_(std::move(decoder)) {}

ScenePipelineSource::ScenePipelineSource(
    ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex,
    std::unique_ptr<ISceneDecoder> decoder)
    : store_(store),
      topic_(topic),
      decoder_(std::move(decoder)),
      parser_(parser),
      parser_mutex_(std::move(parser_mutex)) {}

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

  auto apply = [&](Expected<SceneFrame> result) {
    if (result.has_value()) {
      pending_scene_ = std::move(*result);
    } else {
      // Covers both decode failures and a parser contract violation (the decoder's
      // decode(object) returns an error when the BuiltinObject isn't its type).
      fprintf(
          stderr, "[ScenePipelineSource] decode failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
          result.error().c_str());
      pending_scene_.reset();
    }
  };

  if (parser_ != nullptr) {
    // Parser-backed topic: the store holds the RAW source message. Run the parser
    // to get the canonical object and decode that object directly — no serialize/
    // deserialize round-trip (the 3D consumer decodes the object the same way).
    // Hold the shared parser mutex for the parseObject call only (mirrors
    // ImagePipelineSource::decodeAt): MessageParser plugins keep stateful scratch
    // and aren't thread-safe across consumers of the same singleton.
    auto invokeParser = [&] {
      if (parser_mutex_) {
        std::lock_guard<std::mutex> lock(*parser_mutex_);
        return parser_->parseObject(entry->timestamp, entry->payload);
      }
      return parser_->parseObject(entry->timestamp, entry->payload);
    };
    auto record = invokeParser();
    if (!record.has_value()) {
      fprintf(
          stderr, "[ScenePipelineSource] parseObject failed at ts=%lld: %s\n", static_cast<long long>(ts_ns),
          record.error().c_str());
      pending_scene_.reset();
      return;
    }
    apply(decoder_->decode(record->object));
  } else {
    // Canonical-producer topic: the store holds canonical bytes; decode as-is.
    apply(decoder_->decode(entry->payload.bytes.data(), entry->payload.bytes.size()));
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

void ScenePipelineSource::invalidate() {
  // Drop the timestamp dedup so the next setTimestamp re-decodes even at an
  // unchanged time (composite rebuild re-seeds at the current tracker time).
  last_ts_ = INT64_MIN;
  pending_scene_.reset();
}

}  // namespace PJ
