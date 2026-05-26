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
