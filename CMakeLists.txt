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

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# Foundation (Level 0): plotjuggler_core submodule
# ---------------------------------------------------------------------------

add_subdirectory(plotjuggler_core)

# ---------------------------------------------------------------------------
# Vendored third-party
# ---------------------------------------------------------------------------

add_subdirectory(3rdparty/qwt/src)
add_subdirectory(3rdparty/Qt-Advanced-Docking)

# ---------------------------------------------------------------------------
# PJ4 modules (Levels 1-3)
# ---------------------------------------------------------------------------

add_subdirectory(pj_app_core)

if(PJ4_BUILD_APP)
    add_subdirectory(pj_app)
endif()
