# pj_plotting

## Purpose

Qwt-based plotting module — the PJ4 home for time-series plots, dockers, zoomers, trackers, legends, and the curve editor. This is the **largest PJ3 wholesale lift** in the repo.

## Module layout

Two targets with a strict direction:

```
pj_plotting  ──►  pj_plotting_core  ──►  pj_datastore + pj_base
                       │
                       └──►  Qwt (vendored at 3rdparty/qwt)
```

| Subdir | Contents | Role |
|---|---|---|
| `core/` | `DatastoreCurveAdapter`, `PointSeriesXY` | Bridges PJ3-style `QwtSeriesData<QPointF>` consumers to `pj_datastore::DataReader`. No Qt Widgets. |
| `widget/` | `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, `PlotZoomer`, `PlotPanner`, `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotFocusOverlay`, `DockWidget`, `DockToolbar`, `CurveEditor` | The Qt/Qwt widgets, ported from PJ3's `plotjuggler_app/`. |
| `tests/` | gtest binaries | Adapter and dock-placeholder tests. |

## Port strategy (per root CLAUDE.md "Porting policy")

This module is the canonical example of the **wholesale lift** strategy:

1. **Lift PJ3 files close to verbatim.** Source: `~/ws_plotjuggler/PlotJuggler/plotjuggler_app/`. Class names map directly: PJ3 `PlotWidget` → PJ4 `PlotWidget`, etc.
2. **Apply plotjuggler_sdk style** on the way in: `PascalCase.{h,cpp}`, `PJ::` namespace, `trailing_underscore_` members, Google C++ / 2-space / 120-col.
3. **Preserve `.ui` `objectName` values verbatim** so stylesheets and muscle memory keep working.
4. **Rebind data reads only.** Replace PJ3's `PlotDataMapRef` / `TransformsMap` consumption with `pj_plotting::DatastoreCurveAdapter` over `pj_datastore::DataReader`. This is the only systematic rewrite — do not rewrite plotting logic from scratch.
5. **Do not rebuild from scratch.** If a class reads cleanly in PJ3, port it.

## Cross-module rules

- This module is a sibling of `pj_scene2D/widgets` and `pj_scene3D/widgets`. **They never depend on each other.**
- Shared runtime state flows through `pj_runtime::IDataWidget` (so `PlaybackEngine` can drive tracker updates without coupling to plot internals).
- Reusable Qt controls used here that could serve another widget family go in `pj_widgets`, not here.

## Known historical gotchas

See repo memory and PJ4_PLAN.md §5.3 / §8 for the full list. Two that matter most:

- **OpenGL canvas**: PJ3 had a `Preferences::use_opengl` QSettings gate; the early PJ4 port dropped it, causing software-raster rendering at 72% CPU. Restored. Do not re-drop without measuring.
- **Native window inside ADS**: do **not** propose making `QwtPlotOpenGLCanvas` a `WA_NativeWindow` inside Qt-Advanced-Docking — the native-flag conflict breaks layout.
