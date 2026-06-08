# pj_runtime

## Purpose

App runtime services and contracts for PlotJuggler 4. This is the **services seam** between the shell (`pj_app`) and the rest of the system: every widget family (`pj_plotting`, `pj_scene2D/widgets`, `pj_scene3D/widgets`) reads from and reacts to services exposed here, but the widget families themselves are siblings and never depend on each other.

## Hard constraints (from root CLAUDE.md)

- Qt is allowed (`Qt6::Core`, `Qt6::Network`, `Qt6::Xml`).
- **`Qt6::Widgets` is forbidden.** No concrete widgets live here. Anything that needs `QWidget` belongs in `pj_widgets` or a widget-family module.
- Public headers under `include/pj_runtime/` are the SDK boundary for the shell and widget modules.

## Public surface

The authoritative source is `include/pj_runtime/`. Today:

| Header | Role |
|---|---|
| `AppSession.h` | Central runtime object. Owns and exposes the services below. `pj_app` instantiates one at startup. |
| `SessionManager.h` | Session lifecycle (load/clear/active dataset). Also owns and exposes the session's `CurveColorRegistry` so plot widgets can reach it through their existing `SessionManager` pointer. |
| `CatalogModel.h` | Catalog of curves, objects, and data sources available to the UI. |
| `Time.h` | Canonical time vocabulary: `Timepoint` (absolute `sys_time<ns>`), `Duration`, and `DisplaySeconds` (the Qwt/playback display-axis coordinate), plus named boundary adapters (`fromRaw`/`toRaw`, `rawToDisplaySeconds`, `toAxisDouble`). Sits above the frozen int64-ns spine; nothing here touches the SDK submodule. |
| `PlaybackEngine.h` | Time cursor + playback (play/pause/seek/loop). Public API speaks `DisplaySeconds`/`DisplayRange`; the `currentTimeChanged(double)` signal + `IDataWidget::onTrackerTime(double)` stay `double` (a fleet-wide moc/vtable contract). |
| `DataSourceRuntimeHost.h` | Host-side runner for `DataSource` plugins from `pj_plugins`. |
| `ToolboxRuntimeHost.h` | Host-side runner for `Toolbox` plugins: assembles `ToolboxHostService` (write surface) + `ToolboxRuntimeHostService` (diagnostics / data-changed) + `SettingsStoreService` into a `ServiceRegistry`. App concerns are injected as `Callbacks`; the `[thread-safe]` vtable callbacks marshal onto the constructing (GUI) thread. |
| `QSettingsBackend.h` | `sdk::SettingsBackend` implemented over `QSettings` (→ `PlotJuggler4.conf`); injected into `ToolboxRuntimeHost` so plugin settings persist. `'/'`-separated keys map to `.conf` groups. |
| `ExtensionCatalogService.h` | Marketplace-backed extension catalog (queries `pj_marketplace`). |
| `DiagnosticHistory.h` | Ring buffer of diagnostics surfaced via `pj_base::DiagnosticSink`. |
| `IDataWidget.h` | The contract every data widget (plot / 2D / 3D) implements so playback can drive tracker updates without coupling to concrete widget types. |
| `IObjectViewer.h` | The contract an object-store-backed viewer (e.g. a 2D image dock) implements so the shell can ask it to drop layers whose object topic was removed; returns whether any live layer remains. Pairs with `CatalogModel`'s `cleared()` / `itemsRemoved()` removal signals. |
| `CurveDescriptor.h` | Stable identifier for a curve in the datastore. |
| `CurveColorRegistry.h` | Session-scoped memory of each curve's color (hex string), so a curve keeps its color across plots (issue #68). Owned by `SessionManager` (and surfaced via `AppSession::curveColorRegistry()`); cleared when the catalog empties. |
| `constants.h` | `PJ::kNanosecondsPerSecond` — single-source-of-truth seconds<->nanoseconds factor (derived from `std::chrono::nanoseconds::period`) for the int64-ns spine <-> seconds-double conversions at the IDataWidget/Qwt time boundary. Consumed by `Time.h`. |

## Linked dependencies

Public link surface (per `CMakeLists.txt`): `Qt6::Core`, `Qt6::Network`, `Qt6::Xml`, `pj_datastore`, `pj_marketplace`, `pj_plugin_runtime_catalog`, `nlohmann_json`. Private: `tsl::robin_map`, `pj_internal_fmt`.

## When porting from PJ3

PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into the services above — primarily `CatalogModel`, `SessionManager`, `PlaybackEngine`, and (future) `TransformRegistry`. This is the **one systematic rewrite** during a PJ3 port; everything else should be lifted close to verbatim.

## Tests

`tests/` — gtest-based, one binary per service (`app_session_test`, `catalog_model_test`, …). Add tests alongside any new service.
