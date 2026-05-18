#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "pj_base/builtin/BuiltinObject.hpp"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_plugins/sdk/object_ingest_policy.hpp"

namespace PJ {

class DataEngine;
class DatastoreParserWriteHost;
class ExtensionCatalogService;
class MessageParserHandle;
class ServiceRegistryBuilder;

// Implements the host side of the v4 DataSource SDK for one ingest pass.
//
// Owns:
//   - the SourceWriteHostService that writes rows attributed to the source
//     itself,
//   - the runtime-host vtable (progress, stop, parser-bind, push-raw-message,
//     message-box stub, encoding-list helper),
//   - the set of parser bindings created on the fly when the source
//     delegates decoding to a MessageParser plugin.
//
// Lifetime is per-ingest: instantiate one, register its services into the
// builder used to bind the DataSource handle, run the plugin's start() loop,
// then call flushAll() to make written rows visible to readers. Destruction
// tears parser bindings down in the order required by the SDK contract
// (parser before write_host).
//
// Not a QObject — does not need signals or main-thread affinity and stays
// usable from headless or test contexts.
class DataSourceRuntimeHost {
 public:
  using ObjectTopicParserRegistrar = std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)>;

  // Wires the source-side write host immediately (engine + source_handle).
  // The plugin's bind() will see SourceWriteHostService and
  // DataSourceRuntimeHostService through registerServices().
  DataSourceRuntimeHost(
      DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle,
      ObjectStore& object_store, std::string source_id = {}, ObjectTopicParserRegistrar parser_registrar = {});

  ~DataSourceRuntimeHost();

  DataSourceRuntimeHost(const DataSourceRuntimeHost&) = delete;
  DataSourceRuntimeHost& operator=(const DataSourceRuntimeHost&) = delete;

  // Registers SourceWriteHostService + DataSourceRuntimeHostService into the
  // builder used to bind the DataSource plugin.
  void registerServices(ServiceRegistryBuilder& registry);

  // Flushes the source write host and every parser binding's write host.
  // Must be called once after handle.start() returns successfully so pending
  // rows reach the DataReader — open chunks are invisible until sealed, and
  // every parser binding has its own independent writer.
  void flushAll();

  // Stop signalling for the plugin's cooperative-cancellation callbacks. The
  // reason is also recorded as the last error.
  void requestStop(std::string_view reason);

  // Whether progress/stop callbacks should report a pending cancellation.
  bool stopRequested() const noexcept {
    return stop_requested_.load();
  }

  // Most recent error message captured by any callback. Empty if none.
  const std::string& lastError() const noexcept {
    return last_error_;
  }

  [[nodiscard]] sdk::ObjectIngestPolicyResolver& policyResolver() noexcept {
    return policy_resolver_;
  }

  [[nodiscard]] const sdk::ObjectIngestPolicyResolver& policyResolver() const noexcept {
    return policy_resolver_;
  }

 private:
  // ----- C-ABI callbacks -----
  // Each casts ctx to DataSourceRuntimeHost* and accesses members directly.
  static void cbReportMessage(void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept;
  static bool cbProgressStart(
      void* ctx, PJ_string_view_t label, uint64_t total, bool cancellable, PJ_error_t* out_error) noexcept;
  static bool cbProgressUpdate(void* ctx, uint64_t current) noexcept;
  static void cbProgressFinish(void* ctx) noexcept;
  static bool cbIsStopRequested(void* ctx) noexcept;
  static void cbNotifyState(void* ctx, PJ_data_source_state_t state) noexcept;
  static void cbRequestStop(void* ctx, PJ_data_source_state_t terminal, PJ_string_view_t reason) noexcept;
  static bool cbEnsureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out,
      PJ_error_t* out_error) noexcept;
  static bool cbPushRawMessage(
      void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_bytes_view_t payload,
      PJ_error_t* out_error) noexcept;
  static bool cbPushMessageV2(
      void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_message_data_fetcher_t fetch_message_data,
      PJ_error_t* out_error) noexcept;
  static int cbShowMessageBox(
      void* ctx, PJ_message_box_type_t type, PJ_string_view_t title, PJ_string_view_t message, int buttons) noexcept;
  static const char* cbListAvailableEncodings(void* ctx) noexcept;

  // The vtable referenced by every PJ_data_source_runtime_host_t handed to a
  // plugin. Definition in the cpp — single static instance shared by every
  // session.
  static const PJ_data_source_runtime_host_vtable_t kVtable;

  // Centralised failure path used by every callback that signals an error.
  // Stores `message` into last_error_ and fills `out_error` for the plugin.
  bool fail(PJ_error_t* out_error, const char* message) noexcept;

  // One parser binding owned by the host. Destruction order is load-bearing:
  // the parser may flush pending writes through `write_host` when destroyed,
  // so the parser must die BEFORE `write_host`. The registry builder only
  // supplies fat pointers at bind time — afterwards the plugin holds its own
  // copies, so the builder can die first.
  struct ParserBinding {
    std::unique_ptr<ServiceRegistryBuilder> registry_builder;
    std::unique_ptr<DatastoreParserWriteHost> write_host;
    std::unique_ptr<DatastoreParserObjectWriteHost> object_write_host;
    std::unique_ptr<MessageParserHandle> parser;
    std::string topic_name;
    sdk::BuiltinObjectType object_kind = sdk::BuiltinObjectType::kNone;
    std::optional<ObjectTopicId> object_topic_id;

    ParserBinding();
    ParserBinding(
        std::unique_ptr<ServiceRegistryBuilder> b, std::unique_ptr<DatastoreParserWriteHost> w,
        std::unique_ptr<DatastoreParserObjectWriteHost> ow, std::unique_ptr<MessageParserHandle> p, std::string topic,
        sdk::BuiltinObjectType kind, std::optional<ObjectTopicId> object_topic);
    ~ParserBinding();

    ParserBinding(ParserBinding&&) noexcept;
    ParserBinding& operator=(ParserBinding&&) noexcept;
  };

  DataEngine& engine_;
  ExtensionCatalogService& catalog_;
  ObjectStore& object_store_;
  std::string source_id_;
  ObjectTopicParserRegistrar object_topic_parser_registrar_;
  sdk::ObjectIngestPolicyResolver policy_resolver_;
  DatasetId dataset_id_;
  DatastoreSourceWriteHost source_write_host_;
  DatastoreSourceObjectWriteHost source_object_write_host_;

  std::string last_error_;
  std::atomic<bool> stop_requested_{false};
  uint32_t next_binding_id_ = 1;
  std::unordered_map<uint32_t, ParserBinding> parser_bindings_;
  // Backing for cbListAvailableEncodings — the protocol contract is that the
  // returned pointer is valid until the next call, so the buffer outlives the
  // function return.
  std::string available_encodings_cache_;
  bool flushed_ = false;
};

}  // namespace PJ
