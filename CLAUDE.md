# PlotJuggler 4 (PJ4)

## Goal

Build **PlotJuggler 4** from scratch as a modern desktop application that reaches parity-plus with PlotJuggler 3.x.

This is a greenfield app repo. It is not a refactor of PJ3 and it does not evolve `pj_proto_app` (deprecated). Code is cherry-picked from PJ3 and rebuilt on top of the `plotjuggler_core` foundation.

## Architecture

Full implementation plan: [`PJ4_PLAN.md`](./PJ4_PLAN.md). That document is the source of truth for module boundaries, delivery phases, and architectural decisions — read it before proposing structural changes.

Top-level layout (monorepo, per plan §0 and §5):

```
PJ4/
├── plotjuggler_core/        # git submodule — Level 0 foundation
├── pj_scripting/            # Lua today, Python pluggable later (not yet created)
├── pj_app_core/             # services layer (Qt allowed, no QWidget)
├── pj_plot_widgets/         # Qwt plots (lifted from PJ3); placeholder docks in v1
├── pj_media_widgets_qt/     # QRhi 2D viewer (wraps pj_media) (not yet created)
├── pj_3d_widgets/           # QRhi 3D (post-v1; not yet created)
├── pj_app/                  # main window shell
├── resources/               # SVG icons (ported from PJ3) + resources.qrc
└── PJ4_PLAN.md
```

The three widget families (`pj_plot_widgets`, `pj_media_widgets_qt`, `pj_3d_widgets`) are **siblings** — they never depend on each other. They share only the `IDataWidget` contract exposed by `pj_app_core`.

## Key sources

### `plotjuggler_core/` (submodule)

Foundation libraries live in the submodule at `./plotjuggler_core/`:

- `pj_base` — vocabulary types
- `pj_datastore` — columnar store + `ObjectStore` + `DerivedEngine`
- `pj_plugins` — ABI + runtime for extensions
- `pj_media` — 2D/video pipeline (FFmpeg + QRhi)
- `pj_marketplace` — extension install/manage

These are consumed as-is. Changes to `plotjuggler_core` happen in that repo, not here.

Initialize / update the submodule with:

```
git submodule update --init --recursive
```

### `~/ws_plotjuggler/src/PlotJuggler/` (PJ3 reference — read-only)

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

Other PJ3-style vendorables (`QCodeEditor`, `sol2`, `color_widgets`, `backward-cpp`, `date`) will be vendored on the same pattern as we pull in the modules that need them — decide per-case when each module lands.

Everything else (GLM, assimp, FFmpeg, Lua runtime, etc.) comes from Conan.

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
- Evolving `pj_proto_app` into the final app (it is deprecated)

## Workflow notes

- Architectural questions → consult `PJ4_PLAN.md` first; escalate if the plan is silent or contradictory.
- Cherry-picking from PJ3 → copy the file, update includes, rebind `PlotDataMapRef` reads to `DataReader`, preserve attribution in commit messages.
- New modules must respect the dependency rules in plan §5 (widget families are siblings; `pj_app_core` has no `QWidget`).
