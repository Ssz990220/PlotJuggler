#include "pj_runtime/DataSourceRuntimeHost.h"

#include <QLoggingCategory>
#include <QString>
#include <set>
#include <utility>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcIngest, "pj.runtime.ingest")
}  // namespace

// ---------------------------------------------------------------------------
// ParserBinding — out-of-line definitions so the header only needs forward
// declarations of MessageParserHandle / DatastoreParserWriteHost /
// ServiceRegistryBuilder.
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::ParserBinding::ParserBinding() = default;

DataSourceRuntimeHost::ParserBinding::ParserBinding(
    std::unique_ptr<ServiceRegistryBuilder> b, std::unique_ptr<DatastoreParserWriteHost> w,
    std::unique_ptr<MessageParserHandle> p)
    : registry_builder(std::move(b)), write_host(std::move(w)), parser(std::move(p)) {}

DataSourceRuntimeHost::ParserBinding::~ParserBinding() = default;

DataSourceRuntimeHost::ParserBinding::ParserBinding(ParserBinding&&) noexcept = default;

DataSourceRuntimeHost::ParserBinding& DataSourceRuntimeHost::ParserBinding::operator=(ParserBinding&&) noexcept =
    default;

// ---------------------------------------------------------------------------
// Vtable — single static instance shared by every session.
// ---------------------------------------------------------------------------

const PJ_data_source_runtime_host_vtable_t DataSourceRuntimeHost::kVtable = {
    .protocol_version = 1,
    .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
    .report_message = &DataSourceRuntimeHost::cbReportMessage,
    .progress_start = &DataSourceRuntimeHost::cbProgressStart,
    .progress_update = &DataSourceRuntimeHost::cbProgressUpdate,
    .progress_finish = &DataSourceRuntimeHost::cbProgressFinish,
    .is_stop_requested = &DataSourceRuntimeHost::cbIsStopRequested,
    .notify_state = &DataSourceRuntimeHost::cbNotifyState,
    .request_stop = &DataSourceRuntimeHost::cbRequestStop,
    .ensure_parser_binding = &DataSourceRuntimeHost::cbEnsureParserBinding,
    .push_raw_message = &DataSourceRuntimeHost::cbPushRawMessage,
    .show_message_box = &DataSourceRuntimeHost::cbShowMessageBox,
    .list_available_encodings = &DataSourceRuntimeHost::cbListAvailableEncodings,
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::DataSourceRuntimeHost(
    DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle)
    : engine_(engine), catalog_(catalog), dataset_id_(dataset_id), source_write_host_(engine, source_handle) {}

DataSourceRuntimeHost::~DataSourceRuntimeHost() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DataSourceRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  registry.registerService<sdk::SourceWriteHostService>(source_write_host_.raw());
  registry.registerService<sdk::DataSourceRuntimeHostService>(PJ_data_source_runtime_host_t{
      .ctx = this,
      .vtable = &kVtable,
  });
}

void DataSourceRuntimeHost::flushAll() {
  if (flushed_) {
    return;
  }
  flushed_ = true;
  source_write_host_.flushPending();
  // Each parser binding owns its own DatastoreParserWriteHost / DataWriter
  // pair — their open chunks are independent from the source's. Without this
  // their pending rows never reach the reader, so the catalog sees the
  // column descriptors but bounds()/samples() return nothing and curves
  // drop in with empty plots.
  for (auto& [binding_id, binding] : parser_bindings_) {
    if (binding.write_host != nullptr) {
      binding.write_host->flushPending();
    }
  }
}

void DataSourceRuntimeHost::requestStop(std::string_view reason) {
  last_error_.assign(reason.data(), reason.size());
  stop_requested_.store(true);
}

bool DataSourceRuntimeHost::fail(PJ_error_t* out_error, const char* message) noexcept {
  last_error_ = message;
  sdk::fillError(out_error, 1, "pj.runtime.ingest", last_error_);
  return false;
}

// ---------------------------------------------------------------------------
// C-ABI callbacks
// ---------------------------------------------------------------------------

void DataSourceRuntimeHost::cbReportMessage(
    void* /*ctx*/, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
  const std::string_view text(message.data, message.size);
  switch (level) {
    case PJ_DATA_SOURCE_MESSAGE_ERROR:
      qCWarning(lcIngest) << "[plugin error]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    case PJ_DATA_SOURCE_MESSAGE_WARNING:
      qCWarning(lcIngest) << "[plugin warn]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    default:
      qCInfo(lcIngest) << "[plugin]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
  }
}

bool DataSourceRuntimeHost::cbProgressStart(
    void* /*ctx*/, PJ_string_view_t /*label*/, uint64_t /*total*/, bool /*cancellable*/,
    PJ_error_t* /*out_error*/) noexcept {
  return true;  // Import progress is currently logged by plugin messages only.
}

bool DataSourceRuntimeHost::cbProgressUpdate(void* ctx, uint64_t /*current*/) noexcept {
  return !static_cast<DataSourceRuntimeHost*>(ctx)->stop_requested_.load();
}

void DataSourceRuntimeHost::cbProgressFinish(void* /*ctx*/) noexcept {}

bool DataSourceRuntimeHost::cbIsStopRequested(void* ctx) noexcept {
  return static_cast<DataSourceRuntimeHost*>(ctx)->stop_requested_.load();
}

void DataSourceRuntimeHost::cbNotifyState(void* /*ctx*/, PJ_data_source_state_t /*state*/) noexcept {}

