# TODO — PJ4 PlotWidget port (M2 – M6)

You are picking up a multi-milestone implementation task with cleared context. Read the documents in §1 in order before touching any code. The full architectural plan is the HTML at `plan_plotwidget_port.html` — it is the source of truth; this TODO only summarizes the operational layer.

---

## 1. Required reading (in this order)

| # | Path | Why |
|---|---|---|
| 1 | `CLAUDE.md` (this repo) | Project rules: build, conventions, porting policy, commit policy. Top-of-stack. |
| 2 | `plan_plotwidget_port.html` (this repo) | THE master plan. Open in any browser or read as HTML. Especially: §3 (file map), §4 (class hierarchy), §5 (DatastoreCurveAdapter — already implemented but read it for the design contract), §6–§9 (drag-drop, color, sync zoom, tracker), §10 (services), §12 (milestones), §13 (verification per milestone), §15 (engineering conventions). |
| 3 | `PJ4_PLAN.md` (this repo) | Strategic architecture: §0 (revisions, sibling-widget rule), §5.3 (wholesale-lift strategy), §8 (plotting subsystem). |
| 4 | `plotjuggler_core/CLAUDE.md` | Substrate library rules. Naming (`CamelCase` classes, `camelBack` functions, `lower_case` locals, `lower_case_` members, `kCamelCase` constants), error handling (`PJ::Expected<T>` / `PJ::Status`), containers (`tsl::robin_map`), invariants (`PJ_ASSERT`). |
| 5 | `plotjuggler_core/docs/cpp_design_recommendations.md` | Style guide referenced from CLAUDE.md. |
| 6 | `plotjuggler_core/pj_datastore/docs/REQUIREMENTS.md` and `ARCHITECTURE.md` | Datastore data model, ingest contract, encoding, retention, query semantics. |
| 7 | `plotjuggler_core/pj_datastore/include/pj_datastore/{engine,reader,query,chunk,topic_storage,plugin_data_host}.hpp` | The actual public APIs you will call from the adapter and any new code. |
| 8 | `plotjuggler_core/pj_plugins/docs/data-source-guide.md` | DataSource plugin SDK. Helpful for understanding the `FileLoader` integration; plot widgets don't talk to plugins directly. |
| 9 | `plotjuggler_core/pj_marketplace/include/pj_marketplace/extension.hpp` | What `LoadedDataSource` looks like. |
| 10 | PJ3 reference: `~/ws_plotjuggler/PlotJuggler/plotjuggler_app/{plotwidget,plotwidget_base,plot_docker,plot_docker_toolbar,tabbedplotwidget,curve_tracker,point_series_xy,curvelist_view,mainwindow}.{h,cpp}` and `~/ws_plotjuggler/PlotJuggler/plotjuggler_base/{include/PlotJuggler,src}/{plotwidget_base,plotzoomer,plotpanner,plotmagnifier,plotlegend,timeseries_qwt}.*` | Source code to PORT (verbatim with style adaptation) for M2 / M5 / M6. Read-only. |

The PJ3 path in CLAUDE.md is `~/ws_plotjuggler/PlotJuggler/` (NOT `~/ws_plotjuggler/src/PlotJuggler/`).

---

## 2. State of the world (what's already done)

You are starting on branch `development` at HEAD `d713b41`. Already committed and merged:

### M1 — adapter foundation (DONE)

- `pj_app_core/include/pj_app_core/CurveDescriptor.h` — POD struct mapping curve name → `(TopicId, DatasetId, ColumnIndex, field_path, display_offset_ns)`.
- `pj_app_core::SessionManager` — owns `PJ::DataEngine`, `createReader()`, `commitChunks()` wrapper that emits `topicsCommitted(QVector<TopicId>)`.
- `pj_app_core::CatalogModel` — enumerates numeric leaf columns of all topics into curve names like `topic/field/subfield`; `curveDescriptor(name)` lookup; subscribes to `topicsCommitted` and rebuilds.
- `pj_plot_widgets::DatastoreCurveAdapter` — final design (see plan §5). Pull-through `QwtSeriesData<QPointF>`, no copy of timeseries data. Chunk-index with last-slot cache, cross-chunk boundary guards, `boundingRect()` returns full-data bounds via `ColumnStats` (not ROI-bounded), `visibleYRange(x_min, x_max)` for PJ3-style vertical zoom-to-visible. 11/11 tests pass.

