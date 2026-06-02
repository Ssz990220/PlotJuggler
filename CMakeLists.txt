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
# plotjuggler_core submodule options. Heavy ported plugins and the parquet
# example stay off for the PJ4 app build.
set(PJ_BUILD_TESTS ON CACHE BOOL "Build plotjuggler_core tests" FORCE)
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
# Foundation (Level 0): plotjuggler_core submodule
# ---------------------------------------------------------------------------

add_subdirectory(plotjuggler_core)

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

if(PJ_BUILD_DATASTORE)
    add_subdirectory(pj_scene2D/core)
    add_subdirectory(pj_scene2D/widgets)
    add_subdirectory(pj_scene2D/demos)
endif()
add_subdirectory(pj_scene3D/core)
add_subdirectory(pj_scene3D/widgets)
add_subdirectory(pj_marketplace)
add_subdirectory(pj_dialog_host)
add_subdirectory(pj_runtime)
add_subdirectory(pj_widgets)
add_subdirectory(pj_plotting)

if(PJ4_BUILD_APP)
    add_subdirectory(pj_app)
endif()
