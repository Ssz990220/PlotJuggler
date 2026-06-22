// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ToolboxRuntimeHost.h"

#include <QMetaObject>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ {

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks)
    : ToolboxRuntimeHost(engine, object_store, settings, std::move(callbacks), ParserIngestDeps{}) {}

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks,
    ParserIngestDeps parser_ingest)
    : write_host_(engine, object_store),
      settings_host_(settings),
      callbacks_(std::move(callbacks)),
      engine_(engine),
      object_store_(object_store),
      parser_ingest_deps_(std::move(parser_ingest)),
      runtime_vtable_{
          PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,        sizeof(PJ_toolbox_runtime_host_vtable_t),
          &ToolboxRuntimeHost::onReportMessage,      &ToolboxRuntimeHost::onNotifyDataChanged,
          &ToolboxRuntimeHost::onCreateParserIngest, &ToolboxRuntimeHost::onReleaseParserIngest,
      },
      runtime_{this, &runtime_vtable_} {}

ToolboxRuntimeHost::~ToolboxRuntimeHost() {
  // Flush anything a toolbox left unreleased so rows aren't lost on teardown.
  // Mirror the release path: take the contexts out under the lock, run plugin
  // parser destructors and engine flushes outside it.
  std::unordered_map<uint32_t, std::unique_ptr<DataSourceRuntimeHost>> contexts;
  {
    std::lock_guard lock(parser_ingest_mu_);
    contexts.swap(parser_ingests_);
  }
  for (auto& [id, host] : contexts) {
    host->flushAll();
  }
}

void ToolboxRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  registry.registerService<sdk::ToolboxHostService>(write_host_.raw());
  registry.registerService<sdk::ToolboxRuntimeHostService>(runtime_);
  registry.registerService<sdk::SettingsStoreService>(settings_host_.view());
}

void ToolboxRuntimeHost::onReportMessage(
    void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || !self->callbacks_.on_message) {
    return;
  }
  try {
    std::string text = message.data != nullptr ? std::string(message.data, message.size) : std::string();
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread) so on_message can touch Qt UI safely.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self, level, text = std::move(text)]() mutable {
          if (self->callbacks_.on_message) {
            self->callbacks_.on_message(level, std::move(text));
          }
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary: never let an exception escape into the plugin.
  }
}

void ToolboxRuntimeHost::onNotifyDataChanged(void* ctx) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return;
  }
  try {
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread). Seal buffered writes so the freshly written
    // rows/objects are visible before the host rebuilds its catalog — flush and
    // rebuild then always run together on the GUI thread.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self]() {
          try {
            self->write_host_.flushPending();
            // Drain the datasets that received parser-ingest contexts since
            // the previous notify; drained here (GUI thread) so the set and
            // the callback observing it stay ordered.
            std::vector<DatasetId> ingested;
            {
              std::lock_guard lock(self->parser_ingest_mu_);
              ingested.swap(self->pending_ingest_datasets_);
            }
            if (self->callbacks_.on_data_changed) {
              self->callbacks_.on_data_changed(std::move(ingested));
            }
          } catch (...) {}
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary.
  }
}

namespace {
// Same error taxonomy as DataSourceRuntimeHost::fail (the sibling ingest
// surface), under this host's own domain.
bool parserIngestFail(PJ_error_t* out_error, const char* msg) noexcept {
  sdk::fillError(out_error, 1, "pj.runtime.toolbox_ingest", msg);
  return false;
}
}  // namespace

bool ToolboxRuntimeHost::onCreateParserIngest(
    void* ctx, uint32_t data_source_id, PJ_data_source_runtime_host_t* out_host, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || out_host == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    if (self->parser_ingest_deps_.catalog == nullptr) {
      return parserIngestFail(out_error, "parser ingest is not configured on this host");
    }
    // Same validation the toolbox write host applies to object topics
    // (plugin_data_host.cpp toolboxRegisterObjectTopic): the id must be a
    // live dataset.
    if (self->engine_.getDataset(static_cast<DatasetId>(data_source_id)) == nullptr) {
      return parserIngestFail(out_error, "data source not found — call createDataSource first");
    }
    std::lock_guard lock(self->parser_ingest_mu_);
    // Construct first, insert only on success: a throwing constructor must not
    // leave a null entry in the map (the release path dereferences entries).
    auto it = self->parser_ingests_.find(data_source_id);
    if (it == self->parser_ingests_.end()) {
      auto host = std::make_unique<DataSourceRuntimeHost>(
          self->engine_, *self->parser_ingest_deps_.catalog, static_cast<DatasetId>(data_source_id),
          PJ_data_source_handle_t{data_source_id}, self->object_store_,
          "toolbox-ingest-" + std::to_string(data_source_id), self->parser_ingest_deps_.register_object_parser,
          // No secondary store/engine (toolbox ingest writes straight into the
          // primary), and no plugin-DSO keepalive — the toolbox isn't a
          // DataSource plugin; its render parsers come from the catalog, which
          // owns their .so lifetime and outlives every ingest context.
          /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
          /*library_keepalive=*/std::shared_ptr<void>{});
      it = self->parser_ingests_.emplace(data_source_id, std::move(host)).first;
    }
    // Remember the dataset for the next notify_data_changed: an ingest context
    // marks a bulk import the host will want to focus playback on.
    const auto ds_id = static_cast<DatasetId>(data_source_id);
    auto& pending = self->pending_ingest_datasets_;
    if (std::find(pending.begin(), pending.end(), ds_id) == pending.end()) {
      pending.push_back(ds_id);
    }
    *out_host = it->second->hostHandle();  // out_host written ONLY on success (documented contract)
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in create_parser_ingest");
  }
}

bool ToolboxRuntimeHost::onReleaseParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    std::unique_ptr<DataSourceRuntimeHost> victim;
    {
      std::lock_guard lock(self->parser_ingest_mu_);
      auto it = self->parser_ingests_.find(data_source_id);
      if (it == self->parser_ingests_.end()) {
        return true;  // idempotent
      }
      victim = std::move(it->second);
      self->parser_ingests_.erase(it);
    }
    // Seal rows BEFORE destruction so the next notify_data_changed/catalog
    // rebuild sees everything the parsers wrote.
    victim->flushAll();
    victim.reset();
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in release_parser_ingest");
  }
}

}  // namespace PJ
