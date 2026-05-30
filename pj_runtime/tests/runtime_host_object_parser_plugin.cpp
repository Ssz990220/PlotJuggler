#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

PJ::sdk::SchemaHandler makeByteCountHandler(PJ::sdk::BuiltinObjectType object_type) {
  return PJ::sdk::SchemaHandler{
      .object_type = object_type,
      .parse_scalars = [](PJ::Timestamp /*timestamp_ns*/,
                          PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        return PJ::sdk::ScalarRecord{
            .ts = std::nullopt,
            .fields = {{.name = "byte_count", .value = static_cast<uint64_t>(payload.size())}},
        };
      },
      .parse_object = {},
  };
}

class RuntimeHostObjectParser : public PJ::MessageParserPluginBase {
 public:
  RuntimeHostObjectParser() {
    registerSchemaHandler("mock/image", makeByteCountHandler(PJ::sdk::BuiltinObjectType::kImage));
    registerSchemaHandler("mock/scalar", makeByteCountHandler(PJ::sdk::BuiltinObjectType::kNone));
  }

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
      return status;
    }
    if (type_name == "mock/bind_schema_image") {
      registerSchemaHandler(type_name, makeByteCountHandler(PJ::sdk::BuiltinObjectType::kImage));
    }
    return PJ::okStatus();
  }
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    RuntimeHostObjectParser,
    R"({"id":"runtime-host-object-parser","name":"Runtime Host Object Parser","version":"1.0.0","encoding":["runtime_host_object"]})")
