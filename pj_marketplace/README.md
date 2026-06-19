# pj_marketplace

Extension marketplace for PlotJuggler — handles registry fetching, package downloads, and extension lifecycle management.

## Dependencies

All dependencies are managed by Conan:

- **Qt6** (Core, Widgets, Network, Test)
- **libarchive**
- **GTest**

## Build

The module is built as part of the PJ4 tree from the repo root, which runs Conan
(root `conanfile.txt`) + CMake and compiles `pj_marketplace` via
`add_subdirectory(pj_marketplace)`:

```bash
# From the repo root
./build.sh
```

Build output:
- `build/libpj_marketplace.a` — static library
- `build/pj_marketplace_app` — standalone executable
- `build/tests/` — test executables

Only the in-tree build (`./build.sh` from the repo root) is supported. The
CMakeLists has a standalone-root guard, but it does not build the full module on
its own: `pj_marketplace_ui` PUBLIC-links the `pj_widgets` target and
`marketplace_window.ui` uses the custom widget `PJ::ComboBox` from
`pj_widgets/ComboBox.h`, neither of which the standalone CMake root provides
(only the root `add_subdirectory(pj_widgets)` does). A bare
`cmake -S pj_marketplace -B build` configures the core library and tests but
cannot link `pj_marketplace_ui` / `pj_marketplace_app`.

## Run Tests

```bash
cd build && ctest --output-on-failure
```

## Project Structure

```
pj_marketplace/
├── include/pj_marketplace/           # public headers (snake_case .hpp)
│   ├── download_manager.hpp
│   ├── extension_manager.hpp
│   ├── platform_utils.hpp
│   ├── registry_manager.hpp
│   ├── qt_diagnostic_bridge.hpp
│   ├── extension.hpp                 # Extension metadata model
│   ├── installed_extension.hpp       # Installed extension record
│   ├── marketplace.hpp
│   ├── marketplace_window.hpp
│   └── extension_detail_dialog.hpp
├── src/
│   ├── core/                         # .cpp only
│   │   ├── DownloadManager.cpp       # HTTP download + checksum + libarchive extraction
│   │   ├── ExtensionManager.cpp      # Install/uninstall/update lifecycle
│   │   ├── PlatformUtils.cpp         # Cross-platform paths and detection
│   │   ├── RegistryManager.cpp       # Remote registry fetching
│   │   └── QtDiagnosticBridge.cpp    # Qt-to-pj diagnostic bridge
│   └── ui/
│       ├── marketplace_window.cpp / .ui
│       └── extension_detail_dialog.cpp / .ui
├── main.cpp                          # standalone-app entry point
├── tests/
│   ├── download_manager_test.cpp
│   ├── extension_manager_test.cpp
│   ├── extension_manager_check_plugin_management.cpp  # manual integration check (excluded from CTest)
│   └── registry_manager_test.cpp
└── CMakeLists.txt
```