### Load-file flow (DONE)

- `pj_app/src/FileLoader.{h,cpp}` — drives QFileDialog → marketplace plugin lookup → DatastoreSourceWriteHost ingest → `CatalogModel::rebuildFromDatastore()`. Wired to `LeftPanel::loadDataRequested` in `MainWindow`. Tested manually with the marketplace CSV plugin.

### Sidebar cleanup (DONE)

- Publishers section removed from `LeftPanel.{ui,cpp}` (StatePublisher parity is a non-goal per `PJ4_PLAN.md`).

### Scaffolding present (you'll modify, not create)

- `pj_plot_widgets/{DockWidget,PlotDocker,TabbedPlotWidget,DockToolbar}` — exist as stubs with placeholder content. M2 fills them in.
- `pj_app/src/MainWindow.{h,cpp,ui}` — exists; you'll add `buttonLink` and zoom-broadcast slots in M5.
- `pj_app/src/ui/{CurveListPanel,CurveTreeView,LeftPanel,TimelineWidget}` — left as-is. CurveTreeView already emits the right MIME formats (`curveslist/add_curve`, `curveslist/new_XY_axis`).

---

## 3. Decisions already made (do not re-ask)

| Decision | Choice |
|---|---|
| Data ingestion path | Marketplace CSV plugin (already wired via `FileLoader`). Don't build importers. |
| Link toggle location | `MainWindow` toolbar (global), object name `buttonLink` (PJ3 name preserved). |
| Color picker | Stock `QColorDialog`, NOT `color_widgets`. Do not vendor a color wheel. |
| Curve naming | `topic/field/subfield` — all slashes (already implemented in `CatalogModel`). |
| Editor / Transforms / Statistics dialogs | **DEFERRED** to next phase. Out of scope for this work. |
| `PlotBackground` / colormap selector / colormap editor | Deferred (post-v1). |
| XY tracker visualization | Deferred (no time axis to track on). |
| Workspace-to-disk persistence | Deferred. M4's color persistence is in-memory XML round-trip only. |
| Adapter-side decimation | **NEVER**. Qwt's `QwtPlotCurve::FilterPointsAggressive` + `ClipPolygons` own paint-time decimation. Pre-decimating breaks on zoom. |

---

## 4. Engineering conventions (mandatory)

Use `plotjuggler_core/` as the reference template for design patterns and types. Read `plotjuggler_core/docs/cpp_design_recommendations.md` if a pattern isn't obvious.

| Concern | Use this | Not this |
|---|---|---|
| Hash map | `tsl::robin_map` | `std::unordered_map` |
| Fallible return | `PJ::Expected<T>` | Exceptions, error-out-params |
| Void fallible | `PJ::Status` | `bool` + side channel |
| Invariants | `PJ_ASSERT(cond, msg)` | bare `assert` |
| Stable IDs | typed wrappers (`TopicId`, `DatasetId`, `FieldId`) | raw `uint32_t` |
| Time at storage | `PJ::Timestamp` (int64 ns since epoch) | `double` seconds at storage layer |
| Stable iteration | `std::deque` for chunks (refs survive append) | `std::vector` when growth invalidates pointers |
| Class names | `CamelCase` | `snake_case` |
| Function names | `camelBack` | `snake_case` |
| Local vars | `lower_case` | `camelBack` locals |
| Members | `lower_case_` (trailing underscore) | `m_` prefix |
| Constants | `kCamelCase` | `UPPER_CASE` |
| Files | `PascalCase.{h,cpp}` in PJ4 | `snake_case.h` |
| Const-correctness | `[[nodiscard]]` on factory/lookup returns; `const` by default; `noexcept` on accessors | default mutable, default discardable |
| Namespace | `PJ` (flat) | nested vendor namespaces |
| Error format | `PJ::Expected` text errors | exceptions, log-and-continue |

