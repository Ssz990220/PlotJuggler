# ---------------------------------------------------------------------------
# pj_scripting — Luau scripting engine for PJ4 "Data Processors".
#
# The self-describing filter substrate: a hand-bound Luau ScriptEngine (sandbox +
# instruction/memory watchdog), the FilterClass schema, the FilterCatalogue, and
# LuaSisoTransform — a PJ::proc::DataProcessor whose calculateNextPoint runs a Luau
# filter class. Filters are DATA (one bundled .luau resource), not C++ subclasses.
#
# Qt-free. Depends on pj_base + pj_datastore (public) + Luau (private). MPL-2.0.
# ---------------------------------------------------------------------------

find_package(Luau REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Python REQUIRED)    # conan-provided embedded CPython (cpython::embed)
find_package(pybind11 REQUIRED)  # embedded CPython backend (python_engine.cpp)

# Outside Conan (e.g. a conda/pixi environment) there is no cpython::embed —
# resolve the embed library through CMake's stock FindPython and bridge it to
# the same target name.
if(NOT TARGET cpython::embed)
  find_package(Python REQUIRED COMPONENTS Interpreter Development.Embed)
  add_library(cpython::embed INTERFACE IMPORTED)
  target_link_libraries(cpython::embed INTERFACE Python::Python)
endif()

# Bake the CPython prefix so the embedded interpreter finds its stdlib at runtime
# (PYTHONHOME = <prefix>, with the stdlib at <prefix>/lib/pythonX.Y). Prefer Conan's
# cpython_PACKAGE_FOLDER_<CONFIG> (always emitted); fall back to deriving it from the
# Python include dir or executable if needed.
set(_pj_python_home "")
set(_pj_python_home_is_conan FALSE)
foreach(_cfg RELWITHDEBINFO RELEASE DEBUG MINSIZEREL NONE)
  if(DEFINED cpython_PACKAGE_FOLDER_${_cfg})
    set(_pj_python_home "${cpython_PACKAGE_FOLDER_${_cfg}}")
    set(_pj_python_home_is_conan TRUE)
    break()
  endif()
endforeach()
if(NOT _pj_python_home AND Python_INCLUDE_DIRS)
  list(GET Python_INCLUDE_DIRS 0 _pj_py_inc)
  if(WIN32)
    # Non-Conan Windows Python packages (e.g. conda-forge) install headers
    # directly at <prefix>/include, with no version-suffixed subdir — one hop
    # already reaches <prefix>. The Unix two-hop rule below would overshoot.
    get_filename_component(_pj_python_home "${_pj_py_inc}" DIRECTORY)
  else()
    # <prefix>/include/pythonX.Y -> <prefix> (two hops; the stdlib lives at
    # <prefix>/lib/pythonX.Y).
    get_filename_component(_pj_py_inc_parent "${_pj_py_inc}" DIRECTORY)
    get_filename_component(_pj_python_home "${_pj_py_inc_parent}" DIRECTORY)
  endif()
endif()
if(NOT _pj_python_home AND DEFINED _Python_EXECUTABLE)
  get_filename_component(_pj_python_home "${_Python_EXECUTABLE}" DIRECTORY)
  get_filename_component(_pj_python_home "${_pj_python_home}" DIRECTORY)
endif()
if(NOT _pj_python_home)
  message(WARNING "pj_scripting: could not determine the embedded Python home; the Python backend may fail to init at runtime")
endif()
message(STATUS "pj_scripting: embedded Python home = ${_pj_python_home}")

# PYTHONHOME the embedded interpreter is pointed at. On Windows the Conan CPython
# package nests its runtime — python3XX.dll, Lib/ (stdlib), DLLs/ — under
# <prefix>/bin, so PYTHONHOME must point there; on Linux (and non-Conan Windows
# toolchains, e.g. conda-forge) the stdlib lives directly under the package root.
set(_pj_python_home_define "${_pj_python_home}")
if(WIN32 AND _pj_python_home_is_conan)
  set(_pj_python_home_define "${_pj_python_home}/bin")
endif()

