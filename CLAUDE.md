# PlotJuggler 4 (PJ4)

## Goal

Build **PlotJuggler 4** from scratch as a modern desktop application that reaches parity-plus with PlotJuggler 3.x.

This is a greenfield app repo. It is not a refactor of PJ3. Code is cherry-picked from PJ3 and rebuilt on top of the `plotjuggler_sdk` foundation.

## License

PJ4 is **MPL-2.0** (see [`LICENSE`](./LICENSE)); every source file carries an
`// SPDX-License-Identifier: MPL-2.0` header. New source files must include that
header. The `plotjuggler_sdk` submodule is a separate repo with its own license
(Apache-2.0 for `pj_base`/`pj_plugins`) — do not relicense it from here.

## Architecture

Full implementation plan: [`PJ4_PLAN.md`](./PJ4_PLAN.md). That document is the source of truth for module boundaries, delivery phases, and architectural decisions — read it before proposing structural changes.

Top-level layout (monorepo, per plan §0 and §5):

```
PJ4/
├── 3rdparty/                # vendored CMake dependencies only
├── plotjuggler_sdk/         # git submodule — Level 0 plugin SDK (pj_base / pj_plugins)
├── pj_datastore/            # Level 0 columnar store + ObjectStore + DerivedEngine (moved out of the submodule)
├── pj_scene2D/               # 2D scene module: core logic, Qt widgets, demos, tests
├── pj_marketplace/          # extension install/manage
├── pj_dialog_host/          # Qt host for plugin-provided dialogs
├── pj_scripting/            # Lua today, Python pluggable later (not yet created)
├── pj_runtime/              # services layer (Qt allowed, no Qt6::Widgets link)
├── pj_widgets/              # reusable Qt widgets and UI helpers
├── pj_plotting/             # Qwt plotting module: core adapters, Qt widgets, tests
├── pj_3d_widgets/           # QRhi 3D (post-v1; not yet created)
├── pj_app/                  # main window shell
├── resources/               # SVG icons (ported from PJ3) + resources.qrc
└── PJ4_PLAN.md
```

The widget families (`pj_plotting`, `pj_scene2D/widgets` via the `pj_scene2d_widgets` target, and future `pj_3d_widgets`) never depend on each other. Shared reusable Qt controls/helpers live in `pj_widgets`; shared runtime state flows through the `IDataWidget` contract exposed by `pj_runtime`.

### Placement rules

When adding files, use the owning module rather than creating new top-level folders. If the requested location does not match these boundaries, ask before proceeding and suggest the closest fit.

- `plotjuggler_sdk/`: read-only submodule — the plugin **SDK** (`pj_base`, `pj_plugins`). Canonical object schemas (`Image`, `DepthImage`, `ImageAnnotations`, `PointCloud`, `FrameTransforms`) and their codecs live under `pj_base/builtin/`. Change `plotjuggler_sdk` only when explicitly working in that submodule.
- `pj_datastore/`: Level 0 columnar storage engine — `DataEngine` + `ObjectStore` + `DerivedEngine` + the host-side C-ABI write bridges. App-internal (plugins never link it; they reach storage through the `pj_base` C ABI). Was previously inside the `plotjuggler_sdk` submodule. Pure C++20, no Qt; depends only on `pj_base`. Logic in `src/`, public headers in `include/pj_datastore/`, tests in `tests/`, docs in `docs/`. Licensed MPL-2.0.
- `pj_runtime/`: app runtime services and contracts: session/data lifecycle, catalog, playback, extension catalog, future workspace/transform/toolbox services. No concrete widgets and no `Qt6::Widgets` link.
- `pj_app/`: executable shell only: `MainWindow`, menus/toolbars/status bar, app dialogs, and wiring between runtime services and concrete widgets. Do not put reusable controls or business logic here.
- `pj_widgets/`: reusable Qt widgets and UI helpers that could be used by another Qt app. Depends only on Qt and the C++ standard library; no dependencies on `pj_runtime`, `pj_app`, or other PJ modules.
- `pj_plotting/`: Qwt plotting feature family. Put datastore adapters and plotting logic in `core/`, Qt/Qwt widgets in `widget/`, and focused tests in `tests/`.
- `pj_scene2D/`: 2D media/scene feature family. Put independent media logic in `core/`, Qt viewer widgets in `widgets/`, runnable examples in `demos/`, and tests in `tests/`.
- `pj_marketplace/`: extension registry, download, install/manage services, and marketplace UI.
- `pj_dialog_host/`: Qt host/binding for plugin-provided dialogs. General app dialogs stay in `pj_app`; reusable dialog controls stay in `pj_widgets`.
- `pj_scripting/`: future language-agnostic scripting engine. Do not place scripting code under `pj_app` or widget modules unless it is strictly UI/editor code.
- `pj_3d_widgets/`: future 3D visualization widget family. Do not add 3D code elsewhere unless the plan explicitly names a shared lower-level module.
- `resources/`: shared app resources registered in `resources.qrc`; module-local test/demo assets should live with that module.
- `3rdparty/`: vendored source dependencies added via CMake `add_subdirectory`. Conan/system dependencies do not belong here.

