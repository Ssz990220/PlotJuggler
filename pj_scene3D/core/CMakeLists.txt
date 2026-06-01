# ---------------------------------------------------------------------------
# pj_scene3d_core - geometry/scene core, no Qt or GL dependency
# ---------------------------------------------------------------------------

find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(pj_scene3d_core STATIC
  src/occupancy_grid_reconstructor.cpp
  src/tf/tf_buffer.cpp
  src/version.cpp
)
target_include_directories(pj_scene3d_core PUBLIC include)
target_compile_features(pj_scene3d_core PUBLIC cxx_std_20)
target_compile_options(pj_scene3d_core PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
target_link_libraries(pj_scene3d_core
  PUBLIC glm::glm pj_base nlohmann_json::nlohmann_json
)

if(PJ_BUILD_TESTS)
  set(PJ_SCENE3D_CORE_TESTS
    ../tests/occupancy_grid_reconstructor_test.cpp
    ../tests/tf_buffer_test.cpp
    ../tests/tf_buffer_hierarchy_test.cpp
  )
  foreach(test_src ${PJ_SCENE3D_CORE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE pj_scene3d_core GTest::gtest_main)
    target_compile_options(${test_name} PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
