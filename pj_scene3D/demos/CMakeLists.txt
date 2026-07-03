# ---------------------------------------------------------------------------
# pj_scene3D demos — standalone visual tests (no app, no store, no parser)
# ---------------------------------------------------------------------------

set(CMAKE_AUTOMOC ON)

if(TARGET pj_scene3d_widgets)
  add_executable(marker_gallery_demo marker_gallery_demo.cpp)
  target_compile_features(marker_gallery_demo PRIVATE cxx_std_20)
  target_link_libraries(marker_gallery_demo PRIVATE
    pj_scene3d_widgets pj_scene3d_core Qt6::Widgets
  )

  # PosesInFrame triad gizmos (PosesRenderPass / PosesInFrameLayer render path),
  # with a --screenshot mode for offscreen visual checks.
  add_executable(poses_gallery_demo poses_gallery_demo.cpp)
  target_compile_features(poses_gallery_demo PRIVATE cxx_std_20)
  target_link_libraries(poses_gallery_demo PRIVATE
    pj_scene3d_widgets pj_scene3d_core Qt6::Widgets
  )

  # Standalone URDF mesh viewer — fast look-development loop for the 3D mesh
  # pipeline without loading an MCAP. Reaches the private widgets/src headers
  # (urdf_package_resolver.h) the same way the module tests do.
  # resources.qrc gives the demo the same themed SVG icons the app registers
  # (AUTORCC embeds + auto-registers them for an executable target).
  set(CMAKE_AUTORCC ON)
  add_executable(scene3d_mesh_viewer mesh_viewer.cpp ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
  target_compile_features(scene3d_mesh_viewer PRIVATE cxx_std_20)
  target_compile_options(scene3d_mesh_viewer PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_include_directories(scene3d_mesh_viewer PRIVATE ../widgets/src)
  target_link_libraries(scene3d_mesh_viewer PRIVATE pj_scene3d_widgets Qt6::Xml)

  # Self-verifying GPU-vs-CPU AABB-reduction benchmark (offscreen GL, no window).
  # Prints GL_RENDERER + timings and exits non-zero if the GPU result disagrees
  # with the CPU scan. Run with QT_QPA_PLATFORM=offscreen for headless CI.
  add_executable(pointcloud_aabb_benchmark pointcloud_aabb_benchmark.cpp)
  target_compile_features(pointcloud_aabb_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_aabb_benchmark PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_link_libraries(pointcloud_aabb_benchmark PRIVATE pj_scene3d_widgets pj_scene3d_core Qt6::Gui)

  # macOS docked-composite repro harness: a SceneViewWidget hosted inside the SAME
  # Qt-Advanced-Docking system the app uses, beside a raster widget, so the window
  # takes Qt's full-window RHI composite path. Diagnoses the "3D dock renders
  # blank/stale on macOS" bug via the SceneViewWidget lifecycle trace. Needs the
  # vendored ADS target; skipped where it is unavailable.
  if(TARGET qt_advanced_docking)
    add_executable(scene3d_docked_harness scene3d_docked_harness.cpp)
    target_compile_features(scene3d_docked_harness PRIVATE cxx_std_20)
    target_link_libraries(scene3d_docked_harness PRIVATE
      pj_scene3d_widgets qt_advanced_docking Qt6::Widgets
    )
  endif()
endif()
