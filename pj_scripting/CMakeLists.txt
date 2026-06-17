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

add_library(pj_scripting STATIC
  src/luau_engine.cpp
  src/lua_siso_transform.cpp
  src/filter_catalogue.cpp
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
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # --- Luau filter engine (Approach A) ---
  add_executable(luau_engine_test tests/luau_engine_test.cpp)
  target_link_libraries(luau_engine_test PRIVATE pj_scripting GTest::gtest_main)
  add_test(NAME luau_engine_test COMMAND luau_engine_test)

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
