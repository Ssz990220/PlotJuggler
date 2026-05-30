#pragma once

#include <QObject>
#include <functional>
#include <string>

#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {

class DataEngine;
class ObjectStore;
class ServiceRegistryBuilder;

// Host-side runner for a Toolbox plugin — the toolbox sibling of
// DataSourceRuntimeHost. Owns the host implementations of the services a
// toolbox plugin's bind() consumes and assembles them into a ServiceRegistry:
//
//   - ToolboxHostService        the write surface (DatastoreToolboxHost) into
//                               the session's DataEngine + ObjectStore.
//   - ToolboxRuntimeHostService diagnostics (report_message) + notify_data_changed.
//   - SettingsStoreService      persistence over an injected SettingsBackend.
//
// App-shell concerns (which catalog to rebuild, where messages surface) are
// injected as Callbacks, so this class stays Qt-Widgets-free and headless
// testable. Not movable: the runtime-host vtable stores `this`.
class ToolboxRuntimeHost {
 public:
  // Callbacks are always invoked on the thread that constructed this host (the
  // GUI thread) — see marshaller_. Safe to touch Qt models / session state.
  struct Callbacks {
    // Fired after notify_data_changed flushes buffered writes — the host
    // rebuilds its catalog / reseeds playback here.
    std::function<void()> on_data_changed;
    // Fired for each plugin diagnostic — routed to the app's notification UI.
    std::function<void(PJ_toolbox_message_level_t, std::string)> on_message;
  };

  ToolboxRuntimeHost(
      DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks);

  ToolboxRuntimeHost(const ToolboxRuntimeHost&) = delete;
  ToolboxRuntimeHost& operator=(const ToolboxRuntimeHost&) = delete;

  // Registers ToolboxHostService + ToolboxRuntimeHostService + SettingsStoreService
  // into the builder used to bind the toolbox plugin.
  void registerServices(ServiceRegistryBuilder& registry);

 private:
  static void onReportMessage(void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept;
  static void onNotifyDataChanged(void* ctx) noexcept;

  DatastoreToolboxHost write_host_;
  sdk::SettingsStoreHost settings_host_;
  Callbacks callbacks_;
  PJ_toolbox_runtime_host_vtable_t runtime_vtable_;
  PJ_toolbox_runtime_host_t runtime_;
  // The toolbox runtime-host vtable advertises report_message/notify_data_changed
  // as [thread-safe], so a plugin may call them from a worker thread. The
  // callbacks above touch Qt UI/session state, so the trampolines marshal them to
  // this QObject's thread (= the constructing/GUI thread) via Qt::AutoConnection:
  // same-thread calls run synchronously, cross-thread calls are queued.
  QObject marshaller_;
};

}  // namespace PJ
