# PlotJuggler 4 — Windows installer (Qt Installer Framework)

A self-contained Windows installer for PJ4, built with the **Qt Installer
Framework** (IFW) — the same mechanism PlotJuggler 3 uses. Output is a single
offline `PlotJuggler-<version>-Windows-x64.exe`: a wizard that installs the app
with a Start-Menu + Desktop shortcut and a maintenance/uninstall tool.

There is **no portable-zip path** — the deployment target is this installer.

## Layout

```
installer/
├── config.xml                                     # installer identity — version tokenised as __PJ_VERSION__
├── build_windows_installer.ps1                    # staging + binarycreator orchestrator (run on Windows)
└── packages/
    └── io.plotjuggler.application/
        └── meta/
            ├── package.xml                        # component metadata — version tokenised
            ├── installscript.qs                   # shortcuts, per-user install, custom target-dir page
            ├── targetwidget.ui                    # the target-dir wizard page
            ├── license_mpl.txt                    # MPL-2.0 (PlotJuggler)
            └── license_lgpl.txt                   # LGPL (Qt)
```

The **source tree is templated** (`__PJ_VERSION__` / `__PJ_RELEASE_DATE__`
placeholders) and never mutated by a build: the script renders the XMLs into
a scratch staging directory under `%TEMP%\pj4-installer-stage` and points
`binarycreator -p` at that staged `packages/` tree.

## Prerequisites on the Windows build host

- **PJ4 already built** (RelWithDebInfo) with CPython enabled:
  ```powershell
  conan install . --output-folder=build --build=missing `
      -s build_type=RelWithDebInfo -s compiler.cppstd=20 `
      -o "cpython/*:shared=True" -s "cpython/*:build_type=Release"
  cmake --preset ... ; cmake --build build --config RelWithDebInfo
  ```
  The `cpython/*:shared=True` override matches Windows CI and produces
  `python3XX.dll` — the local default is static on MSVC and the installer
  would then fail to find the runtime.
- **Qt 6.11.1 msvc2022_64** — for `windeployqt.exe`.
- **Qt Installer Framework tools** — for `binarycreator.exe`. Install via the
  Qt Maintenance Tool, or `aqt install-tool windows desktop tools_ifw`.
- The **Conan cache** that produced the build (`%USERPROFILE%\.conan2`).

## Usage

From the repo root, after a Windows build:

```powershell
.\installer\build_windows_installer.ps1 `
    -BuildDir build `
    -QtDir C:\Qt\6.11.1\msvc2022_64
```

`-Version` defaults to `PJ_APP_VERSION` read from repo-root `versions.env`;
override with `-Version <x.y.z>` for a one-off build.

The script:

1. Locates the built `plotjuggler4.exe` / `pj_app.exe` under `-BuildDir`.
2. Stages it as `PlotJuggler4.exe` in a scratch tree under `%TEMP%`.
3. Runs `windeployqt` over the exe **and every plugin DLL** — a plugin can
   pull Qt modules the main exe does not, and without walking each plugin
   those transitive Qt DLLs never land in the bundle.
4. Copies non-Qt Conan-shared runtime DLLs (FFmpeg, CPython, dav1d, Draco,
   zstd, lz4, mcap) from `~/.conan2/p/<hash>/p/bin/` — the search is
   constrained to Conan **package** dirs, so intermediate build-tree copies
   are ignored.
5. Copies the CPython `Lib/`, `DLLs/`, and any `python3XX._pth` next to the
   DLL so the embedded interpreter finds its stdlib.
6. Renders `config.xml` / `package.xml` into the stage tree (version +
   release-date tokens substituted).
7. Runs `binarycreator --offline-only` against the stage.

`binarycreator` and `windeployqt` are auto-detected from `PATH` (and IFW also
from `C:\Qt\Tools\QtInstallerFramework\*`) when the corresponding `-QtDir` /
`-IfwDir` are omitted. Bundle extra plugins with `-PluginsDir <dir>`.

## Install-time behaviour

- **Per-user install by default** — the wizard offers `%LOCALAPPDATA%\PlotJuggler4`
  and does not call `gainAdminRights()`. Corporate machines that block UAC
  prompts install without an escalation dialog. Admin-write locations still
  work; the user just picks one.
- **Replace-existing** — if the target directory already contains a PJ4
  install, the wizard **asks before** running `maintenancetool purge`. The
  earlier flow purged silently.
- **Shortcuts** — Start Menu and Desktop link to `PlotJuggler4.exe`.

## Known gaps (first version)

- **No code signing.** The `.exe` is unsigned, so SmartScreen warns on first
  run ("More info" → "Run anyway").
- **CI.** Producing this in `windows-ci.yml` is a follow-up; today it is a
  local, on-demand build. The stage dir is under `%TEMP%` so a CI run is
  clean-slate every time.
- **ROS 2 subscriber.** The ros2-stream build system is Linux-only, so ROS
  streaming is absent from the Windows installer for this release.