Build / test:
- `./build.sh` from repo root (incremental).
- `./run.sh` to launch (sets `QT_IM_MODULE=` to avoid IBus segfault under Qt 6.8.3).
- Tests: `cd build && ctest -R <pattern> --output-on-failure`.
- Vendored Qwt CMake target name is `plotjuggler_qwt` (not `qwt` or `Qt6::Qwt`).

---

## 5. Hard rules

1. **Do NOT modify `plotjuggler_core/`**. If you find a missing API while building, STOP and ask. Do not add anything to the submodule.
2. **Do NOT add new `pj_app_core` services** (no `WidgetRegistry`, `TransformRegistry`, `WorkspaceManager`, `UndoManager`, `ToolboxManager`, `NotificationCenter`). Out of scope for v1.
3. **`pj_app_core` may NOT link `Qt6::Widgets`** — only `Qt6::Core`/`Gui`/`Network`/`Svg`. Plot widgets live in `pj_plot_widgets`, which CAN link Qt6::Widgets and Qwt.
4. **Do NOT add a downsampler / decimator in the adapter or anywhere else.** Qwt owns paint-time filtering. The adapter is a thin pull-through.
5. **`boundingRect()` must return FULL-data bounds, NEVER ROI-bounded.** Qwt's autoscale path calls it before propagating ROI; ROI-bounded bounds would lag a frame.
6. **`setRectOfInterest` must NOT cause `rectChanged` echo emits.** When MainWindow broadcasts a peer update via `setZoomRectangle(rect, /*emit=*/false)`, that path must NOT re-emit. Otherwise infinite recursion.
7. **Drop targets**: drag-drop accept lives on the Qwt plot canvas (forwarded through `PlotWidgetBase`), NOT on `DockWidget`. Match PJ3.
8. **PJ3 `objectName` preservation** in ported `.ui` files (`buttonLink`, `buttonSplitHorizontal`, `buttonSplitVertical`, `buttonFullscreen`, `buttonClose`, etc.). Adapt class/file names to PascalCase, but keep widget object names verbatim.
9. **Per-curve display transforms stay plot-local** — do NOT route through `DerivedEngine` (which is for persistent derived topics).
10. **`PointSeriesXY` is its own `QwtSeriesData<QPointF>` subclass with its own alignment index.** Do NOT compose it from two `DatastoreCurveAdapter`s — Qwt's ROI X-coord for an XY plot is the X curve's value, not display time, so forwarding ROI to two time-series adapters would narrow on the wrong domain.
11. **Display offset is looked up live** via `engine.getTimeDomain(...)`. Do NOT cache it on adapters. (M1 already enforces this; preserve.)
12. **`pj_proto_app/` is deprecated reference code**. Read it for patterns (e.g. `data_source_session.cpp`, `point_series_xy.cpp`) but do NOT link against it from PJ4.

---

## 6. Milestones to execute

Each milestone ends in a buildable, runnable, demo-able state. Stage on a feature branch off `development`, e.g. `feature/m2-plot-canvas-skeleton`. Commit per logical unit (one commit per ported class is fine; conventional-commit messages: `feat:`, `refactor:`, `test:`, `fix:`). NEVER auto-merge to `development` — ask the user for approval first.

### M2 — Plot canvas skeleton

**Goal:** replace placeholder `DockWidget` content with a real Qwt plot using ported PJ3 base classes. Local pan/zoom/magnify works; no curves yet.

