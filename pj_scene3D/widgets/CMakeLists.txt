set(CMAKE_AUTOMOC ON)

find_package(OpenGL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Xml Concurrent Network)
find_package(assimp REQUIRED)

set(PJ_SCENE3D_WIDGETS_SOURCES
  src/gl/buffer.cpp
  src/gl/program.cpp
  src/gl/vertex_array.cpp
  src/gl/debug.cpp
  src/gl/texture.cpp
  src/gl/framebuffer.cpp
  src/scene_hdr_fbo.cpp
  src/gizmos/arrow_gizmo.cpp
  src/passes/axis_overlay_pass.cpp
  src/passes/axis_render_pass.cpp
  src/passes/grid_render_pass.cpp
  src/passes/pointcloud_render_pass.cpp
  src/passes/occupancy_grid_render_pass.cpp
  src/passes/marker_render_pass.cpp
  src/passes/mesh_render_pass.cpp
  src/passes/ssao_pass.cpp
  src/passes/edl_pass.cpp
  src/render_pass.cpp
  src/scene_view_widget.cpp
  src/scene3d_layer.cpp
  src/layers/pointcloud_layer.cpp
  src/layers/occupancy_grid_layer.cpp
  src/layers/scene_entities_layer.cpp
  src/layers/robot_model_layer.cpp
  src/urdf_parser.cpp
  src/urdf_package_resolver.cpp
  src/mesh_loader.cpp
  src/Scene3DDockWidget.cpp
  src/transform_service.cpp
  include/pj_scene3d_widgets/scene_view_widget.h
  include/pj_scene3d_widgets/scene3d_layer.h
  include/pj_scene3d_widgets/mesh_data.h
  include/pj_scene3d_widgets/layers/pointcloud_layer.h
  include/pj_scene3d_widgets/layers/occupancy_grid_layer.h
  include/pj_scene3d_widgets/layers/scene_entities_layer.h
  include/pj_scene3d_widgets/layers/robot_model_layer.h
  include/pj_scene3d_widgets/passes/occupancy_grid_render_pass.h
  include/pj_scene3d_widgets/passes/marker_render_pass.h
  include/pj_scene3d_widgets/passes/mesh_render_pass.h
  include/pj_scene3d_widgets/passes/ssao_pass.h
  include/pj_scene3d_widgets/passes/edl_pass.h
  include/pj_scene3d_widgets/gl/texture.h
  include/pj_scene3d_widgets/gl/framebuffer.h
  include/pj_scene3d_widgets/scene_hdr_fbo.h
  include/pj_scene3d_widgets/mesh_shading_params.h
  include/pj_scene3d_widgets/Scene3DDockWidget.h
  include/pj_scene3d_widgets/transform_service.h
  include/pj_scene3d_widgets/parse_locked.h
)

add_library(pj_scene3d_widgets STATIC ${PJ_SCENE3D_WIDGETS_SOURCES})
target_include_directories(pj_scene3d_widgets PUBLIC include PRIVATE src)
target_compile_features(pj_scene3d_widgets PUBLIC cxx_std_20)
target_compile_options(pj_scene3d_widgets PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
target_link_libraries(pj_scene3d_widgets
  PUBLIC pj_scene3d_core pj_scene_common pj_runtime pj_plugin_runtime_catalog pj_widgets Qt6::Widgets Qt6::OpenGLWidgets Qt6::Xml OpenGL::GL
  PRIVATE pj_internal_fmt Qt6::Concurrent Qt6::Network assimp::assimp
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # Regression: a file reload re-registers each surviving topic's parser slot;
  # layers must re-resolve the parser through SessionManager per use instead of
  # caching the raw pointer from attach() (use-after-free on the next tick).
  add_executable(occupancy_grid_layer_rebind_test
    ../tests/occupancy_grid_layer_rebind_test.cpp
  )
  target_link_libraries(occupancy_grid_layer_rebind_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(occupancy_grid_layer_rebind_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(occupancy_grid_layer_rebind_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME occupancy_grid_layer_rebind_test COMMAND occupancy_grid_layer_rebind_test)

  # Pure (non-GL) URDF parser + package-resolver unit tests. They reach the
  # private src/ headers (urdf_parser.h / urdf_package_resolver.h) and use small
  # hand-written fixtures under tests/fixtures/ (NOT the external DATA/ datasets).
  set(PJ_SCENE3D_WIDGETS_TESTS
    urdf_parser_test
    urdf_package_resolver_test
    mesh_loader_test
    mesh_render_pass_test
    robot_model_layer_test
    scene_entities_layer_model_test
  )
  foreach(test_name ${PJ_SCENE3D_WIDGETS_TESTS})
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE pj_scene3d_widgets GTest::gtest_main)
    target_include_directories(${test_name} PRIVATE src)
    target_compile_definitions(${test_name} PRIVATE
      PJ_SCENE3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_options(${test_name} PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
