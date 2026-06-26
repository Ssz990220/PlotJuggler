# PlotJuggler 4 — AppImage packaging (first draft)

Builds a single relocatable `PlotJuggler-<version>-x86_64.AppImage` bundling the
`pj_app` shell, its Qt 6 + Conan runtime, and a fixed set of plugins.

## Build

```bash
./install_qt6.sh        # once: Qt into ./.qt
./build.sh              # builds build/pj_app/pj_app + Conan runtime env
# build the plugin .so files (see plotjuggler_sdk/pj_ported_plugins/)
appimage/build_appimage.sh
```

Output lands at `appimage/PlotJuggler-<version>-x86_64.AppImage`.

## How it differs from PlotJuggler 3

PJ3 linked the system Qt 5, so `linuxdeploy-plugin-qt` found Qt on system paths.
PJ4 links Qt from `./.qt` and most external dependencies from Conan, so the build
script points `linuxdeploy` at both (`QMAKE`, `PATH`, and sourcing
`build/conanrun.sh` for the Conan library closure).

## Plugins in this release

Baked in (the required set):

| Source dir | `.so` |
|---|---|
| data_load_MCAP | libmcap_source_plugin.so |
| data_load_CSV | libcsv_source_plugin.so |
| data_load_parquet | libparquet_source_plugin.so |
| data_stream_dummy | libdummy_stream_plugin.so |
| data_stream_ros2 | libros2_stream_plugin.so (+ per-distro inners) |
| data_stream_foxglove_bridge | libfoxglove_source_plugin.so |
| data_stream_pj_bridge | libpj_bridge_source_plugin.so |
| parser_ros | libparser_ros_plugin.so |
| parser_protobuf | libparser_protobuf_plugin.so |
| parser_json | libparser_json_plugin.so |
| toolbox_mosaico | libtoolbox_mosaico_plugin.so |
| toolbox_quaternion | libtoolbox_quaternion_plugin.so |
| toolbox_transform_editor | libtoolbox_transform_editor_plugin.so |

Deliberately excluded: `toolbox_colormap`, `toolbox_reactive_scripts_editor`.

App plugins are placed under `usr/share/pj_app/plugins/<source-dir>/` — kept out
of `usr/plugins/` so the recursive plugin scanner does not collide with Qt's own
platform plugins that `linuxdeploy-plugin-qt` deploys there. `AppRun.sh` points
`pj_app` at this directory via `--plugin-dir`.

## Known limitations (first draft)

1. **Marketplace install is inert inside the AppImage.** `--plugin-dir` is both
   the scan dir and the marketplace install target, and the bundled dir is
   read-only when the image is mounted. The fix is to seed the bundled plugins
   into the writable `AppDataLocation` on first run and let the default path
   handle installs; not yet done.
2. **ROS 2 plugins need a ROS install on the user's machine.** The bundled proxy
   detects `ROS_DISTRO` / `/opt/ros/*` and `dlopen`s a per-distro inner. The
   inner libraries are bundled but resolve `rclcpp` against the user's ROS at
   runtime — they are not self-contained.
3. **Qt-module coverage for app plugins is not separately analyzed.** The Qt
   plugin deploys modules based on `pj_app`; a plugin needing a Qt module that
   `pj_app` does not pull would be missed. None observed yet; revisit if a
   plugin fails to load with a missing-`libQt6*` error.
