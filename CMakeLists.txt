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
option(PJ_ENABLE_SANITIZERS "Enable ASAN for Debug builds" OFF)
option(PJ_ENABLE_TSAN "Enable ThreadSanitizer for Debug builds" OFF)
# Standalone dev tools (pj_scene2D/tools, e.g. extract_frame). Off by default so
# they don't cost a default/CI build; configure with -DPJ_BUILD_TOOLS=ON to build.
option(PJ_BUILD_TOOLS "Build pj_scene2D dev tools" OFF)
# Foundation build options. PJ_BUILD_TESTS feeds both the plotjuggler_sdk SDK
# submodule and the pj_datastore module. The rest stay off for the PJ4 app
# build: the SDK's ported-plugins collection, and pj_datastore's parquet
# example (PJ_BUILD_PARQUET_IMPORT_EXAMPLE is now a pj_datastore option, pinned
# off here before that subdirectory is added).
set(PJ_BUILD_TESTS ON CACHE BOOL "Build foundation (SDK + pj_datastore) tests" FORCE)
set(PJ_BUILD_PARQUET_IMPORT_EXAMPLE OFF CACHE BOOL "" FORCE)
set(PJ_BUILD_PORTED_PLUGINS OFF CACHE BOOL "" FORCE)

if(PJ_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------

if(MSVC)
    set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
    set(PJ_WARNING_FLAGS
        -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
    )
endif()

# Keep frame pointers in all PJ4-owned targets so `perf record --call-graph fp`
# can unwind without DWARF.
if(NOT MSVC)
    add_compile_options(-fno-omit-frame-pointer)
endif()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network OpenGLWidgets Xml)
find_package(fmt REQUIRED)

# ---------------------------------------------------------------------------
# Foundation (Level 0): the plotjuggler_sdk SDK submodule (pj_base + pj_plugins)
# plus pj_datastore, the columnar storage engine. pj_datastore used to live in
# the plotjuggler_sdk submodule; it now lives in the
# app repo (plugins reach it only through the pj_base C ABI, never by linking
# it). pj_datastore MUST be added AFTER the submodule so the pj_base /
# pj_internal_fmt targets it links already exist.
# ---------------------------------------------------------------------------

add_subdirectory(plotjuggler_sdk)
add_subdirectory(pj_datastore)

# ---------------------------------------------------------------------------
# Vendored third-party
# ---------------------------------------------------------------------------

add_subdirectory(3rdparty/qwt/src)
add_subdirectory(3rdparty/nanocdr)

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
# Sanitizers (root-owned modules only)
# ---------------------------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(PJ_ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
        add_compile_definitions(PJ_ASAN_ACTIVE)
    endif()
    if(PJ_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# ---------------------------------------------------------------------------
# Root-owned PlotJuggler modules
# ---------------------------------------------------------------------------

# pj_scene2D depends on pj_datastore, which is now always built (above).
add_subdirectory(pj_scene2D/core)
add_subdirectory(pj_scene2D/widgets)
if(PJ_BUILD_TOOLS)
    add_subdirectory(pj_scene2D/tools)
endif()
add_subdirectory(pj_scene3D/core)
add_subdirectory(pj_scene3D/widgets)
add_subdirectory(pj_scene3D/demos)
add_subdirectory(pj_marketplace)
add_subdirectory(pj_dialog_host)
add_subdirectory(pj_runtime)
add_subdirectory(pj_widgets)
add_subdirectory(pj_scene_common)
add_subdirectory(pj_plotting)

if(PJ4_BUILD_APP)
    add_subdirectory(pj_app)
endif()

# External GPL render helper (separate executable; PlotJuggler links none of it).
# Built only when the vendored engine source is present.
if(EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/doomgeneric/doomgeneric/doomgeneric.h")
    add_subdirectory(raster_helper)
endif()

if(TARGET pj-raster-helper)
    install(TARGETS pj-raster-helper RUNTIME DESTINATION thirdparty/retro)
    # GPLv2 / shareware compliance: the helper binary must ship with its license
    # text, the written offer for its corresponding source, and the WAD's
    # shareware terms. These travel next to the binary in the installed bundle.
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/COPYING"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/SOURCE-OFFER.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/SHAREWARE-LICENSE.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/README.md"
        DESTINATION thirdparty/retro)
endif()
