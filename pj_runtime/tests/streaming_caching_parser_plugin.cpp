// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A minimal scalar MessageParser that CACHES a FieldHandle on its first parse
// and reuses it on every subsequent message via appendBoundRecord — the
// parser_protobuf pattern. This is precisely what exposes the parser path to
// the streaming pause/resume "stale cached handle" bug.
//
// Why a custom plugin is needed: the default MessageParserPluginBase dispatch
// (and the existing runtime_host_object test parser) writes scalars BY NAME
// every message. The by-name path re-resolves the field through a fresh
// WriteCore after each setDataEngineTarget swap and so self-heals — it would
// never reproduce the bug. Only a parser that holds a FieldHandle across the
// swap hits the stale-id failure.
//
// Payload contract: the first 4 bytes are a native-endian float32 written to a
// field named "value"; shorter payloads write 0.0. The bound type is
// "streaming/cached_scalar" (a scalar topic — BuiltinObjectType::kNone), so the
// runtime host routes pushMessage straight through parse() to the scalar write
// host.

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

class StreamingCachingParser : public PJ::MessageParserPluginBase {
 public:
  StreamingCachingParser() {
    // Register the bound type so classifySchema()/bind() accept it. The
    // handler's parse_scalars is never invoked (we override parse() below); it
    // exists only so the type classifies as a scalar topic.
    registerSchemaHandler(
        "streaming/cached_scalar",
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kNone,
            .parse_scalars = [](PJ::Timestamp, PJ::Span<const uint8_t>) -> PJ::Expected<PJ::sdk::ScalarRecord> {
              return PJ::sdk::ScalarRecord{};
            },
            .parse_object = {},
        });
  }

  // Resolve+cache the FieldHandle once, then write bound records — exactly the
  // pattern that breaks across a pause/resume DataEngine swap when the secondary
  // engine was never told about the field at a matching id.
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("streaming caching parser: write host not bound"));
    }
    if (!cached_field_.has_value()) {
      auto field_or = writeHost().ensureField("value", PJ::PrimitiveType::kFloat32);
      if (!field_or.has_value()) {
        return PJ::unexpected(field_or.error());
      }
      cached_field_ = *field_or;
    }
    float value = 0.0F;
    if (payload.size() >= sizeof(float)) {
      std::memcpy(&value, payload.data(), sizeof(float));
    }
    return writeHost().appendBoundRecord(
        timestamp_ns, {PJ::sdk::BoundFieldValue{.field = *cached_field_, .value = value}});
  }

 private:
  std::optional<PJ::sdk::FieldHandle> cached_field_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    StreamingCachingParser,
    R"({"id":"streaming-caching-parser","name":"Streaming Caching Parser","version":"1.0.0","encoding":["streaming_caching"]})")