## Documentation

Each PJ4 module owns its intent docs. An agent landing in the repo reads root `CLAUDE.md` → `PJ4_PLAN.md` → per-module `CLAUDE.md` → per-module `docs/` → code. If any link in that chain is missing or stale, treat it as a documentation bug, not a code bug.

### Code doc-comments

Every class, struct, function, and method — and any member that is not self-explanatory — carries a **concise doc-comment**, especially in **header files** (the API surface a reader meets first).

"Concise" is not "minimal": convey everything the reader needs and nothing they don't. Above all, document what the **name cannot convey** — pitfalls, side-effects, ownership/lifetime, threading or call-order constraints, units, and non-obvious invariants or rationale. Skip the genuinely self-evident (a trivial getter/setter, an obvious field): restating the signature in prose is just noise. **Comment the surprise, not the obvious.**

### Per-module documentation contract

Every PJ4 module (anything matching `pj_*/`) owns:

- `<module>/CLAUDE.md` — one-paragraph purpose + pointer to the module's `docs/` and key headers. The entry point for an agent landing in that subtree.
- `<module>/docs/` — module-local intent docs. Minimum content depends on module complexity:
  - **Trivial / structurally obvious** (e.g. `pj_widgets`, `pj_dialog_host`, `pj_app`): CLAUDE.md only; no `docs/` required.
  - **Standard module**: `docs/REQUIREMENTS.md` (the WHAT).
  - **Module with non-obvious internals**: add `docs/ARCHITECTURE.md` (the HOW). See `pj_scene2D/docs/` for the reference shape.

Cross-cutting docs (porting strategy, glossary, ADRs) live in top-level `docs/`. Its scope is described in [`docs/README.md`](./docs/README.md).

### Module documentation index

| Module | Entry point | Docs |
|---|---|---|
| `pj_app` | [pj_app/CLAUDE.md](./pj_app/CLAUDE.md) | — |
| `pj_runtime` | [pj_runtime/CLAUDE.md](./pj_runtime/CLAUDE.md) | — |
| `pj_widgets` | [pj_widgets/CLAUDE.md](./pj_widgets/CLAUDE.md) | — |
| `pj_plotting` | [pj_plotting/CLAUDE.md](./pj_plotting/CLAUDE.md) | — |
| `pj_dialog_host` | [pj_dialog_host/CLAUDE.md](./pj_dialog_host/CLAUDE.md) | — |
| `pj_scene2D` | [pj_scene2D/CLAUDE.md](./pj_scene2D/CLAUDE.md) | [docs/](./pj_scene2D/docs/) — REQUIREMENTS, ARCHITECTURE, TECHNICAL_NOTES, datatypes_2D, … |
| `pj_marketplace` | [pj_marketplace/README.md](./pj_marketplace/README.md) | [docs/](./pj_marketplace/docs/) — REQUIREMENTS, ARCHITECTURE, USER_MANUAL, marketplace-spec |
| `pj_scene3D` | — (WIP, not yet tracked) | WIP design spec lives at `docs/superpowers/specs/2026-05-15-pj-scene3d-design.md` (also untracked); will migrate into `pj_scene3D/docs/` when the module stabilizes |
| `pj_datastore` | [pj_datastore/CLAUDE.md](./pj_datastore/CLAUDE.md) | [docs/](./pj_datastore/docs/) — REQUIREMENTS, ARCHITECTURE, USER_GUIDE, OBJECT_STORE_DESIGN |
| `plotjuggler_sdk/` (submodule) | [plotjuggler_sdk/CLAUDE.md](./plotjuggler_sdk/CLAUDE.md) | submodule owns its own `docs/` tree |

