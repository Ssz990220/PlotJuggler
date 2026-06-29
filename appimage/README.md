# PlotJuggler 4 — AppImage packaging

Builds a single relocatable `PlotJuggler-<version>-x86_64.AppImage` bundling the
`plotjuggler4` shell, its Qt 6 + Conan runtime, and (optionally) a set of plugins.

## Build

```bash
./install_qt6.sh        # once: Qt into ./.qt
./build.sh              # builds build/pj_app/plotjuggler4 + Conan runtime env

appimage/build_appimage.sh                       # app-only AppImage
appimage/build_appimage.sh --plugins-dir <path>  # bundle a local plugin folder
appimage/build_appimage.sh --plugins-registry    # bundle the official set (CI default)
```

Output lands at `appimage/PlotJuggler-<version>-x86_64.AppImage`.

## Plugins

Plugins are **not** part of this repo. They are built and published separately by
[`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) as
self-contained, per-extension marketplace zips (each = the plugin `.so` + its
`manifest.json`, with heavy dependencies static-linked), and indexed for download
by the [`pj-plugin-registry`](https://github.com/PlotJuggler/pj-plugin-registry).
Because the `.so`s are self-contained, bundling is just *copy/unpack* — no
dependency deployment.

There are two ways to bundle them (and a no-plugin default):

- **`--plugins-dir <path>`** — copy a folder you curated, verbatim, into
  `usr/lib/plotjuggler/plugins/`. Use this for local/offline builds; the folder
  must contain ready-to-run (self-contained) plugins, e.g. unpacked marketplace
  zips, each in its own subdirectory.
- **`--plugins-registry [url]`** — download the curated set (`BUNDLE_IDS` in
  `build_appimage.sh`) from the plugin registry, verify each `sha256` checksum,
  and unpack into `usr/lib/plotjuggler/plugins/<id>/`. Defaults to the registry's
  `main`; pass a URL to pin a specific ref. This is what CI uses.
- **(no flag)** — app-only AppImage. Users add plugins later via the in-app
  marketplace or `--plugin-dir`.

### Curated set (`--plugins-registry`)

The registry lists every official extension; the AppImage bundles this subset
(`BUNDLE_IDS`):

`csv-loader`, `mcap-loader`, `parquet-loader`, `dummy-streamer`,
`foxglove-bridge`, `plotjuggler-bridge`, `ros-parser`, `protobuf-parser`,
`json-parser`, `toolbox-quaternion`.

Not bundled: `ros2-stream`, `toolbox-mosaico`, `toolbox-transform-editor` (not
published to the registry — ROS 2 streaming needs a host ROS install, so it is
installed by the user, not baked in); `toolbox-colormap`,
`toolbox-reactive-scripts-editor` (excluded by request).

### Where plugins live, and why

Bundled plugins go under `usr/lib/plotjuggler/plugins/<id>/` — the FHS-correct
bucket for arch-dependent `.so` code, and the exact path the installed
`plotjuggler4` auto-discovers relative to itself (`usr/bin/plotjuggler4` →
`../lib/plotjuggler/plugins`). Kept out of `usr/plugins/` so the recursive plugin
scanner does not collide with Qt's own platform plugins that
`linuxdeploy-plugin-qt` deploys there. `AppRun.sh` therefore does **not** pass
`--plugin-dir`; that flag stays a user-facing option, forwarded verbatim if the
user supplies one. Marketplace installs land in the writable per-user extensions
dir, which the app also scans — so installing from within the AppImage works,
separately from the read-only bundle.

## CI

`.github/workflows/release.yml` builds the AppImage with `--plugins-registry` and,
on a `v*` tag, attaches it to the GitHub Release (`workflow_dispatch` builds an
artifact only). It reuses the Qt/Conan/ccache caching from `linux-ci.yml`.

## Follow-up: multi-distro ROS 2

The goal is for a **single AppImage to support multiple ROS 2 distros**
(humble / iron / jazzy / rolling). `pj-official-plugins` already builds this as
one `linux-x86_64` zip: a distro-agnostic **proxy** (`libros2_stream_plugin.so`,
links no ROS) plus per-distro inner libraries under `dist/<distro>/`. At runtime
the proxy detects the user's distro (`$ROS_DISTRO` → `/opt/ros/<distro>` →
`$CONDA_PREFIX/share/<distro>`) and `dlopen`s the matching inner, which binds to
the user's **sourced system ROS** (the inners are intentionally *not*
self-contained — they must speak to the live ROS graph).

The AppImage needs **no special handling** for this: registry-mode already
unpacks any zip into `usr/lib/plotjuggler/plugins/<id>/` verbatim, preserving the
`dist/<distro>/` layout the proxy resolves relative to itself. The blockers are
in other repos.

**To enable it (other repos, not this one):**

0. **`pj-official-plugins` — proxy must be self-describing for discovery**
   ([PR #169](https://github.com/PlotJuggler/pj-official-plugins/pull/169)). The
   proxy now returns a *static* vtable carrying its embedded manifest, so the
   host plugin scanner can discover and catalog the extension on a machine with
   **no ROS installed** (the per-distro inner is `dlopen`-ed lazily, only when a
   source is instantiated). Without this, installing/bundling the plugin on a
   non-ROS machine fails discovery ("not a valid plugin"). Prerequisite for
   marketplace/registry/AppImage distribution.
1. **`pj-official-plugins`** — in `ci-ros2.yml`, attach `ros2_subscriber-linux-x86_64.zip`
   to a GitHub Release on tag (as `build-release.yml` does for the other
   extensions), so it has a stable `releases/download/...` URL.
2. **`pj-plugin-registry`** — add a `ros2-stream` extension entry whose
   `platforms.linux-x86_64.url` + `checksum` point at that release asset.

Then, here: uncomment `ros2-stream` in `BUNDLE_IDS` (see `build_appimage.sh`).
No other change — the proxy+inners flow through registry-mode unchanged.

## How it differs from PlotJuggler 3

PJ3 linked the system Qt 5, so `linuxdeploy-plugin-qt` found Qt on system paths.
PJ4 links Qt from `./.qt` and most external dependencies from Conan, so the build
script points `linuxdeploy` at both (`QMAKE`, `PATH`, and sourcing
`build/conanrun.sh` for the Conan library closure).

## Known limitations

1. **Qt-module coverage for plugins is not separately analyzed.** The Qt plugin
   deploys modules based on `plotjuggler4`; a plugin needing a Qt module that the
   app does not pull would be missed. None observed yet; revisit if a plugin
   fails to load with a missing-`libQt6*` error.
