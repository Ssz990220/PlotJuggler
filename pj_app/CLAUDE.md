# pj_app

## Purpose

The executable shell. Owns `MainWindow`, the application menus/toolbars/status bar, the `AppSession` (from `pj_runtime`), and the wiring between runtime services and concrete widgets (`pj_plotting`, `pj_scene2D/widgets`, …). Nothing else.

## What belongs here

- `MainWindow` and its `.ui` file.
- App-level dialogs (`PreferencesDialog`, `DiagnosticsDetailDialog`, etc.) and their navigation rows.
- The shell's left panel, curve list, file loader, theme manager, title bar.
- Glue code that constructs an `AppSession`, then wires it to docked widgets.
- `main.cpp`.

## What does NOT belong here

- **Reusable Qt controls** → `pj_widgets`.
- **Business logic / data services** → `pj_runtime`.
- **Plotting / 2D / 3D widgets** → the owning widget-family module.
- **Plugin dialog runtime** → `pj_dialog_host`.

If you're tempted to add a class here that could be reused by another Qt app, move it to `pj_widgets` instead.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files** over programmatic widget construction. `pj_app/CMakeLists.txt` uses `AUTOUIC`. Drop to hand-written `QWidget` subclasses only for genuinely dynamic construction (plugin-driven widgets) or when explicitly requested.

## Layout

- `src/` — top-level shell sources and root `.ui` files (`MainWindow.ui`, `PreferencesDialog.ui`, `TitleBar.ui`).
- `src/ui/` — embedded shell sub-widgets: left-panel widgets (`CurveListPanel`, `LeftPanel`, `DiagnosticsCard`, `DiagnosticsPopup`, `DiagnosticsDetailDialog`), the bottom timeline strip (`TimelineWidget`), and the right-sidepanel scene config panels (`Scene2DConfigPanel`, `Scene3DConfigPanel`).

`pj_app` has no `docs/` folder by design — the shell's intent is "wire the services to the widgets," and the wiring is best read directly from `MainWindow.cpp` and `main.cpp`.
