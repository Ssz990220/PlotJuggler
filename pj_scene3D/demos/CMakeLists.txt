# ---------------------------------------------------------------------------
# pj_scene3D demos — standalone visual tests (no app, no store, no parser)
# ---------------------------------------------------------------------------

if(TARGET pj_scene3d_widgets)
  add_executable(marker_gallery_demo marker_gallery_demo.cpp)
  target_compile_features(marker_gallery_demo PRIVATE cxx_std_20)
  target_link_libraries(marker_gallery_demo PRIVATE
    pj_scene3d_widgets pj_scene3d_core Qt6::Widgets
  )
endif()
