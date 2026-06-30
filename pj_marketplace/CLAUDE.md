# pj_marketplace

## Purpose

Extension marketplace for PlotJuggler 4 — registry fetching, package download,
and installed-extension lifecycle management, plus the marketplace UI. Three
library targets (`pj_marketplace` core, `pj_marketplace_ui`, `pj_plugin_catalog`)
and a standalone `pj_marketplace_app` harness.

## Layout

- `include/pj_marketplace/` — public headers (`marketplace.hpp`, `registry_manager.hpp`,
  `download_manager.hpp`, `extension_manager.hpp`, `installed_extension.hpp`,
  `marketplace_window.hpp`, `extension_detail_dialog.hpp`, `platform_utils.hpp`,
  `qt_diagnostic_bridge.hpp`).
- `src/core/` — registry/download/extension managers; `src/ui/` — `MarketplaceWindow`
  + `ExtensionDetailDialog` (`.ui`-driven); `tests/` — one gtest binary per manager.

Consumed by the app through `pj_runtime`'s `ExtensionCatalogService`.

## Read deeper

| For | Read |
|---|---|
| Module purpose + Conan deps | [`README.md`](./README.md) |
| What it must do | [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) |
| How it works | [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) |
| End-user flow | [`docs/USER_MANUAL.md`](./docs/USER_MANUAL.md) |
| Registry/package wire format | [`docs/plotjuggler-marketplace-spec-v1.0.0-en.md`](./docs/plotjuggler-marketplace-spec-v1.0.0-en.md) |