void DataSourceRuntimeHost::cbRequestStop(
    void* ctx, PJ_data_source_state_t /*terminal*/, PJ_string_view_t reason) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  self->requestStop(std::string_view(reason.data, reason.size));
}

bool DataSourceRuntimeHost::cbEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    const std::string_view encoding(request->parser_encoding.data, request->parser_encoding.size);
    const std::string_view topic_name(request->topic_name.data, request->topic_name.size);
    const std::string_view type_name(request->type_name.data, request->type_name.size);

    const LoadedMessageParser* parser_entry =
        self->catalog_.findParserByEncoding(QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size())));
    if (parser_entry == nullptr) {
      return self->fail(out_error, ("no parser found for encoding '" + std::string(encoding) + "'").c_str());
    }

    auto parser = std::make_unique<MessageParserHandle>(parser_entry->library.createHandle());
    if (!parser->valid()) {
      return self->fail(out_error, ("failed to create parser instance for '" + std::string(encoding) + "'").c_str());
    }

    auto topic_or = self->engine_.createTopic(self->dataset_id_, TopicDescriptor{.name = std::string(topic_name)});
    if (!topic_or.has_value()) {
      return self->fail(
          out_error, ("failed to create topic '" + std::string(topic_name) + "': " + topic_or.error()).c_str());
    }
    const PJ_topic_handle_t topic_handle{static_cast<uint32_t>(*topic_or)};

    auto write_host = std::make_unique<DatastoreParserWriteHost>(self->engine_, topic_handle);

    // Build the service registry the parser binds against. The builder must
    // outlive bind() because the plugin may hold a view into it; we move it
    // into the ParserBinding so its lifetime matches the parser's.
    auto registry_builder = std::make_unique<ServiceRegistryBuilder>();
    registry_builder->registerService<sdk::ParserWriteHostService>(write_host->raw());
    if (auto status = parser->bind(registry_builder->view()); !status) {
      return self->fail(out_error, ("failed to bind parser services: " + status.error()).c_str());
    }

    if (request->schema.size > 0) {
      const Span<const uint8_t> schema_span(request->schema.data, request->schema.size);
      if (auto status = parser->bindSchema(type_name, schema_span); !status) {
        return self->fail(
            out_error, ("failed to bind schema for " + std::string(type_name) + ": " + status.error()).c_str());
      }
    }

    if (request->parser_config_json.size > 0) {
      const std::string_view parser_config(request->parser_config_json.data, request->parser_config_json.size);
      if (auto status = parser->loadConfig(parser_config); !status) {
        return self->fail(out_error, ("failed to load parser config: " + status.error()).c_str());
      }
    }

    const uint32_t binding_id = self->next_binding_id_++;
    self->parser_bindings_.emplace(
        binding_id, ParserBinding{std::move(registry_builder), std::move(write_host), std::move(parser)});

    *out = PJ_parser_binding_handle_t{binding_id};
    qCInfo(lcIngest) << "[parser-bind] encoding="
                     << QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))
                     << "topic=" << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()));
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while binding parser");
  }
}

bool DataSourceRuntimeHost::cbPushRawMessage(
    void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_bytes_view_t payload,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    auto it = self->parser_bindings_.find(handle.id);
    if (it == self->parser_bindings_.end()) {
      return self->fail(out_error, "invalid parser binding handle");
    }
    if (auto status = it->second.parser->parse(timestamp_ns, Span<const uint8_t>(payload.data, payload.size));
        !status) {
      return self->fail(out_error, status.error().c_str());
    }
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while pushing raw message");
  }
}

int DataSourceRuntimeHost::cbShowMessageBox(
    void* /*ctx*/, PJ_message_box_type_t /*type*/, PJ_string_view_t title, PJ_string_view_t message,
    int buttons) noexcept {
  // Headless default — log and pick a positive button. The host is single-
  // threaded for the import, but Qt modal dialogs from a plugin callback are
  // fragile, so v1 doesn't pop them. Plugins that need user input go through
  // their own dialog capability.
  qCInfo(lcIngest) << "[plugin msgbox]" << QString::fromUtf8(title.data, static_cast<int>(title.size)) << "—"
                   << QString::fromUtf8(message.data, static_cast<int>(message.size));
  if ((buttons & PJ_MSG_BTN_OK) != 0) {
    return PJ_MSG_BTN_OK;
  }
  if ((buttons & PJ_MSG_BTN_YES) != 0) {
    return PJ_MSG_BTN_YES;
  }
  if ((buttons & PJ_MSG_BTN_CONTINUE) != 0) {
    return PJ_MSG_BTN_CONTINUE;
  }
  return -1;
}

const char* DataSourceRuntimeHost::cbListAvailableEncodings(void* ctx) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    // Build a JSON array of unique encodings the catalog knows. Cached on
    // the session so the returned char* is valid until the next call (per
    // the protocol contract).
    std::set<std::string> unique_encodings;
    for (const auto& parser : self->catalog_.messageParsers()) {
      for (const auto& encoding : parser.encodings) {
        unique_encodings.insert(encoding);
      }
    }
    std::string json = "[";
    bool first = true;
    for (const auto& enc : unique_encodings) {
      if (!first) {
        json += ",";
      }
      first = false;
      json += "\"" + enc + "\"";
    }
    json += "]";
    self->available_encodings_cache_ = std::move(json);
    return self->available_encodings_cache_.c_str();
  } catch (...) {
    return nullptr;
  }
}

}  // namespace PJ