**Deliverables (port PJ3 → PJ4 with style adaptation; rebind data paths):**
- `pj_plot_widgets/{include/pj_plot_widgets,src}/PlotWidgetBase.{h,cpp}` — port from PJ3 `plotjuggler_base/{include/PlotJuggler,src}/plotwidget_base.{h,cpp}`. Strip `PlotData*` references; expose virtual hook for curve creation.
- `pj_plot_widgets/.../PlotWidget.{h,cpp}` — port from PJ3 `plotjuggler_app/plotwidget.{h,cpp}`. Constructor takes `SessionManager*` + `CatalogModel*` (NOT `PlotDataMapRef&`). Curve creation routes through `DatastoreCurveAdapter`. Each `QwtPlotCurve` gets `setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true)` + `setPaintAttribute(QwtPlotCurve::ClipPolygons, true)`. Defer color-editor / transform-dialog / statistics-dialog wiring (those classes are deferred).
- `pj_plot_widgets/.../PlotZoomer.{h,cpp}` — port from `plotjuggler_base/src/plotzoomer.{h,cpp}`. Pure Qwt extension.
- `pj_plot_widgets/.../PlotPanner.{h,cpp}` — port from `plotjuggler_base/src/plotpanner.{h,cpp}`.
- `pj_plot_widgets/.../PlotMagnifier.{h,cpp}` — port from `plotjuggler_base/src/plotmagnifier.{h,cpp}`.
- `pj_plot_widgets/.../PlotLegend.{h,cpp}` — port from `plotjuggler_base/src/plotlegend.{h,cpp}`.
- `pj_plot_widgets/include/pj_plot_widgets/DockWidget.h` + `src/DockWidget.cpp` — modify existing stub. Replace placeholder grey frame with `PlotWidget*` ownership. `IDataWidget::onTrackerTime(double)` forwards to the embedded plot.
- `pj_plot_widgets/include/pj_plot_widgets/PlotDocker.h` + `src/PlotDocker.cpp` — modify. Add Qt signal `void plotWidgetAdded(PlotWidget*);` and emit it whenever a new dock's plot is created (initial creation + splits). Existing `dockAdded` stays.

**Build adjustments:** `pj_plot_widgets/CMakeLists.txt` — add the new sources, link `plotjuggler_qwt`. `AUTOMOC ON`, `AUTOUIC ON`, `AUTORCC ON` (already set).

**Out of scope for M2:** `CurveTracker`, drag-drop wiring, color editor, transforms, sync zoom. Those are M5/M6.

**Verification:** `./run.sh` → app opens with an empty Qwt canvas in each dock. Wheel zoom changes axes. Left-drag rectangle zoom works. Middle-mouse-drag pans. Click split-horizontal/-vertical button on the dock toolbar → another real plot appears.

### M3 — Drag-drop curves end-to-end

**Goal:** drag curve names from the existing side panel onto plots and see a real time-series rendered against the datastore. XY plot creation via right-drag of two curves.

**Deliverables:**
- `pj_plot_widgets/.../PointSeriesXY.{h,cpp}` — port from PJ3 `plotjuggler_app/point_series_xy.{h,cpp}`. **Own `QwtSeriesData<QPointF>` subclass with an alignment index** (do NOT compose from two `DatastoreCurveAdapter`s — see hard rule 10). Same-topic fast path: pair by row index. Different-topic: two-pointer scan by raw `Timestamp`, exact matches only. `sample(i)` dereferences a pre-built `PairSlot{x_chunk, x_row, y_chunk, y_row}`. Invalidate alignment index on `topicsCommitted` for either source topic.
- Drag-drop accept on `PlotWidgetBase` (Qt event forwarding through `dragEnterEvent`/`dropEvent` signals from the canvas). MIME formats already match: `"curveslist/add_curve"` and `"curveslist/new_XY_axis"`. Decode via `QDataStream` of `QString` curve names. For each name, look up `CatalogModel::curveDescriptor(name)`; if found, create a `DatastoreCurveAdapter` and attach as a `QwtPlotCurve`. Reject silently with a status-bar toast for unknown names. Reject right-drag on a non-empty plot.
- Plot color cycle for newly added curves (PJ3-equivalent). A small constexpr `std::array<QColor, N>` of stable colors, indexed by the next-curve counter on the plot.
- `PlotWidget::isXYPlot()` — true when current mode is XY. Used by M5 sync-zoom to exclude XY plots.

**Verification:** load a CSV with ≥3 numeric columns via the marketplace CSV plugin. Side panel populates with `topic/field/subfield` names. Drag one onto plot 1 → curve renders. Drag two more onto plot 2 → both render. Right-drag two onto plot 3 → XY scatter. Right-drag one or three → rejected. Drag an unknown name (e.g. after removing it) → rejected.

### M4 — Curve color via context menu

**Goal:** right-click curve → "Change color…" → `QColorDialog` → curve repaints. Plot XML state round-trip preserves color.

