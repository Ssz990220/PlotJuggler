# pj_runtime

## Purpose

App runtime services and contracts for PlotJuggler 4. This is the **services seam** between the shell (`pj_app`) and the rest of the system: every widget family (`pj_plotting`, `pj_scene2D/widgets`, future `pj_3d_widgets`) reads from and reacts to services exposed here, but the widget families themselves are siblings and never depend on each other.

## Hard constraints (from root CLAUDE.md)

- Qt is allowed (`Qt6::Core`, `Qt6::Network`).
- **`Qt6::Widgets` is forbidden.** No concrete widgets live here. Anything that needs `QWidget` belongs in `pj_widgets` or a widget-family module.
- Public headers under `include/pj_runtime/` are the SDK boundary for the shell and widget modules.

## Public surface

The authoritative source is `include/pj_runtime/`. Today:

| Header | Role |
|---|---|
| `AppSession.h` | Central runtime object. Owns and exposes the services below. `pj_app` instantiates one at startup. |
| `SessionManager.h` | Session lifecycle (load/clear/active dataset). |
| `CatalogModel.h` | Catalog of curves, objects, and data sources available to the UI. |
| `PlaybackEngine.h` | Time cursor + playback (play/pause/seek/loop). Drives `IDataWidget::onTrackerTime`. |
| `DataSourceRuntimeHost.h` | Host-side runner for `DataSource` plugins from `pj_plugins`. |
| `ToolboxRuntimeHost.h` | Host-side runner for `Toolbox` plugins: assembles `ToolboxHostService` (write surface) + `ToolboxRuntimeHostService` (diagnostics / data-changed) + `SettingsStoreService` into a `ServiceRegistry`. App concerns are injected as `Callbacks`; the `[thread-safe]` vtable callbacks marshal onto the constructing (GUI) thread. |
| `QSettingsBackend.h` | `sdk::SettingsBackend` implemented over `QSettings` (→ `PlotJuggler4.conf`); injected into `ToolboxRuntimeHost` so plugin settings persist. `'/'`-separated keys map to `.conf` groups. |
| `ExtensionCatalogService.h` | Marketplace-backed extension catalog (queries `pj_marketplace`). |
| `DiagnosticHistory.h` | Ring buffer of diagnostics surfaced via `pj_base::DiagnosticSink`. |
| `IDataWidget.h` | The contract every data widget (plot / 2D / 3D) implements so playback can drive tracker updates without coupling to concrete widget types. |
| `CurveDescriptor.h` | Stable identifier for a curve in the datastore. |

## Linked dependencies

Public link surface (per `CMakeLists.txt`): `Qt6::Core`, `Qt6::Network`, `pj_datastore`, `pj_marketplace`, `pj_plugin_runtime_catalog`, `nlohmann_json`. Private: `tsl::robin_map`, `fmt`.

## When porting from PJ3

PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into the services above — primarily `CatalogModel`, `SessionManager`, `PlaybackEngine`, and (future) `TransformRegistry`. This is the **one systematic rewrite** during a PJ3 port; everything else should be lifted close to verbatim.

## Tests

`tests/` — gtest-based, one binary per service (`app_session_test`, `catalog_model_test`, …). Add tests alongside any new service.
