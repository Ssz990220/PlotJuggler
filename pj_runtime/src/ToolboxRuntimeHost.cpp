#include "pj_runtime/ToolboxRuntimeHost.h"

#include <QMetaObject>
#include <string>
#include <utility>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

namespace PJ {

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks)
    : write_host_(engine, object_store),
      settings_host_(settings),
      callbacks_(std::move(callbacks)),
      runtime_vtable_{
          PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
          sizeof(PJ_toolbox_runtime_host_vtable_t),
          &ToolboxRuntimeHost::onReportMessage,
          &ToolboxRuntimeHost::onNotifyDataChanged,
      },
      runtime_{this, &runtime_vtable_} {}

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
            if (self->callbacks_.on_data_changed) {
              self->callbacks_.on_data_changed();
            }
          } catch (...) {}
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary.
  }
}

}  // namespace PJ
