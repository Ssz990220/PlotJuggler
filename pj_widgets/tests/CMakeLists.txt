find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(curve_tree_view_test
    curve_tree_view_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(curve_tree_view_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(curve_tree_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(curve_tree_view_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CurveTreeViewTest COMMAND curve_tree_view_test)

add_executable(file_dialog_test
    file_dialog_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(file_dialog_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(file_dialog_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(file_dialog_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME FileDialogTest COMMAND file_dialog_test)

add_executable(credentials_editor_test
    credentials_editor_test.cpp
)

target_link_libraries(credentials_editor_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(credentials_editor_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(credentials_editor_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CredentialsEditorTest COMMAND credentials_editor_test)

find_package(Qt6 REQUIRED COMPONENTS Test)

# Protocol peer for the stream-view test: publishes one frame, no engine.
add_executable(raster_stub_helper raster_stub_helper.cpp)
target_include_directories(raster_stub_helper PRIVATE ${CMAKE_SOURCE_DIR}/raster_ipc)
target_link_libraries(raster_stub_helper PRIVATE Qt6::Core Qt6::Network)
set_target_properties(raster_stub_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(raster_frame_test raster_frame_test.cpp)
target_link_libraries(raster_frame_test PRIVATE pj_widgets GTest::gtest_main)
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(raster_frame_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
set_target_properties(raster_frame_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME RasterFrame COMMAND raster_frame_test)

add_executable(raster_stream_view_test raster_stream_view_test.cpp)
set_target_properties(raster_stream_view_test PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(raster_stream_view_test PRIVATE pj_widgets Qt6::Test GTest::gtest)
target_compile_definitions(raster_stream_view_test PRIVATE
    PJ_STUB_HELPER_PATH="$<TARGET_FILE:raster_stub_helper>")
add_dependencies(raster_stream_view_test raster_stub_helper)
add_test(NAME RasterStreamView COMMAND raster_stream_view_test)

add_executable(layer_list_view_test
    layer_list_view_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

target_link_libraries(layer_list_view_test PRIVATE
    pj_widgets
    GTest::gtest
    Qt6::Test
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(layer_list_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(layer_list_view_test PROPERTIES
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME LayerListViewTest COMMAND layer_list_view_test)
