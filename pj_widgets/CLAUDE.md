# pj_widgets

## Purpose

Reusable Qt widgets and UI helpers — the kind of controls that could be dropped into another Qt application without dragging PJ4-specific state with them. Static library, `Qt6::Core` / `Gui` / `Svg` / `Widgets` only.

## Hard constraints (from root CLAUDE.md)

- Depends only on **Qt and the C++ standard library**.
- **No dependencies on `pj_runtime`, `pj_app`, or any other PJ module.** Anything that needs runtime state belongs upstream of here.
- If a widget is specific to PlotJuggler workflows (e.g. plot-specific dockers), it belongs in the widget-family module instead.

## Inventory

Widgets:

| Header | Role |
|---|---|
| `Dialog.h` + `Dialog.ui` | Chrome wrapper used by app dialogs and `FileDialog`. Source of `ChromeMetrics` defaults `{24, 2, 2, 2}`. |
| `FileDialog.h` | `QFileDialog` wrapped in `PJ::Dialog` chrome. |
| `MessageBox.h` | Themed `QMessageBox` replacement. |
| `RecentFilesMenu.h` | `QMenu` populated from a persistent recent-files list. |
| `ColorPickerPopup.h` | Compact popup color picker. |
| `ToggleSwitch.h` | iOS-style toggle replacement for `QCheckBox`. |
| `IntScrubber.h` / `DoubleScrubber.h` / `ScrubberBase.h` | Drag-to-scrub numeric inputs. |
| `RealSlider.h` | Floating-point `QSlider`. |
| `FlowLayout.h` | Standard Qt example flow layout. |
| `ElidingLabel.h` | `QLabel` that elides instead of clipping. |
| `CurveTreeView.h` | Tree view tuned for PlotJuggler's curve catalog (no runtime coupling — model is plugged in by the caller). |
| `VisualizationPlaceholderWidget.h` | Placeholder shown when no data widget is bound. |

Helpers (header-only or small):

| Header | Role |
|---|---|
| `ChromeMetrics.h` | Shared spacing constants for dialog chrome. |
| `Style.h` | Common style accessors. |
| `SvgUtil.h` | Helpers to load and recolor SVG resources. |

## UI convention

Per root CLAUDE.md: **prefer `.ui` files**. `AUTOUIC` is on. Search path is `src/`. The only `.ui` today is `Dialog.ui` — keep new chromed dialogs on the same pattern.

## Tests

`tests/curve_tree_view_test.cpp`. Add a focused gtest binary per widget when behavior is non-trivial.

`pj_widgets` has no `docs/` folder — each widget's intent fits in its header doc-comment.