### Freshness discipline

Before any commit that changes behavior, public APIs, ABI structs, module ownership, or user-facing semantics: verify that the relevant `CLAUDE.md` / `docs/` files still match reality. If they don't, either update them in the same change or ask whether to update before committing. Do not commit known-stale docs.

## Key sources

### `plotjuggler_sdk/` (submodule) — the plugin SDK

The SDK libraries live in the submodule at `./plotjuggler_sdk/`:

- `pj_base` — vocabulary types + canonical object schemas (`pj_base/builtin/Image.hpp`, `DepthImage.hpp`, `ImageAnnotations.hpp`, `PointCloud.hpp`, `FrameTransforms.hpp`) and their codecs. SDK boundary for plugin authors producing or consuming canonical objects.
- `pj_plugins` — ABI + runtime for extensions

These are consumed as-is. Changes to `plotjuggler_sdk` happen in that repo, not here.

Initialize / update the submodule with:

```
git submodule update --init --recursive
```

### `pj_datastore/` (top-level module — moved out of the submodule)

The columnar storage engine is an app-internal Level 0 module, not part of the plugin SDK
(plugins reach storage only through the `pj_base` C ABI). It builds from source via
`add_subdirectory(pj_datastore)` in the root `CMakeLists.txt`, immediately after the submodule so
the `pj_base` / `pj_internal_fmt` targets it links already exist.

- `pj_datastore` — columnar store (`DataEngine`) + `ObjectStore` + `DerivedEngine` + the host-side
  C-ABI write bridges (`plugin_data_host.hpp`). Public headers under `include/pj_datastore/`.
- Conan deps it needs (`nanoarrow`, `tsl-robin-map`, `benchmark`) are declared in the root
  `conanfile.txt`; its CMake also performs the nanoarrow/IPC target detection (relocated from the
  submodule when the engine moved).

### `~/ws_plotjuggler/PlotJuggler/` (PJ3 reference — read-only)

PlotJuggler 3 source tree. This is **the primary source for cherry-picked code**. Expect heavy reference, particularly for:

- `plotjuggler_app/` — `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, zoomers, `AxisTimeOffset`, tracker, drag-drop, per-curve display transform UI, Lua engine
- `plotjuggler_base/` — shared base types (cross-check against `pj_base`)
- `plotjuggler_plugins/` — historical plugins (already ported; reference only)

Treat the PJ3 tree as read-only. Do not modify it from this repo.

The "wholesale lift" strategy for plot widgets (plan §5.3, §8) means porting files largely intact, then rebinding data reads (`PlotDataMapRef` → `DatastoreCurveAdapter` against `pj_datastore::DataReader`). Do not rebuild plot widgets from scratch.

## Build

- **Qt 6.8** (required).
- **CMake + Conan**. CMake is the build driver; Conan provides external non-vendored dependencies.
- **C++20**.
- **Linux-only** for v1. The code **must stay portable** — no Linux-only APIs or POSIX-specific paths in module code; gate anything platform-specific behind the usual CMake / `#ifdef` guards so a future macOS/Windows build is a build-system problem, not a code problem.

### Vendored third-party

Mirror PJ3's `3rdparty/` convention. Vendored deps live at `./3rdparty/<name>/` and are added via `add_subdirectory` from the top-level `CMakeLists.txt`.

Explicitly vendored (do not take from Conan or system packages):

- **Qwt** — required for Qt 6.8 compatibility and for parity with PJ3 plot widgets.
- **Qt-Advanced-Docking-System** — docking framework used by `pj_app`.

Other PJ3-style vendorables (`QCodeEditor`, `sol2`, `color_widgets`, `date`) will be vendored on the same pattern as we pull in the modules that need them — decide per-case when each module lands.

Everything else (GLM, assimp, FFmpeg, Lua runtime, etc.) comes from Conan — including
**backward-cpp** (consumed via `backward-cpp/1.6`, not vendored): `pj_app` links
`Backward::Backward` and installs a `backward::SignalHandling` crash handler in `main.cpp`
for symbolized stack traces. Its default `dw` backend pulls `elfutils` transitively from
Conan, so rich traces need no system `-dev` packages.

### Compile instructions

One-time setup (installs Qt 6.8.3 into `./.qt/`, ~1GB):

