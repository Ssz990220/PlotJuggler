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
  src/object_topic_metadata.cpp
  src/url_fetcher.cpp
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
  include/pj_scene3d_widgets/object_topic_metadata.h
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

  # H.20: full coverage for TransformService's incremental UID-keyed TF ingest —
  # the heart of PR #174 (live + scrubbable streaming TF) and the bulk file path.
  # Pure logic against a SessionManager + a mock FrameTransforms parser; no GL,
  # no widgets. Custom main (QCoreApplication for the QObject service).
  add_executable(transform_service_test
    tests/transform_service_test.cpp
  )
  target_link_libraries(transform_service_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_options(transform_service_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME transform_service_test COMMAND transform_service_test)

  # Regression: a TF-only 3D dock (FrameTransforms consumed as config, zero
  # layers) must survive revalidateObjects (H.4), and the TF/dataset binding must
  # reset+rebind when the bound dataset's topics are removed (M.17). Needs the
  # PJ_SCENE3D_TEST_HOOKS accessors and a QApplication (the dock is a QWidget).
  add_executable(scene3d_dock_streaming_test
    tests/scene3d_dock_streaming_test.cpp
  )
  target_link_libraries(scene3d_dock_streaming_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(scene3d_dock_streaming_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(scene3d_dock_streaming_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME scene3d_dock_streaming_test COMMAND scene3d_dock_streaming_test)

  # Regression: layout persistence round-trips FrameTransforms config topics
  # (M.18), re-resolves dataset identity by stable source name across sessions
  # (M.55), and forces the lazily-created view so a saved explicit fixed frame
  # survives a zero-layer restore (M.19). Needs the PJ_SCENE3D_TEST_HOOKS
  # accessors and a QApplication (the dock is a QWidget).
  add_executable(scene3d_dock_persistence_test
    tests/scene3d_dock_persistence_test.cpp
  )
  target_link_libraries(scene3d_dock_persistence_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(scene3d_dock_persistence_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(scene3d_dock_persistence_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME scene3d_dock_persistence_test COMMAND scene3d_dock_persistence_test)

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
    url_fetcher_test
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
  # These tests stand up in-process QTcpServer endpoints / talk Qt networking
  # directly; pj_scene3d_widgets links Qt6::Network only PRIVATEly.
  target_link_libraries(url_fetcher_test PRIVATE Qt6::Network)
  target_link_libraries(scene_entities_layer_model_test PRIVATE Qt6::Network)
  # The robot-model test asserts the render() draw-cache invalidation set via the
  # PJ_SCENE3D_TEST_HOOKS-gated drawsDirtyForTest()/clearDrawsDirtyForTest().
  target_compile_definitions(robot_model_layer_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
endif()
