#include "pj_runtime/DataSourceRuntimeHost.h"

#include <fmt/format.h>

#include <QLoggingCategory>
#include <QString>
#include <functional>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcIngest, "pj.runtime.ingest")

struct PayloadAnchorGuard {
  PJ_payload_anchor_t anchor;
  ~PayloadAnchorGuard() {
    if (anchor.release != nullptr) {
      anchor.release(anchor.ctx);
    }
  }
};

std::vector<uint8_t> copyPayloadBytes(const PJ_payload_t& payload) {
  std::vector<uint8_t> bytes;
  if (payload.size > 0 && payload.data != nullptr) {
    bytes.assign(payload.data, payload.data + payload.size);
  }
  return bytes;
}

struct FetcherOwner {
  explicit FetcherOwner(PJ_message_data_fetcher_t fetcher_in) : fetcher(fetcher_in) {}

  FetcherOwner(const FetcherOwner&) = delete;
  FetcherOwner& operator=(const FetcherOwner&) = delete;

  ~FetcherOwner() {
    if (fetcher.release != nullptr) {
      fetcher.release(fetcher.ctx);
    }
  }

  PJ_message_data_fetcher_t fetcher;
};

QString errorMessage(const PJ_error_t& err) {
  if (err.message[0] == '\0') {
    return QStringLiteral("<none>");
  }
  return QString::fromUtf8(err.message);
}

struct LazyFetchContext {
  DatasetId dataset_id = 0;
  ObjectTopicId object_topic_id{};
  std::string source_id;
  std::string topic_name;
  int64_t timestamp_ns = 0;
};

std::function<std::vector<uint8_t>()> makeLazyFetchClosure(
    std::shared_ptr<FetcherOwner> owner, std::shared_ptr<std::mutex> fetch_mutex, LazyFetchContext context) {
  return [owner = std::move(owner), fetch_mutex = std::move(fetch_mutex),
          context = std::move(context)]() -> std::vector<uint8_t> {
    PJ_payload_t payload{};
    PJ_error_t err{};
    bool ok = false;
    if (owner->fetcher.fetchMessageData != nullptr) {
      if (fetch_mutex != nullptr) {
        std::lock_guard lock(*fetch_mutex);
        ok = owner->fetcher.fetchMessageData(owner->fetcher.ctx, &payload, &err);
      } else {
        ok = owner->fetcher.fetchMessageData(owner->fetcher.ctx, &payload, &err);
      }
    }
    if (!ok) {
      qCWarning(lcIngest) << "[lazy-fetch] failed source=" << QString::fromStdString(context.source_id)
                          << "dataset=" << context.dataset_id << "topic=" << QString::fromStdString(context.topic_name)
                          << "object_topic_id=" << context.object_topic_id.id << "timestamp_ns=" << context.timestamp_ns
                          << "error=" << errorMessage(err);
      return {};
    }
    PayloadAnchorGuard payload_anchor_guard{payload.anchor};
    if (payload.data == nullptr && payload.size > 0) {
      qCWarning(lcIngest) << "[lazy-fetch] null data with nonzero size source="
                          << QString::fromStdString(context.source_id) << "dataset=" << context.dataset_id
                          << "topic=" << QString::fromStdString(context.topic_name)
                          << "object_topic_id=" << context.object_topic_id.id << "timestamp_ns=" << context.timestamp_ns
                          << "payload_size=" << payload.size;
      return {};
    }
    if (payload.size == 0) {
      qCWarning(lcIngest) << "[lazy-fetch] empty payload source=" << QString::fromStdString(context.source_id)
                          << "dataset=" << context.dataset_id << "topic=" << QString::fromStdString(context.topic_name)
                          << "object_topic_id=" << context.object_topic_id.id
                          << "timestamp_ns=" << context.timestamp_ns;
      return {};
    }
    return copyPayloadBytes(payload);
  };
}
}  // namespace

// ---------------------------------------------------------------------------
// ParserBinding — out-of-line definitions so the header only needs forward
// declarations of MessageParserHandle / DatastoreParserWriteHost /
// ServiceRegistryBuilder.
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::ParserBinding::ParserBinding() = default;

