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
  src/gl/gpu_profiler.cpp
  src/scene_hdr_fbo.cpp
  src/gizmos/arrow_gizmo.cpp
  src/gizmos/arrow_mesh.cpp
  src/passes/axis_overlay_pass.cpp
  src/passes/axis_render_pass.cpp
  src/passes/grid_geometry.cpp
  src/passes/grid_render_pass.cpp
  src/passes/tf_connections_render_pass.cpp
  src/passes/pointcloud_render_pass.cpp
  src/passes/occupancy_grid_render_pass.cpp
  src/passes/marker_render_pass.cpp
  src/passes/poses_render_pass.cpp
  src/passes/mesh_render_pass.cpp
  src/passes/ssao_pass.cpp
  src/passes/edl_pass.cpp
  src/render_pass.cpp
  src/scene_view_widget.cpp
  src/scene3d_layer.cpp
  src/layers/depth_cloud_layer.cpp
  src/layers/pointcloud_layer.cpp
  src/layers/occupancy_grid_layer.cpp
  src/layers/poses_in_frame_layer.cpp
  src/layers/scene_entities_layer.cpp
  src/layers/robot_model_layer.cpp
  src/urdf_parser.cpp
  src/urdf_package_resolver.cpp
  src/mesh_loader.cpp
  src/object_topic_metadata.cpp
  src/url_fetcher.cpp
  src/Scene3DDockWidget.cpp
  src/transform_service.cpp
  src/hud_overlay.cpp
  include/pj_scene3d_widgets/hud_overlay.h
  include/pj_scene3d_widgets/scene_view_widget.h
  include/pj_scene3d_widgets/scene3d_layer.h
  include/pj_scene3d_widgets/mesh_data.h
  include/pj_scene3d_widgets/layers/depth_cloud_layer.h
  include/pj_scene3d_widgets/layers/pointcloud_layer.h
  include/pj_scene3d_widgets/layers/occupancy_grid_layer.h
  include/pj_scene3d_widgets/layers/poses_in_frame_layer.h
  include/pj_scene3d_widgets/layers/scene_entities_layer.h
  include/pj_scene3d_widgets/layers/robot_model_layer.h
  include/pj_scene3d_widgets/passes/occupancy_grid_render_pass.h
  include/pj_scene3d_widgets/passes/marker_render_pass.h
  include/pj_scene3d_widgets/passes/poses_render_pass.h
  include/pj_scene3d_widgets/passes/mesh_render_pass.h
  include/pj_scene3d_widgets/passes/ssao_pass.h
  include/pj_scene3d_widgets/passes/edl_pass.h
  include/pj_scene3d_widgets/gl/texture.h
  include/pj_scene3d_widgets/gl/framebuffer.h
  include/pj_scene3d_widgets/gl/gpu_profiler.h
  include/pj_scene3d_widgets/moving_average.h
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
  # Shared test scaffolding (mock_parser_support.h: makeBoundHandle /
  # CountingObjectParser / registerObjectTopic / pumpUntil) lives in tests/; put
  # it on every test target's include path (directory-scoped — tests only).
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests)

  # The coalescing test encodes valid fixtures with the Cloudini encoder, which
  # pj_scene3d_core links PRIVATE — re-resolve the imported target here so the
  # test can link it directly (same as pointcloud_codecs_test in core/).
  find_package(cloudini REQUIRED)

  # grid_geometry_test links glm directly (the widgets lib gets it transitively
  # via pj_scene3d_core, but this test compiles the geometry source standalone).
  find_package(glm REQUIRED)

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

  # Pure (no GL/Qt) ground-grid geometry: full-checkerboard cell coverage +
  # per-vertex parity, and the line-grid vertex counts. Compiles the geometry
  # source directly and links only glm — no widgets/Qt/GL in this test.
  add_executable(grid_geometry_test
    tests/grid_geometry_test.cpp
    src/passes/grid_geometry.cpp
  )
  target_include_directories(grid_geometry_test PRIVATE include)
  target_link_libraries(grid_geometry_test PRIVATE
    glm::glm
    GTest::gtest_main
  )
  target_compile_options(grid_geometry_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME grid_geometry_test COMMAND grid_geometry_test)

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
  # Layer-level coverage for the '<base>_updates' patch window: UID-based store
  # traversal vs an independent cell-by-cell replay, the retroactive-ingest
  # (behind-live-edge) invalidate path, and the base-keyframe parse memo.
  add_executable(occupancy_grid_layer_updates_test
    tests/occupancy_grid_layer_updates_test.cpp
  )
  target_link_libraries(occupancy_grid_layer_updates_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(occupancy_grid_layer_updates_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(occupancy_grid_layer_updates_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME occupancy_grid_layer_updates_test COMMAND occupancy_grid_layer_updates_test)

  # PointCloudLayer sample-identity caches: reset on dataset replace (stale
  # decoded/failed/pushed ids under a stable topic id), stamp-0 is data not a
  # sentinel, and wanted_-gating of stale async decodes on mixed-mode topics.
  add_executable(pointcloud_layer_cache_test
    tests/pointcloud_layer_cache_test.cpp
  )
  target_link_libraries(pointcloud_layer_cache_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(pointcloud_layer_cache_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(pointcloud_layer_cache_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_layer_cache_test COMMAND pointcloud_layer_cache_test)

  # Copy/paste/apply-to-family: serializeLayerParams/applyLayerParams must
  # round-trip every PointCloudLayer parameter (the data path the Scene3D config
  # panel's copy/paste toolbar drives).
  add_executable(pointcloud_layer_param_transfer_test
    tests/pointcloud_layer_param_transfer_test.cpp
  )
  target_link_libraries(pointcloud_layer_param_transfer_test PRIVATE
    pj_scene3d_widgets
    pj_scene_common
    GTest::gtest
  )
  target_compile_options(pointcloud_layer_param_transfer_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_layer_param_transfer_test COMMAND pointcloud_layer_param_transfer_test)

  # DepthCloudLayer: a depth-encoded sdk::Image + CameraInfo (matched by frame_id)
  # back-project into points via the kImage path; color images opt out; the
  # encoding gate (isDepthEncoding) is pinned.
  add_executable(depth_cloud_layer_test
    tests/depth_cloud_layer_test.cpp
  )
  target_link_libraries(depth_cloud_layer_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(depth_cloud_layer_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(depth_cloud_layer_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME depth_cloud_layer_test COMMAND depth_cloud_layer_test)

  # Dock-level depth/color gate: Scene3DDockWidget::addTopic accepts a depth-
  # encoded kImage as a DepthCloud layer and refuses a color kImage (the
  # "droppable into 3D" guarantee behind drag-drop absorption). The dock is a
  # QWidget, so this links GTest::gtest and supplies a QApplication main.
  add_executable(scene3d_dock_depth_gate_test
    tests/scene3d_dock_depth_gate_test.cpp
  )
  target_link_libraries(scene3d_dock_depth_gate_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(scene3d_dock_depth_gate_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(scene3d_dock_depth_gate_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME scene3d_dock_depth_gate_test COMMAND scene3d_dock_depth_gate_test)

  # PosesInFrameLayer: tracker-time decode -> triad expansion, scrub coalescing,
  # size/opacity re-expansion, empty/orphan edges, the per-use parseLocked rebind
  # guard, and the copy/paste/apply-to-family param round-trip (serializeLayerParams).
  add_executable(poses_in_frame_layer_test
    tests/poses_in_frame_layer_test.cpp
  )
  target_link_libraries(poses_in_frame_layer_test PRIVATE
    pj_scene3d_widgets
    pj_scene_common
    GTest::gtest
  )
  target_compile_definitions(poses_in_frame_layer_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(poses_in_frame_layer_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME poses_in_frame_layer_test COMMAND poses_in_frame_layer_test)

  # M.65: pin PointCloudLayer's per-use parser binding (raw renderAt path still
  # calls parseLocked on the UI thread per tick — a reintroduced cached pointer is
  # a same-file-reload use-after-free). Mirrors occupancy_grid_layer_rebind_test.
  add_executable(pointcloud_layer_rebind_test
    ../tests/pointcloud_layer_rebind_test.cpp
  )
  target_link_libraries(pointcloud_layer_rebind_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(pointcloud_layer_rebind_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(pointcloud_layer_rebind_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_layer_rebind_test COMMAND pointcloud_layer_rebind_test)

  # RGB-direct colour mode: a cloud carrying per-point colour (foxglove
  # red/green/blue/alpha or a packed rgba field) collapses the channels to a single
  # RGB mode, defaults to it, and round-trips through layout XML.
  add_executable(pointcloud_layer_rgb_test
    ../tests/pointcloud_layer_rgb_test.cpp
  )
  target_link_libraries(pointcloud_layer_rgb_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_definitions(pointcloud_layer_rgb_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(pointcloud_layer_rgb_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_layer_rgb_test COMMAND pointcloud_layer_rgb_test)

  # M.66: drive the latest-wins compressed-decode coalescing state machine
  # (pending_ collapse, wanted_ staleness, failed_id_ memo) over the real
  # QtConcurrent + QFutureWatcher path. Encodes valid fixtures with the Cloudini
  # encoder (PRIVATE in pj_scene3d_core), so it links cloudini explicitly like
  # pointcloud_codecs_test does.
  add_executable(pointcloud_layer_coalescing_test
    ../tests/pointcloud_layer_coalescing_test.cpp
  )
  target_link_libraries(pointcloud_layer_coalescing_test PRIVATE
    pj_scene3d_widgets
    cloudini::cloudini
    GTest::gtest
  )
  target_compile_definitions(pointcloud_layer_coalescing_test PRIVATE PJ_SCENE3D_TEST_HOOKS)
  target_compile_options(pointcloud_layer_coalescing_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME pointcloud_layer_coalescing_test COMMAND pointcloud_layer_coalescing_test)

  # CPU-rasterized HUD panel (perf HUD + TF hover label): correct sizing,
  # DPR-independent logical layout, actual glyph pixels, multi-line growth, and
  # the empty->null contract. Needs a QApplication for the font system; no GL.
  add_executable(hud_overlay_test
    tests/hud_overlay_test.cpp
  )
  target_link_libraries(hud_overlay_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_options(hud_overlay_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME hud_overlay_test COMMAND hud_overlay_test)

  # Real-GL integration test: renders the perf HUD on an actual OpenGL context,
  # forces the dock-reparent context recreation that corrupted HUD text in #214,
  # and asserts the overlay survives. Skips when no GL is available; CI runs it
  # under xvfb-run. Needs a QApplication (custom main) so it links GTest::gtest.
  add_executable(hud_overlay_gl_test
    tests/hud_overlay_gl_test.cpp
  )
  target_link_libraries(hud_overlay_gl_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_options(hud_overlay_gl_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME hud_overlay_gl_test COMMAND hud_overlay_gl_test)

  # Real-GL integration test for the TF parent-connection lines: renders the
  # scene and asserts magenta line pixels appear only when enabled (and survive
  # the dock-reparent context recreation). Same skip rules / xvfb requirement as
  # hud_overlay_gl_test; custom main, so it links GTest::gtest.
  add_executable(tf_connections_gl_test
    tests/tf_connections_gl_test.cpp
  )
  target_link_libraries(tf_connections_gl_test PRIVATE
    pj_scene3d_widgets
    GTest::gtest
  )
  target_compile_options(tf_connections_gl_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME tf_connections_gl_test COMMAND tf_connections_gl_test)

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
    gl_owning_context_test
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
