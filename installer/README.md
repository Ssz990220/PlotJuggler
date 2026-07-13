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

### Installed layout (prefix tree)

The staged payload mirrors the prefix layout the app discovers plugins from —
the same convention the Linux AppImage uses (`bin` beside `lib`):

```
<TargetDir>/                         # @ApplicationsDirX64@/PlotJuggler4
├── bin/
│   ├── PlotJuggler4.exe             # + Qt DLLs, Qt plugins, MSVC runtime, FFmpeg, CPython (+ Lib/, DLLs/, ._pth)
│   └── platforms/ …
└── lib/plotjuggler/plugins/         # built + bundled plugins; app resolves bin/../lib/plotjuggler/plugins
    └── *_plugin.dll
```

The app scans `bin/../lib/plotjuggler/plugins` (via `applicationDirPath`), so
bundled plugins MUST live there — dropping them next to the exe is not scanned.
Plugins load their Qt/FFmpeg/CPython DLLs from `bin/` (the exe's directory, which
Windows searches at load time); `windeployqt --dir bin` puts any plugin-only Qt
modules there too.

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
- **Qt 6.11.1 msvc2022_64** — for `windeployqt.exe`. Auto-detected under `.qt`
  (the `install_qt6.sh` / aqt layout); otherwise pass `-QtDir`.
- **Qt Installer Framework tools** — for `binarycreator.exe`. Install via the Qt
  Maintenance Tool, or `aqt install-tool --outputdir .qt windows desktop tools_ifw`.
  Auto-detected next to `-QtDir` (aqt `.qt\Tools`) and under `C:\Qt\Tools`.
- **Conan + CMake** on PATH — the script builds the ported plugins natively (no
  Git Bash). It handles the MSVC toolchain and Ninja itself:
  - **MSVC** — if `cl.exe` isn't already on PATH (i.e. you're not in an x64 Native
    Tools prompt), the script locates Visual Studio via `vswhere` and imports
    `vcvarsall.bat x64` into the process. Needs VS with the **C++ x64 workload**.
  - **Ninja** — required by the Conan profile's generator; the script auto-locates
    `ninja.exe` (PATH, Conan cache, or Python's Scripts dir) and passes it via
    `-DCMAKE_MAKE_PROGRAM`, so it need not be on PATH. `pip install ninja` if it is
    missing entirely.

  Skip the whole plugin build with `-SkipPlugins`.
- **Consistent MSVC toolset + a clean Conan cache.** The plugin build reuses
  cached deps by `compiler.version=194` (all VS2022 minors share one package id),
  so a cache holding a binary built with a *different* MSVC minor (e.g. 14.5x vs
  14.4x) yields STL link errors (`unresolved external __std_*`). On a clean machine
  this never happens; on a mixed one, `conan remove "<dep>/*" -c` and rebuild.

## Usage

From the repo root, after a Windows build of the app — a plain run builds the
plugins and produces the full installer:

```powershell
.\installer\build_windows_installer.ps1
```

`-QtDir` / `-IfwDir` are auto-detected (`.qt` first, then `C:\Qt`); pass them only
if Qt/IFW live elsewhere. `-Version` defaults to `PJ_APP_VERSION` from repo-root
`versions.env`; override with `-Version <x.y.z>`.

The script:

1. Locates the built `plotjuggler4.exe` / `pj_app.exe` under `-BuildDir`; stages
   it as `bin\PlotJuggler4.exe` in a scratch tree under `%TEMP%`.
2. **Builds the ported plugins** from source (unless `-SkipPlugins`): for each
   plugin in `-PluginList` it runs `conan install` + `cmake` + `cmake --build`
   natively (the same three steps as `pj_ported_plugins/build.sh`, no Git Bash),
   prints an OK/FAIL summary, and bundles the resulting `*_plugin.dll` into
   `lib\plotjuggler\plugins`. A plugin that fails to build is skipped (not fatal) —
   the rest still ship. Only `*_plugin.dll` is collected, so dependency DLLs in the
   build tree are never dragged in.
3. Runs `windeployqt` over the exe **and every bundled plugin DLL** (`--dir bin`,
   so plugin-only Qt modules land in `bin`).
4. Copies the FFmpeg + CPython runtime DLLs from the Conan cache (both the
   downloaded `~/.conan2/p/<hash>/p/bin` and built-from-source
   `~/.conan2/p/b/<hash>/p/bin` layouts; everything else in the graph is static).
5. Copies the CPython `Lib/`, `DLLs/`, and any `python3XX._pth` so the embedded
   interpreter finds its stdlib.
6. Renders `config.xml` / `package.xml` into the stage tree (version +
   release-date tokens substituted).
7. Runs `binarycreator --offline-only` against the stage.

Options: `-SkipPlugins` (fast core-only installer), `-PluginList a,b,c` (override
the plugin set), `-PortedPluginsDir <path>` (plugins repo location).

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