**Deliverables:**
- Context menu on `PlotWidget` (right-click on a curve in the legend or canvas). One action: "Change color…".
- Action handler opens `QColorDialog::getColor(currentColor, this, "Pick curve color")`. On accept, calls `PlotWidget::onChangeCurveColor(name, color)` which updates the `QwtPlotCurve` pen and calls `replot()`.
- `xmlSaveState(QDomDocument&) → QDomElement` and `xmlLoadState(QDomElement&) → bool` — port from PJ3 `plotwidget.cpp`. Cover: curve names, colors, X/Y ranges, mode (time-series vs XY), per-curve line width. Round-trip must be in-memory (no file persistence for v1).

**Out of scope for M4:** the full PJ3 `PlotWidgetEditor` dialog (line width, Y limits, style toggles, multi-curve selection). Deferred to next phase per user direction.

**Verification:** right-click a curve → Change color → pick magenta → curve repaints. Save plot state via `xmlSaveState` → drop the curve → reload via `xmlLoadState` → color is still magenta.

### M5 — Synchronized X-axis zoom

**Goal:** PJ3-style global Link toggle. When on, zooming any non-XY plot's X-axis broadcasts to peers.

**Deliverables:**
- `MainWindow.ui`: add `QToolButton buttonLink` (checkable, PJ3 object name preserved). State persisted to `QSettings` under key `"MainWindow.buttonLink"`.
- `PlotWidget::rectChanged(PlotWidget* modified, QRectF rect)` signal. Emit on user-driven zoom/pan/magnify and from `setZoomRectangle(rect, /*emit=*/true)`. **Do NOT emit** on `setZoomRectangle(rect, /*emit=*/false)` (the broadcast path) or for XY plots. **This is the echo-loop avoidance** — get it wrong and you'll infinite-recurse.
- `MainWindow` slots: `onPlotTabAdded(PlotDocker*)`, `onPlotAdded(PlotWidget*)`, `onPlotZoomChanged(PlotWidget*, QRectF)`. Wire in this order:
  - `tabbedPlotWidget::tabAdded → onPlotTabAdded`
  - `docker::plotWidgetAdded → onPlotAdded`
  - `plot::rectChanged → onPlotZoomChanged`
- Broadcast algorithm in `onPlotZoomChanged`: skip if `buttonLink` unchecked. For each peer plot in all tabs: skip self, skip empty, skip XY, skip `!isZoomLinkEnabled()`. Get peer's current rect, replace only `left`/`right` with `new_range.left/right`, call `peer->setZoomRectangle(rect, /*emit=*/false)` and `peer->replot()`. PJ3 also calls `on_zoomOutVertical_triggered(false)` after the broadcast — port that intact.

**Verification:** load curves into two plots. Link off → independent zoom. Link on → X follows; each plot keeps its own Y. Add an XY plot → zoom non-XY → XY unaffected; zoom XY → others unaffected.

### M6 — Tracker integration + dock toolbar polish

**Goal:** vertical tracker line moves in lockstep with the global timeline; dragging a tracker in any plot drives the timeline. Dock toolbar matches PJ3 layout.

**Deliverables:**
- `pj_plot_widgets/.../CurveTracker.{h,cpp}` — port from PJ3 `plotjuggler_app/curve_tracker.{h,cpp}`. Crosshair markers + value labels at a given X. Owned by `PlotWidget`.
- `PlotWidget::setTrackerPosition(double display_time_sec)` — moves the tracker, updates value labels.
- `DockWidget::onTrackerTime(double t)` (already overrides `IDataWidget`) → forwards to `plot_widget_->setTrackerPosition(t)`.
- `MainWindow` wires `PlaybackEngine::currentTimeChanged` → broadcast `onTrackerTime` to every dock (walk the tab/dock tree once per signal).
- Reverse direction: `PlotWidget::trackerMoved(QPointF)` signal, emitted on user-driven tracker drag. `MainWindow` slot calls `session_->playbackEngine().setCurrentTime(p.x())`. The broadcast then echoes back to all plots (including the source) — this loop is intentional and converges.
- XY plots show no tracker line in v1 (no time axis to track on).
- `pj_plot_widgets/include/pj_plot_widgets/DockToolbar.h` + `src/DockToolbar.cpp` + `src/DockToolbar.ui` — extend existing stub to PJ3 layout: split-horizontal, split-vertical, fullscreen, close buttons. Preserve PJ3 object names (`buttonSplitHorizontal`, `buttonSplitVertical`, `buttonFullscreen`, `buttonClose`).
- Extend XML round-trip from M4 to cover: tracker enable, line width, XY mode, plot title.

