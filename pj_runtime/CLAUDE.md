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
| `AppSession.h` | Central runtime object. Owns and exposes the services below. `pj_app` instantiates one at startup. **Source Timeline additions:** `datasetRawTimeRange(DatasetId)` returns the raw `[min,max]` absolute-ns bounds across a dataset's catalog-visible topics (used by `SourceTimelineController` to populate bar extents); `recomputeRange()` updates only the `PlaybackEngine` range from the offset-adjusted union of visible topics (called throttled on the drag path — never resets `currentTime`) and returns that union's display-min, which `seedPlaybackFromSession()` reuses for the first-load playhead snap (one scan, not two). Both are implemented via a shared `forEachVisibleRawRange` private scan so visibility/dedup rules live in one place. `mergeDatasets(selected)` **destructively** folds the selected datasets into one (the OR/union): the anchor = leftmost-in-display dataset keeps its clock + ids, the others are shifted into it by their relative display offset and removed from the catalog, scalar topics union by name, object topics fuse by name, and the anchor is relabelled `*_merged` (returns the anchor id). `AppSession::objectMergeConflicts(selected)` is the canonical builtin-object-type preflight; on successful merge AppSession emits `datasetsMerged(anchor, consumed)`. |
| `SessionManager.h` | Session lifecycle (load/clear/active dataset). Also owns and exposes the session's `CurveColorRegistry` so plot widgets can reach it through their existing `SessionManager` pointer. **Source Timeline additions:** `setDisplayOffset(DatasetId, DisplayOffset)` writes a per-source display shift to the underlying `TimeDomain` and emits the per-dataset `displayOffsetChanged(DatasetId)` — idempotent, so an unchanged offset emits nothing (no spurious replot/rebuild); it is consumed by `PlotWidget` (drops that dataset's adapter offset cache + replots at current zoom) and by `SourceTimelineController` (refreshes the bar offsets + triggers throttled `recomputeRange`). This per-source signal is **overloaded against** the no-arg `displayOffsetChanged()` that the global "Use time offset" frame (`setUseTimeOffset`) emits — connect either with `qOverload<>` / `qOverload<PJ::DatasetId>`. **Time-offset model (decoupled):** the total `displayOffset(id)` that everything on the display axis reads (plot curves, scenes, playback range/cursor) = the per-source *alignment* shift (`sourceDisplayOffset(id)`, the TimeDomain offset the Source Timeline edits) **plus** a single global `globalTimeReference()` (the earliest raw sample across all datasets when "Use time offset" is on, else 0 — applied **uniformly** so cross-dataset time gaps are preserved, not a per-source rebase). `setUseTimeOffset` flips *only* the global reference; it no longer bulk-writes per-source offsets, so toggling never disturbs the Timeline's bar positions. The Source Timeline reads `sourceDisplayOffset` for its bars (align-only frame) and the host (`SourceTimelineController`) bridges `globalTimeReference` across the playback↔timeline frame gap. |
| `CatalogModel.h` | Catalog of curves, objects, and data sources available to the UI. |
| `Time.h` | Display-relative time vocabulary: `DisplayOffset`, `DisplaySeconds`/`DisplayRange` (the Qwt/playback display-axis coordinate), the display adapters (`offsetOf`, `rawToDisplaySeconds`, `toAxisDouble`, …), and `kNanosecondsPerSecond`. Layered on the absolute spine (`Timepoint`/`Duration`/`fromRaw`/`toRaw`), which now lives in the SDK at `pj_base/time.hpp` so every layer can name absolute time; the display coordinate stays here because the per-dataset offset is an app-presentation policy, not an SDK concern. |
| `PlaybackEngine.h` | Time cursor + playback (play/pause/seek/loop). Public API speaks `DisplaySeconds`/`DisplayRange`; the `currentTimeChanged(double)` signal + `IDataWidget::onTrackerTime(double)` stay `double` (a fleet-wide moc/vtable contract). |
| `DataSourceRuntimeHost.h` | Host-side runner for `DataSource` plugins from `pj_plugins`. Threads a **plugin-DSO keepalive** (`shared_ptr<void>` from `DataSourceHandle::libraryOwner()`) into every lazy payload anchor + fetch closure, deferring `dlclose` until the last anchor/parser drops — so a decode worker never calls plugin code (`release`/`parseObject`) in an unmapped `.so`. Pitfall: a plugin `release` may run on an arbitrary worker thread during/after teardown (see the header). |
| `ToolboxRuntimeHost.h` | Host-side runner for `Toolbox` plugins: assembles `ToolboxHostService` (write surface) + `ToolboxRuntimeHostService` (diagnostics / data-changed) + `SettingsStoreService` into a `ServiceRegistry`. App concerns are injected as `Callbacks`; the `[thread-safe]` report/notify callbacks marshal onto the constructing (GUI) thread. With optional `ParserIngestDeps` (extension catalog + render-parser registrar), the `create_parser_ingest`/`release_parser_ingest` tail slots hand a toolbox-created dataset the standard `DataSourceRuntimeHost` delegated-ingest surface (one per dataset; release = flush + destroy, idempotent; driven from the toolbox worker thread, not GUI-marshalled). `on_data_changed` reports the datasets that received parser-ingest contexts so the shell can focus playback on a bulk import. |
| `DataProcessorService.h` | Applies "Data Processors" as eager `DerivedEngine` nodes. Two tiers of one substrate: **filters** (per-curve, hidden output, keyed by output topic) via `applyFilter`/`updateFilter`/`clearAllFilters`; **transforms** (named, plugin-owned, session-persisted, keyed `"<plugin>/<id>"`) via `upsertTransform`/`removeTransform`/`restoreTransform`/`transformRecipes`. Transforms resolve inputs by name, infer the backend from the script payload (Luau today; WASM/Python reserved), and roll back transactionally. `validateScript` compiles + test-runs a script without installing anything (for a plugin semaphore). |
| `DataProcessorsRuntimeHost.h` | Host-side bridge exposing the `pj.data_processors.v1` SDK service to ONE plugin over `DataProcessorService` (data-only; every id namespaced under the plugin; all slots `[main-thread]`, so no Qt / no marshalling). A created transform OUTLIVES the bridge — destroying it (DSO unload) leaves the node running; only an explicit `remove`/`clearTransformsForPlugin` tears it down. |
| `QSettingsBackend.h` | `sdk::SettingsBackend` implemented over `QSettings` (→ `PlotJuggler4.conf`); injected into `ToolboxRuntimeHost` so plugin settings persist. `'/'`-separated keys map to `.conf` groups. |
| `ExtensionCatalogService.h` | Marketplace-backed extension catalog (queries `pj_marketplace`). |
| `DiagnosticHistory.h` | Ring buffer of diagnostics surfaced via `pj_base::DiagnosticSink`. |
| `IDataWidget.h` | The contract every data widget (plot / 2D / 3D) implements so playback can drive tracker updates without coupling to concrete widget types. |
| `IObjectViewer.h` | The contract an object-store-backed viewer (e.g. a 2D image dock) implements so the shell can ask it to drop layers whose object topic was removed; returns whether any live layer remains. Pairs with `CatalogModel`'s `cleared()` / `itemsRemoved()` removal signals. |
| `CurveDescriptor.h` | Stable identifier for a curve in the datastore. |
| `CurveColorRegistry.h` | Session-scoped memory of each curve's color (hex string), so a curve keeps its color across plots (issue #68). Owned by `SessionManager` (and surfaced via `AppSession::curveColorRegistry()`); cleared when the catalog empties. |

## Linked dependencies

Public link surface (per `CMakeLists.txt`): `Qt6::Core`, `Qt6::Network`, `Qt6::Xml`, `pj_datastore`, `pj_marketplace`, `pj_plugin_runtime_catalog`, `nlohmann_json`. Private: `tsl::robin_map`, `pj_internal_fmt`, `pj_scripting` (DataProcessorService routes by-id applyFilter through the Luau FilterCatalogue).

## When porting from PJ3

PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into the services above — primarily `CatalogModel`, `SessionManager`, `PlaybackEngine`, and (future) `TransformRegistry`. This is the **one systematic rewrite** during a PJ3 port; everything else should be lifted close to verbatim.

## Tests

`tests/` — gtest-based, one binary per service (`app_session_test`, `catalog_model_test`, …). Add tests alongside any new service.
