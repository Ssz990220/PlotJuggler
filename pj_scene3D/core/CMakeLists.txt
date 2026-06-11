# ---------------------------------------------------------------------------
# pj_scene3d_core - geometry/scene core, no Qt or GL dependency
# ---------------------------------------------------------------------------

find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)
# Compressed-point-cloud decoders (Draco + Cloudini). Linked PRIVATE: the codecs are
# an implementation detail of pj_scene3d_core, mirroring how pj_scene2d_core links
# libjpeg-turbo / libpng PRIVATE for image decode.
find_package(draco REQUIRED)
find_package(cloudini REQUIRED)

add_library(pj_scene3d_core STATIC
  src/camera/camera.cpp
  src/camera/camera_math.cpp
  src/occupancy_grid_reconstructor.cpp
  src/pointcloud_codecs.cpp
  src/scene_entities_decode.cpp
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
  PRIVATE draco::draco cloudini::cloudini
)

if(PJ_BUILD_TESTS)
  set(PJ_SCENE3D_CORE_TESTS
    ../tests/occupancy_grid_reconstructor_test.cpp
    ../tests/occupancy_grid_bounds_test.cpp
    ../tests/camera_near_far_test.cpp
    ../tests/camera_zoom_to_cursor_test.cpp
    ../tests/camera_state_transfer_test.cpp
    ../tests/scene_entities_decode_test.cpp
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

  # pointcloud_codecs_test drives the Draco + Cloudini *encoders* directly to build
  # fixtures, so it links them explicitly (pj_scene3d_core links them PRIVATE).
  add_executable(pointcloud_codecs_test ../tests/pointcloud_codecs_test.cpp)
  target_link_libraries(pointcloud_codecs_test PRIVATE
    pj_scene3d_core draco::draco cloudini::cloudini GTest::gtest_main)
  target_compile_options(pointcloud_codecs_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_codecs_test COMMAND pointcloud_codecs_test)
endif()