DataSourceRuntimeHost::ParserBinding::ParserBinding(
    std::unique_ptr<ServiceRegistryBuilder> b, std::unique_ptr<DatastoreParserWriteHost> w,
    std::unique_ptr<DatastoreParserObjectWriteHost> ow, std::unique_ptr<MessageParserHandle> p, std::string topic,
    sdk::BuiltinObjectType kind, std::optional<ObjectTopicId> object_topic)
    : registry_builder(std::move(b)),
      write_host(std::move(w)),
      object_write_host(std::move(ow)),
      parser(std::move(p)),
      topic_name(std::move(topic)),
      object_kind(kind),
      object_topic_id(object_topic) {}

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
    .push_message_v2 = &DataSourceRuntimeHost::cbPushMessageV2,
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::DataSourceRuntimeHost(
    DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle,
    ObjectStore& object_store, std::string source_id, ObjectTopicParserRegistrar parser_registrar)
    : engine_(engine),
      catalog_(catalog),
      object_store_(object_store),
      source_id_(std::move(source_id)),
      object_topic_parser_registrar_(std::move(parser_registrar)),
      dataset_id_(dataset_id),
      source_write_host_(engine, source_handle),
      source_object_write_host_(object_store, dataset_id),
      lazy_fetch_mutex_(std::make_shared<std::mutex>()) {}

DataSourceRuntimeHost::~DataSourceRuntimeHost() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DataSourceRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  registry.registerService<sdk::SourceWriteHostService>(source_write_host_.raw());
  registry.registerService<sdk::SourceObjectWriteHostService>(source_object_write_host_.raw());
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
    void* ctx, PJ_string_view_t label, uint64_t total, bool cancellable, PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->onProgressStart) {
    self->onProgressStart(std::string_view(label.data, label.size), total, cancellable);
  }
  return true;
}

bool DataSourceRuntimeHost::cbProgressUpdate(void* ctx, uint64_t current) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->stop_requested_.load()) {
    return false;
  }
  if (self->onProgressUpdate) {
    return self->onProgressUpdate(current);
  }
  return true;
}

void DataSourceRuntimeHost::cbProgressFinish(void* ctx) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->onProgressFinish) {
    self->onProgressFinish();
  }
}

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

    const Span<const uint8_t> schema_span(request->schema.data, request->schema.size);
    if (auto status = parser->bindSchema(type_name, schema_span); !status) {
      return self->fail(
          out_error, ("failed to bind schema for " + std::string(type_name) + ": " + status.error()).c_str());
    }

    std::string parser_config;
    if (request->parser_config_json.size > 0) {
      parser_config.assign(request->parser_config_json.data, request->parser_config_json.size);
      if (auto status = parser->loadConfig(parser_config); !status) {
        return self->fail(out_error, ("failed to load parser config: " + status.error()).c_str());
      }
    }

    const sdk::BuiltinObjectType object_kind = parser->classifySchema(type_name, schema_span);
    std::optional<ObjectTopicId> object_topic_id;
    std::unique_ptr<DatastoreParserObjectWriteHost> object_write_host;
    if (object_kind == sdk::BuiltinObjectType::kNone) {
      // The parser declined to classify this topic as a builtin object — it
      // will only produce scalar columns. Surface this once per binding so
      // the operator knows why an image-shaped topic might not be showing in
      // the catalog as an ObjectTopic. Plugins that legitimately do not
      // produce objects (string topics, etc.) just generate one info line.
      qCInfo(lcIngest) << "[parser-bind] classifySchema=kNone topic="
                       << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                       << "type=" << QString::fromUtf8(type_name.data(), static_cast<int>(type_name.size()))
                       << "encoding=" << QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))
                       << "— scalar-only ingest";
    } else {
      if (auto existing = self->object_store_.findTopic(self->dataset_id_, topic_name); existing.has_value()) {
        object_topic_id = existing;
      } else {
        const std::string metadata_json = fmt::format(R"({{"builtin_object_type":"{}"}})", sdk::name(object_kind));
        auto registered = self->object_store_.registerTopic(
            ObjectTopicDescriptor{
                .dataset_id = self->dataset_id_,
                .topic_name = std::string(topic_name),
                .metadata_json = metadata_json,
            });
        if (!registered.has_value()) {
          return self->fail(
              out_error,
              ("failed to register object topic '" + std::string(topic_name) + "': " + registered.error()).c_str());
        }
        object_topic_id = *registered;
      }
      object_write_host = std::make_unique<DatastoreParserObjectWriteHost>(self->object_store_, object_topic_id->id);
      registry_builder->registerService<sdk::ParserObjectWriteHostService>(object_write_host->raw());

      if (self->object_topic_parser_registrar_) {
        auto object_parser = std::make_unique<MessageParserHandle>(parser_entry->library.createHandle());
        if (!object_parser->valid()) {
          return self->fail(
              out_error, ("failed to create object parser instance for '" + std::string(encoding) + "'").c_str());
        }
        if (auto status = object_parser->bindSchema(type_name, schema_span); !status) {
          return self->fail(
              out_error,
              ("failed to bind object parser schema for " + std::string(type_name) + ": " + status.error()).c_str());
        }
        if (!parser_config.empty()) {
          if (auto status = object_parser->loadConfig(parser_config); !status) {
            return self->fail(out_error, ("failed to load object parser config: " + status.error()).c_str());
          }
        }
        self->object_topic_parser_registrar_(*object_topic_id, std::move(object_parser));
      }
    }

    if (auto status = parser->bind(registry_builder->view()); !status) {
      return self->fail(out_error, ("failed to bind parser services: " + status.error()).c_str());
    }

    const uint32_t binding_id = self->next_binding_id_++;
    self->parser_bindings_.emplace(
        binding_id, ParserBinding{
                        std::move(registry_builder),
                        std::move(write_host),
                        std::move(object_write_host),
                        std::move(parser),
                        std::string(topic_name),
                        object_kind,
                        object_topic_id,
                    });

    *out = PJ_parser_binding_handle_t{binding_id};
    qCInfo(lcIngest) << "[parser-bind] encoding="
                     << QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))
                     << "topic=" << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                     << "object_kind=" << static_cast<int>(object_kind);
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
    if (it->second.object_topic_id.has_value()) {
      std::vector<uint8_t> owned;
      if (payload.size > 0) {
        owned.assign(payload.data, payload.data + payload.size);
      }
      if (auto status = self->object_store_.pushOwned(*it->second.object_topic_id, timestamp_ns, std::move(owned));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushOwned failed: " + status.error()).c_str());
      }
    }
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while pushing raw message");
  }
}

