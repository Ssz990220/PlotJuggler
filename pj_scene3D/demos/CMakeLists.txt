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
endif()