```bash
aqt install-qt linux desktop 6.8.3 linux_gcc_64 \
    --modules qtcharts qtwebsockets \
    --outputdir ./.qt
```

Build (configures Conan, runs CMake, builds):

```bash
./build.sh
```

That script:

1. Checks for `.qt/6.8.3/gcc_64/` and errors with the install command if missing.
2. Runs `conan install ... --output-folder=build --build=missing -s compiler.cppstd=20` (reads `conanfile.txt`).
3. Configures CMake with `CMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake` and `CMAKE_PREFIX_PATH=./.qt/6.8.3/gcc_64`.
4. Builds with `cmake --build build -j$(nproc)`.

Run the app:

```bash
./run.sh
```

`run.sh` unsets `QT_IM_MODULE` before launching. Otherwise the IBus platform input context gets loaded from a system / older Qt install and segfaults under the Qt 6.8.3 runtime.

Re-running `./build.sh` after code changes does incremental builds. `ccache` is picked up automatically if installed.

Submodule: `git submodule update --init --recursive` on first clone.

## UI conventions

- **Prefer `.ui` files over programmatic widget construction.** Widgets, layouts, menus, toolbars, dialogs — build them in Qt Designer (`.ui`) and load via `uic`. Use `AUTOUIC` in the module's `CMakeLists.txt`. Drop to hand-written `QWidget` subclasses only when the construction is genuinely dynamic (e.g. widgets created at runtime from plugin metadata) or when I explicitly ask for it.

## v1 scope (per plan §0)

Parity-plus with PJ3: file + streaming sources, 11 built-in transforms, undo/redo, derived-series editor (incl. Lua via `pj_scripting`), reactive scripts (via Toolbox + `onTimeChanged`), multi-tab workspace, marketplace install UI, all toolboxes.

`pj_3d_widgets` is contract-reserved in v1 — full implementation is post-v1 (~6–7 weeks extra per plan §5.5).

## Non-goals (explicitly deferred)

- `StatePublisher` parity
- Exact 3.x UI/terminology parity
- Hot reload of running extension instances
- Full backward compatibility with 3.x layout files
- Recreating the removed prototype app as the final app
- Windows runtime packaging / deployment (windeployqt + bundling the FFmpeg shared DLLs that `pj_app` transitively requires via `pj_scene2d_core`) — deferred until Windows is a supported release target. Today Windows CI is a non-blocking portability tracker and resolves these DLLs via `PATH` at test time only (see `.github/workflows/windows-ci.yml`).

## Workflow notes

- Architectural questions → consult `PJ4_PLAN.md` first; escalate if the plan is silent or contradictory.
- New modules must respect the dependency rules in plan §5 (widget families are siblings; `pj_runtime` has no concrete widget implementation and does not link `Qt6::Widgets`).

### Commit policy

- **Never commit autonomously.** Surface the diff, then ask for approval. Commit only after explicit user confirmation in that turn.

### Porting policy from PJ3

- **Default: port, don't rewrite.** For every UI element, widget, or helper we need, check `~/ws_plotjuggler/PlotJuggler/` first. If PJ3 has something that works, port it. Greenfield rewrites need a real reason.
- **Style changes are expected; widget names are not.** When porting, adapt file/class names and member conventions to plotjuggler_sdk style (`PascalCase.{h,cpp}`, `PJ::` namespace, `trailing_underscore_` members, Google C++ / 2-space / 120-col). But **preserve the `objectName` of widgets inside `.ui` files verbatim** (e.g. `buttonLoadDatafile`, `frameFile`, `checkBoxAddPrefix`, `displayTime`, `playbackLoop`, `streamingSpinBox`) so existing layout files, stylesheet selectors, and user muscle memory keep working — unless I explicitly ask you to rename one.
- **Rebind data paths.** PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into `pj_runtime` services (`CatalogModel`, `SessionManager`, `PlaybackEngine`, `TransformRegistry`). That's the one systematic rewrite.
- **Proactively surface improvement opportunities.** If you see a chance to improve separation of concerns, reusability, testability, or remove duplication while you're porting — flag it and **ask for approval before changing**. Don't silently refactor, and don't silently skip obvious wins. The bar is: "is there a cleaner shape that we'd regret not taking?" If yes, ask.
- **Don't fix what isn't broken.** Code that already reads cleanly and does the right thing gets ported close to verbatim (modulo style). Save the refactor energy for real problems.