bool DataSourceRuntimeHost::cbPushMessageV2(
    void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_message_data_fetcher_t fetch_message_data,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  auto fetcher_owner = std::make_shared<FetcherOwner>(fetch_message_data);

  try {
    auto it = self->parser_bindings_.find(handle.id);
    if (it == self->parser_bindings_.end()) {
      return self->fail(out_error, "invalid parser binding handle");
    }
    auto& binding = it->second;
    if (fetcher_owner->fetcher.fetchMessageData == nullptr) {
      return self->fail(out_error, "message data fetcher is null");
    }

    // Object ingest policy only applies to parser bindings that actually
    // classify as builtin objects. Scalar-only topics must stay eager so a
    // broad default lazy policy cannot accidentally drop normal curves.
    const bool is_object_topic = binding.object_topic_id.has_value();
    const auto policy = is_object_topic
                            ? self->policy_resolver_.resolve(self->source_id_, binding.topic_name, binding.object_kind)
                            : sdk::ObjectIngestPolicy::kEager;

    auto push_lazy_object = [&]() -> bool {
      if (!is_object_topic) {
        return true;
      }
      auto closure = makeLazyFetchClosure(
          fetcher_owner, self->lazy_fetch_mutex_,
          LazyFetchContext{
              .dataset_id = self->dataset_id_,
              .object_topic_id = *binding.object_topic_id,
              .source_id = self->source_id_,
              .topic_name = binding.topic_name,
              .timestamp_ns = timestamp_ns,
          });
      if (auto status = self->object_store_.pushLazy(*binding.object_topic_id, timestamp_ns, std::move(closure));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushLazy failed: " + status.error()).c_str());
      }
      return true;
    };

    if (policy == sdk::ObjectIngestPolicy::kPureLazy) {
      return push_lazy_object();
    }

    PJ_payload_t payload{};
    bool fetched = false;
    if (self->lazy_fetch_mutex_ != nullptr) {
      std::lock_guard lock(*self->lazy_fetch_mutex_);
      fetched = fetcher_owner->fetcher.fetchMessageData(fetcher_owner->fetcher.ctx, &payload, out_error);
    } else {
      fetched = fetcher_owner->fetcher.fetchMessageData(fetcher_owner->fetcher.ctx, &payload, out_error);
    }
    if (!fetched) {
      return false;
    }

    PayloadAnchorGuard payload_anchor_guard{payload.anchor};
    if (payload.data == nullptr && payload.size > 0) {
      return self->fail(out_error, "message data fetcher returned null data");
    }
    if (auto status = binding.parser->parse(timestamp_ns, Span<const uint8_t>(payload.data, payload.size)); !status) {
      return self->fail(out_error, status.error().c_str());
    }

    if (policy == sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars) {
      return push_lazy_object();
    }

    if (is_object_topic) {
      if (auto status =
              self->object_store_.pushOwned(*binding.object_topic_id, timestamp_ns, copyPayloadBytes(payload));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushOwned failed: " + status.error()).c_str());
      }
    }
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while pushing message v2");
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