**Verification:** play timeline → tracker line moves in every non-XY plot, value labels update at the tracker. Drag tracker in a plot → timeline jumps to match. Click split/fullscreen/close on the dock toolbar → behavior matches PJ3. Save plot state with non-default tracker enable + line width → restore → settings preserved.

---

## 7. Branch and commit strategy

- One feature branch per milestone, e.g. `feature/m2-plot-canvas-skeleton`, `feature/m3-drag-drop`, etc.
- Branch off `development`. Commit per logical unit (often per ported class). Conventional-commit messages.
- Trailer on every commit:
  ```
  Co-Authored-By: Codex (gpt-5.x) <noreply@openai.com>
  ```
- After completing each milestone, run `./build.sh` (must be clean) and `cd build && ctest --output-on-failure` (existing tests must still pass; add new tests where the milestone calls for them).
- Do NOT auto-merge. Surface the diff and milestone summary, ask the user for go/no-go.
- Do NOT use `--no-verify` to skip pre-commit hooks (clang-format, etc.). If a hook reformats your code, restage and retry.

---

## 8. STOP-and-ask conditions

If you encounter any of these, **STOP and surface the issue rather than guessing**:

- A `plotjuggler_core/` API method you need does not exist (e.g. you find yourself wanting `removeDataset`, per-column min/max stats, a `dataCleared` signal). Hard rule 1: ask before extending the submodule.
- A milestone deliverable conflicts with the plan or with an earlier decision in §3.
- A PJ3 file you intended to port has a behavior whose PJ4 equivalent isn't obvious (e.g. depends on `TransformsMap` for math curves, depends on `PlotBackground` for colored zones — both are deferred per §3).
- A milestone's verification step can't run (e.g. M3 verification needs the marketplace CSV plugin installed; if it isn't installed, surface the gap rather than half-testing).
- The Qwt vendored target name doesn't link as expected (CMake target is `plotjuggler_qwt`).
- An existing test breaks and the cause isn't obvious — better to surface a regression than to silently "fix" the test to match new behavior.

---

## 9. Reference patterns to imitate

For consistent style, reuse these as templates:

- **Pull-through `QwtSeriesData` adapter**: `pj_plot_widgets/src/DatastoreCurveAdapter.cpp` (already in tree). Same general shape for `PointSeriesXY`.
- **Qt-side service wrapping a Qt-optional substrate type**: `pj_app_core/src/SessionManager.cpp` (wraps `PJ::DataEngine`, emits `topicsCommitted`).
- **PIMPL with `std::deque` for stable references**: `plotjuggler_core/pj_datastore/include/pj_datastore/topic_storage.hpp`.
- **C-ABI plugin host adapter**: `pj_app/src/FileLoader.cpp` (RuntimeHost vtable + DatastoreSourceWriteHost binding).
- **Conventional-commit messages with design rationale**: `git log` on `development` from `441ea07` onwards.

---

## 10. End-state

When all six milestones land on `development`, the four demo features run end-to-end against real CSV data:

1. Drag-drop time-series curves from the side panel onto plots; right-drag two onto an empty plot for an XY scatter.
2. Right-click a curve → "Change color…" → `QColorDialog` → curve repaints; survives plot XML save/load.
3. Toggle Link → zoom one plot's X-axis; peers follow (XY plots excluded).
4. Play the timeline → tracker line moves in lockstep across plots; drag the tracker → timeline jumps.

Plus splittable docks via the toolbar, multiple tabs (existing), and the marketplace CSV ingest path.

The deferred items (`PlotWidgetEditor`, `PlotWidgetTransforms`, `StatisticsDialog`, `PlotBackground` + colormap family) are tracked in the plan §3.5 for the next phase.
