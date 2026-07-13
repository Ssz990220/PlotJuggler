# Release AppImage — all plugins embedded (compiled)

`build_appimage.sh --plugins-registry` (the CI default, see [`README.md`](./README.md))
bundles only the plugins that are published to `pj-plugin-registry`. Three release
plugins are **not** in that set and so are absent from a registry build:

- `ros2-stream` — ROS 2 topic subscriber (a distro-agnostic proxy + per-distro binaries)
- `toolbox-mosaico`
- `toolbox-transform-editor`

To ship a release AppImage with the **complete** curated plugin set, those three are
built **from source** and embedded alongside the registry-style ones.
[`build_release_appimage.sh`](./build_release_appimage.sh) orchestrates that full build.

## The full bundled set (13)

| Loaders | Parsers | Streams | Toolboxes |
|---|---|---|---|
| csv, mcap, parquet | ros, protobuf, json | dummy-streamer, foxglove-bridge, plotjuggler-bridge, **ros2-stream** | quaternion, **transform-editor**, **mosaico** |

(In **bold**: the three that the registry build cannot supply and that this flow
compiles. Excluded by request: `toolbox-colormap`, `toolbox-reactive-scripts-editor`.)

## Two build flows, tied together

1. **ROS 2 (multi-distro)** — `data_stream_ros2/docker/run-local.sh --bundle` builds a
   distro-agnostic proxy plus one binary per supported ROS 2 distro
   (`humble`, `iron`, `jazzy`, `rolling`), each compiled in its own Docker image. At load
   time the proxy dispatches to the binary matching the ROS 2 distro installed on the
   user's machine.
2. **App + non-ROS 2 plugins + package** — the default is `appimage/build_in_docker.sh
   --plugins-dir <sdk>/pj_ported_plugins`, which compiles both the app and the aggregate
   plugin set in the Ubuntu 22.04 builder image (glibc 2.35), picks up the ROS 2 bundle
   from step 1, and packages the AppImage. `build_release_appimage.sh` then extracts the
   result, drops any `.so` outside the curated 13 (the aggregate build produces more),
   and repacks — so the released AppImage is portable **and** carries exactly the 13.
   With `--host-build` the same 13-plugin filter runs but the app + plugins are built on
   the host (`./build.sh` and `pj_ported_plugins/build.sh`), producing a faster iteration
   but non-portable AppImage tied to the host's glibc.

```bash
appimage/build_release_appimage.sh                       # DEFAULT: docker (portable, glibc 2.35)
appimage/build_release_appimage.sh --host-build          # opt-in: host build (non-portable)
appimage/build_release_appimage.sh --skip-ros2           # reuse a previous ROS 2 bundle
appimage/build_release_appimage.sh --ros2-distros "jazzy"  # restrict the ROS 2 matrix
appimage/build_release_appimage.sh --fresh               # (docker) wipe the Conan+ccache volumes
```

## Runtime requirement for ROS 2

The `ros2-stream` proxy links **no** ROS libraries; it `dlopen`s the per-distro binary at
load time, and that binary links `rclcpp` & friends. So the ROS 2 topic subscriber only
appears (and works) when a supported ROS 2 distribution is installed and its environment
is on the process — i.e. `source /opt/ros/<distro>/setup.bash` before launching
PlotJuggler. With no ROS 2 present the plugin stays silent (this is expected, not an
error); every other bundled plugin works regardless.

## glibc floor / portability

By default `build_release_appimage.sh` runs the app + non-ROS 2 plugin build inside
[`build_in_docker.sh`](./build_in_docker.sh) (Ubuntu 22.04 / glibc 2.35), so the released
AppImage runs on any distro with glibc ≥ 2.35 (Ubuntu 22.04 and newer, and equivalents on
other families). ROS 2 inner binaries are always built per-distro in Docker; the proxy
that PlotJuggler loads is built in Ubuntu 22.04, matching the app's glibc floor. Pass
`--host-build` to compile on the host instead — faster for local iteration, but the
resulting AppImage inherits the host's glibc floor (e.g. Ubuntu 24.04 → glibc 2.38).

## Status

`build_release_appimage.sh` is a convenience wrapper over the two flows above — each of
which is the supported way to build its own piece. It targets a local/release build on a
developer machine with Docker available (both the ROS 2 step and the default app+plugins
step run in Docker). The registry path (`build_appimage.sh --plugins-registry`) remains
the lighter option for an AppImage that omits the three compile-only plugins.
