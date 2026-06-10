# ---------------------------------------------------------------------------
# pj_scene2d_widgets — Qt widget layer for media display (requires Qt 6.8+)
# ---------------------------------------------------------------------------

find_package(Qt6 6.8 QUIET COMPONENTS Widgets)
if(NOT Qt6_FOUND)
  message(STATUS "pj_scene2d_widgets: Qt 6.8+ not found, skipping")
  return()
endif()
message(STATUS "pj_scene2d_widgets: using Qt ${Qt6_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- pj_scene2d_widgets: image viewer (QRhiWidget), no mpv dependency ---
set(PJ_SCENE2D_WIDGETS_SOURCES
  src/Scene2DDockWidget.cpp
  src/layers/depth_image_layer.cpp
  src/layers/image_layer.cpp
  src/layers/scene2d_layer.cpp
  src/layers/scene_decoder_layer.cpp
  src/media_viewer_widget.cpp
  src/pixel_inspector.cpp
  src/scene2d_pipelines.cpp
  include/pj_scene2d_widgets/Scene2DDockWidget.h
  include/pj_scene2d_widgets/layers/depth_image_layer.h
  include/pj_scene2d_widgets/layers/image_layer.h
  include/pj_scene2d_widgets/layers/scene2d_layer.h
  include/pj_scene2d_widgets/layers/scene_decoder_layer.h
  include/pj_scene2d_widgets/media_viewer_widget.h
  include/pj_scene2d_widgets/pixel_inspector.h
  include/pj_scene2d_widgets/scene2d_pipelines.h
  shaders/shaders.qrc
)

get_target_property(_pj_scene2d_core_public_defs pj_scene2d_core INTERFACE_COMPILE_DEFINITIONS)
set(PJ_SCENE2D_WIDGETS_HAS_FFMPEG FALSE)
if(_pj_scene2d_core_public_defs AND "PJ_HAS_FFMPEG" IN_LIST _pj_scene2d_core_public_defs)
  set(PJ_SCENE2D_WIDGETS_HAS_FFMPEG TRUE)
  list(APPEND PJ_SCENE2D_WIDGETS_SOURCES
    src/layers/video_layer.cpp
    include/pj_scene2d_widgets/layers/video_layer.h
  )
endif()

add_library(pj_scene2d_widgets STATIC ${PJ_SCENE2D_WIDGETS_SOURCES})
target_include_directories(pj_scene2d_widgets PUBLIC include)
target_compile_features(pj_scene2d_widgets PUBLIC cxx_std_20)
target_compile_options(pj_scene2d_widgets PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
target_link_libraries(pj_scene2d_widgets
  PUBLIC pj_scene_common pj_scene2d_core pj_runtime Qt6::Widgets Qt6::Gui Qt6::GuiPrivate
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # Dispatch-table unit test: verifies makeScene2DPipelineFor() returns the right
  # built-in pipeline (or nullptr) per object type. Does not construct the
  # widget, so it does not require an RHI context.
  add_executable(scene2d_dock_widget_dispatch_test
    ../tests/scene2d_dock_widget_dispatch_test.cpp
  )
  target_link_libraries(scene2d_dock_widget_dispatch_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest
  )
  target_compile_options(scene2d_dock_widget_dispatch_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME scene2d_dock_widget_dispatch_test COMMAND scene2d_dock_widget_dispatch_test)

  add_executable(pixel_inspector_test
    ../tests/pixel_inspector_test.cpp
  )
  target_link_libraries(pixel_inspector_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest_main
  )
  target_compile_options(pixel_inspector_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME PixelInspector COMMAND pixel_inspector_test)

  add_executable(scene2d_dock_widget_test
    ../tests/scene2d_dock_widget_test.cpp
  )
  target_link_libraries(scene2d_dock_widget_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest
  )
  target_compile_options(scene2d_dock_widget_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME scene2d_dock_widget_test COMMAND scene2d_dock_widget_test)
endif()
