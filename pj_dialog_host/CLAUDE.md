# pj_dialog_host

## Purpose

Qt host for **plugin-provided dialogs**. Plugins (DataSource, MessageParser, Toolbox, Dialog) describe their UIs declaratively via the dialog protocol in `plotjuggler_sdk/pj_plugins`; this module turns those descriptions into actual `QWidget` trees and drives the typed event loop back to the plugin.

Single static-library target: `pj_dialog_engine_qt`. Links `Qt6::Widgets`, `Qt6::UiTools`, `Qt6::Charts`, `Qt6::SvgWidgets`.

## What belongs here

- The dialog engine that interprets the SDK's `WidgetData` graph and instantiates Qt widgets.
- Widget bindings (the per-widget glue that maps SDK events ↔ `QObject` signals).
- Specialty embedded widgets that only make sense inside plugin dialogs (e.g. `chart_preview_widget`, `drop_event_filter`).
- Syntax-highlighter implementations for embedded code editors (Lua, Python).

## What does NOT belong here

- **General app dialogs** (Preferences, Diagnostics, etc.) → `pj_app`.
- **Reusable dialog chrome and controls** (`Dialog`, `FileDialog`, `MessageBox`, scrubbers, …) → `pj_widgets`.
- **The dialog protocol / SDK** itself → `plotjuggler_sdk/pj_plugins` (this module is the host, not the contract).

## Public surface

Public headers live under `include/pj_plugins/host_qt/` — the namespace mirrors `pj_plugins` because this is conceptually a Qt-side companion to that SDK family.

| Header | Role |
|---|---|
| `dialog_engine.hpp` | Top-level engine: takes a `WidgetData` tree, returns a constructed `QWidget`, drives events. |
| `panel_engine.hpp` | Hosts a long-lived interactive panel built from a plugin's typed-dialog UI. Sibling of `dialog_engine.hpp`: same .ui loader/binding/tick-and-diff, but returns a bare `QWidget*` via `openPanel()` (no modal `exec()`) and is closed by plugin-initiated `requestClose("<reason>")`. |
| `widget_binding.hpp` | Per-widget event/data binding plumbing. |
| `pj_ui_loader.hpp` | `QUiLoader` subclass (`PjUiLoader`) that teaches `QUiLoader` to instantiate host-provided custom widgets (RangeSlider, DateRangePicker, CredentialsEditor) from plugin `.ui` files; shared by both the dialog and panel engines. |
| `chart_preview_widget.hpp` | Embedded chart widget used by toolboxes (e.g. FFT preview). |
| `drop_event_filter.hpp` | Event filter that turns Qt drops into SDK drag-drop events. |

## Tests

Three test executables: `tests/dialog_engine_test.cpp`, `tests/panel_engine_test.cpp` (with `tests/mock_panel_plugin.cpp`), and `tests/widget_binding_test.cpp`. Add tests when extending the protocol coverage.

`pj_dialog_host` has no `docs/` folder — the protocol itself is documented in `plotjuggler_sdk/pj_plugins/docs/dialog-plugin-guide.md`; this host just realizes that protocol in Qt.
