# pj_widgets

## Purpose

Reusable Qt widgets and UI helpers — the kind of controls that could be dropped into another Qt application without dragging PJ4-specific state with them. Static library, `Qt6::Core` / `Gui` / `Network` / `Svg` / `Widgets` only.

## Hard constraints (from root CLAUDE.md)

- Depends only on **Qt and the C++ standard library** (plus the dependency-free, header-only `raster_ipc/` wire-protocol contract, which is not a PJ module).
- **No dependencies on `pj_runtime`, `pj_app`, or any other PJ module.** Anything that needs runtime state belongs upstream of here.
- If a widget is specific to PlotJuggler workflows (e.g. plot-specific dockers), it belongs in the widget-family module instead.

## Inventory

Widgets:

| Header | Role |
|---|---|
| `Dialog.h` + `Dialog.ui` | Chrome wrapper used by app dialogs and `FileDialog`. |
| `FileDialog.h` | `QFileDialog` wrapped in `PJ::Dialog` chrome. |
| `MessageBox.h` | Themed `QMessageBox` replacement — frameless modal with heading, body, optional "Don't show again" checkbox, and a vertical column of N labelled buttons (primary button paints the light_purple → light_blue gradient via the `msgbox_role` dynamic property). Static helpers: `information` / `warning` / `critical` (single OK) + `question` (multi-button, returns clicked index). |
| `ProgressDialog.h` | `QProgressDialog` replacement on the `Dialog` chrome: message + bar + up to two stop buttons. Domain-neutral — reports `Action::Primary` / `Action::Secondary` (sticky, polled via `action()` or the `stopRequested` signal) and leaves their meaning to the caller. No close affordance (base ✕ hidden, Esc swallowed). |
| `RecentFilesMenu.h` | `QMenu` populated from a persistent recent-files list. |
| `ColorPickerPopup.h` | Compact popup color picker. |
| `ToggleSwitch.h` | iOS-style toggle replacement for `QCheckBox`. |
| `IntScrubber.h` / `DoubleScrubber.h` / `ScrubberBase.h` | Drag-to-scrub numeric inputs. |
| `SectionHeaderBand.h` | 24-px titlebar-tone section header strip (leading indent baked in); background themed once via the `PJ--SectionHeaderBand` QSS class rule, so instances need no per-objectName stylesheet registration. |
| `RealSlider.h` | Floating-point `QSlider`. |
| `FlowLayout.h` | Standard Qt example flow layout. |
| `ElidingLabel.h` | `QLabel` that elides instead of clipping. |
| `CurveTreeView.h` | Tree view tuned for PlotJuggler's curve catalog (no runtime coupling — model is plugged in by the caller). |
| `VisualizationPlaceholderWidget.h` | Placeholder shown when no data widget is bound. |
| `RasterStreamView.h` | Hosts an external renderer process and paints its framebuffer (streamed over shared memory via the `raster_ipc/` contract) in a panel; forwards key input through an injectable translator. Pure Qt, no window embedding. |
| `RasterFrame.h` | Decodes a `raster_ipc` shared-memory frame into a `QImage` (consumer side of the protocol). |

Helpers (header-only or small):

| Header | Role |
|---|---|
| `ChromeMetrics.h` | Shared icon/layout spacing constants for dialog chrome, broadcast from MainWindow. Defaults `{icon_size=20, icon_padding=4, layout_padding=0, layout_spacing=0}`. |
| `Style.h` | Common style accessors. |
| `SvgUtil.h` | Helpers to load and recolor SVG resources. |

## UI convention

Per root CLAUDE.md: **prefer `.ui` files**. `AUTOUIC` is on. Search path is `src/`. The only `.ui` today is `Dialog.ui` — keep new chromed dialogs on the same pattern.

## Tests

`tests/curve_tree_view_test.cpp`. Add a focused gtest binary per widget when behavior is non-trivial.

`pj_widgets` has no `docs/` folder — each widget's intent fits in its header doc-comment.
