cmake_minimum_required(VERSION 3.22)
project(plotjuggler4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(PJ4_BUILD_APP "Build the pj_app desktop shell" ON)
# plotjuggler_core submodule options — pj_marketplace in the current submodule
# pin declares tests unconditionally, so PJ_BUILD_TESTS must stay ON until that
# is fixed upstream. Heavy ported plugins and the parquet example stay off.
set(PJ_BUILD_TESTS ON CACHE BOOL "Build plotjuggler_core tests" FORCE)
set(PJ_BUILD_PARQUET_IMPORT_EXAMPLE OFF CACHE BOOL "" FORCE)
set(PJ_BUILD_PORTED_PLUGINS OFF CACHE BOOL "" FORCE)

if(PJ_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)

# ---------------------------------------------------------------------------
# Foundation (Level 0): plotjuggler_core submodule
# ---------------------------------------------------------------------------

add_subdirectory(plotjuggler_core)

# ---------------------------------------------------------------------------
# Vendored third-party
# ---------------------------------------------------------------------------

add_subdirectory(3rdparty/qwt/src)

# Qt-Advanced-Docking submodule (PlotJuggler/Qt-Advanced-Docking-System fork
# tracking upstream master + the Wayland fix on feature/wayland-fix). Pin the
# version to skip the upstream root CMake's git_describe call (which fails in
# a submodule checkout), build static, and skip examples/demos.
set(ADS_VERSION "4.5.0" CACHE STRING "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/Qt-Advanced-Docking)
add_library(qt_advanced_docking ALIAS qtadvanceddocking-qt6)

# ---------------------------------------------------------------------------
# PJ4 modules (Levels 1-3)
# ---------------------------------------------------------------------------

add_subdirectory(pj_app_core)
add_subdirectory(pj_plot_widgets)

if(PJ4_BUILD_APP)
    add_subdirectory(pj_app)
endif()
