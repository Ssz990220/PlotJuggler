// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/video_frame_nal_extractor.h"

#include <any>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace PJ {

StreamingVideoDecoder::NalExtractor makeVideoFrameNalExtractor(
    MessageParserPluginBase* parser, std::shared_ptr<std::mutex> parser_mutex) {
  // `vf->data` aliases bytes owned by the parsed VideoFrame's anchor (or the
  // wire buffer). The parsed ObjectRecord is destroyed when this closure
  // returns, so stash the most recent parse in a closure-owned slot (replaced on
  // the next call): the decoder consumes the span synchronously on the same
  // worker thread before the following extract call, so one slot suffices. Held
  // in a shared_ptr so the std::function stays copyable.
  auto keepalive = std::make_shared<std::optional<sdk::ObjectRecord>>();
  return [parser, parser_mutex = std::move(parser_mutex),
          keepalive](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    if (parser == nullptr) {
      return unexpected("no parser registered for video topic");
    }
    sdk::PayloadView payload = entry.payload;
    auto invokeParser = [&] {
      if (parser_mutex) {
        std::lock_guard<std::mutex> lock(*parser_mutex);
        return parser->parseObject(entry.timestamp, payload);
      }
      return parser->parseObject(entry.timestamp, payload);
    };
    auto object_or = invokeParser();
    if (!object_or.has_value()) {
      return unexpected("parseObject failed: " + object_or.error());
    }
    if (sdk::typeOf(object_or->object) != sdk::BuiltinObjectType::kVideoFrame) {
      return unexpected("parseObject returned wrong object_kind (expected kVideoFrame)");
    }
    const auto* vf = std::any_cast<sdk::VideoFrame>(&object_or->object);
    if (vf == nullptr) {
      return unexpected("any_cast<sdk::VideoFrame> failed (parser contract violation)");
    }
    Span<const uint8_t> data = vf->data;
    // Copy the codec id + real PTS before the parsed ObjectRecord moves into the
    // slot below. timestamp_ns is the true presentation timestamp (the store entry
    // is keyed by DTS), which the decoder needs to present B-frame video in order.
    std::string format = vf->format;
    const Timestamp pts = vf->timestamp_ns;
    *keepalive = std::move(object_or.value());
    return StreamingVideoDecoder::ExtractedFrame{data, std::move(format), pts};
  };
}

}  // namespace PJ
