add_executable(curve_tree_view_test
    curve_tree_view_test.cpp
)

target_link_libraries(curve_tree_view_test PRIVATE
    pj_widgets
    GTest::gtest
)

if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(curve_tree_view_test PRIVATE ${PJ_WARNING_FLAGS})
endif()

set_target_properties(curve_tree_view_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME CurveTreeViewTest COMMAND curve_tree_view_test)
