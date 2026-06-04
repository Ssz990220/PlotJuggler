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
add_library(pj_scene2d_widgets STATIC
  src/Scene2DDockWidget.cpp
  src/layers/depth_image_layer.cpp
  src/layers/image_layer.cpp
  src/layers/scene2d_layer.cpp
  src/layers/scene_decoder_layer.cpp
  src/media_viewer_widget.cpp
  src/pixel_inspector.cpp
  include/pj_scene2d_widgets/Scene2DDockWidget.h
  include/pj_scene2d_widgets/Media2DDockWidget.h
  include/pj_scene2d_widgets/layers/depth_image_layer.h
  include/pj_scene2d_widgets/layers/image_layer.h
  include/pj_scene2d_widgets/layers/scene2d_layer.h
  include/pj_scene2d_widgets/layers/scene_decoder_layer.h
  include/pj_scene2d_widgets/media_viewer_widget.h
  include/pj_scene2d_widgets/pixel_inspector.h
  shaders/shaders.qrc
)
target_include_directories(pj_scene2d_widgets PUBLIC include)
target_compile_features(pj_scene2d_widgets PUBLIC cxx_std_20)
target_compile_options(pj_scene2d_widgets PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
target_link_libraries(pj_scene2d_widgets
  PUBLIC pj_scene_common pj_scene2d_core pj_runtime Qt6::Widgets Qt6::Gui
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  # Dispatch-table unit test: verifies makePipelineFor() returns the right
  # built-in pipeline (or nullptr) per object type. Does not construct the
  # widget, so it does not require an RHI context.
  add_executable(media2d_dock_widget_dispatch_test
    ../tests/media2d_dock_widget_dispatch_test.cpp
  )
  target_link_libraries(media2d_dock_widget_dispatch_test PRIVATE
    pj_scene2d_widgets
    GTest::gtest
  )
  target_compile_options(media2d_dock_widget_dispatch_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME Media2DDockDispatch COMMAND media2d_dock_widget_dispatch_test)

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

# QRhiWidget requires private Gui headers (rhi/qrhi.h)
get_target_property(_qt6_gui_dir Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_qt6_gui_dir})
  if(EXISTS "${_dir}/${Qt6_VERSION}/QtGui")
    target_include_directories(pj_scene2d_widgets PUBLIC "${_dir}/${Qt6_VERSION}/QtGui")
    break()
  endif()
endforeach()