# Windows is a non-blocking portability tracker (PJ4 v1 is Linux-only). CPython's
# Conan recipe disables the static MSVC build (>=3.10), so on Windows CPython is
# linked shared and python_engine_test needs pythonXY.dll resolvable at launch,
# else STATUS_DLL_NOT_FOUND (0xc0000135). Export the CPython runtime dir(s) so the
# CI Test step can prepend them to PATH — mirrors pj_scene2D/core's FFmpeg bin-dir
# export. Linux links CPython statically and needs none of this.
if(WIN32 AND _pj_python_home)
  file(WRITE "${CMAKE_BINARY_DIR}/pj_python_bin_dirs.txt" "${_pj_python_home}/bin\n${_pj_python_home}\n")
endif()

add_library(pj_scripting STATIC
  src/luau_engine.cpp
  src/lua_siso_transform.cpp
  src/lua_mimo_transform.cpp
  src/filter_catalogue.cpp
  src/python_engine.cpp
)
target_include_directories(pj_scripting PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pj_scripting PUBLIC cxx_std_20)
set_target_properties(pj_scripting PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PJ_ASSERT_THROWS)
  target_compile_definitions(pj_scripting PUBLIC PJ_ASSERT_THROWS)
endif()
# NOTE: PJ_WARNING_FLAGS (-Werror) is intentionally NOT applied here — the Luau
# 3rd-party headers trip -Wextra at instantiation points.
target_link_libraries(pj_scripting
  PUBLIC
    pj_base
    pj_datastore  # DataProcessor base + ProcessorSisoAdapter + ISISOTransform (formerly pj_proc)
    nlohmann_json::nlohmann_json  # filter_class.h exposes nlohmann::json in its public API
  PRIVATE
    Luau::VM                      # Luau backend (hand-bound) — luau_engine.cpp only
    Luau::Compiler
    pybind11::embed               # Python backend (embedded CPython) — python_engine.cpp only
    cpython::embed                # static libpython for the embedded interpreter
)

target_compile_definitions(pj_scripting PRIVATE PJ_PYTHON_HOME="${_pj_python_home_define}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # --- Luau filter engine (Approach A) ---
  add_executable(luau_engine_test tests/luau_engine_test.cpp)
  target_link_libraries(luau_engine_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME luau_engine_test COMMAND luau_engine_test)

  add_executable(python_engine_test tests/python_engine_test.cpp)
  target_link_libraries(python_engine_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME python_engine_test COMMAND python_engine_test)

  add_executable(lua_siso_transform_test tests/lua_siso_transform_test.cpp)
  target_link_libraries(lua_siso_transform_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME lua_siso_transform_test COMMAND lua_siso_transform_test)

  add_executable(lua_time_precision_test tests/lua_time_precision_test.cpp)
  target_link_libraries(lua_time_precision_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME lua_time_precision_test COMMAND lua_time_precision_test)

  # --- M2: sandbox + watchdog ---
  add_executable(luau_sandbox_test tests/luau_sandbox_test.cpp)
  target_link_libraries(luau_sandbox_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME luau_sandbox_test COMMAND luau_sandbox_test)

  add_executable(luau_watchdog_test tests/luau_watchdog_test.cpp)
  target_link_libraries(luau_watchdog_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME luau_watchdog_test COMMAND luau_watchdog_test)

  # --- M9: golden behavioral coverage relocated from the retired C++ builtin specs ---
  add_executable(builtin_golden_test tests/builtin_golden_test.cpp)
  target_link_libraries(builtin_golden_test PRIVATE pj_scripting GTest::gtest_main)
  target_compile_definitions(builtin_golden_test PRIVATE
    BUILTIN_FILTERS_PATH="${CMAKE_SOURCE_DIR}/resources/filters/builtin_filters.luau")
  add_test(NAME BuiltinGoldenTest COMMAND builtin_golden_test)

  # --- M5: filter catalogue ---
  add_executable(filter_catalogue_test tests/filter_catalogue_test.cpp)
  target_link_libraries(filter_catalogue_test PRIVATE pj_scripting GTest::gtest_main)
  target_compile_definitions(filter_catalogue_test PRIVATE
    BUILTIN_FILTERS_PATH="${CMAKE_SOURCE_DIR}/resources/filters/builtin_filters.luau")
  add_test(NAME filter_catalogue_test COMMAND filter_catalogue_test)
endif()
