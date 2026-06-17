find_package(GTest REQUIRED)

add_executable(datastore_curve_adapter_test
    datastore_curve_adapter_test.cpp
)

target_link_libraries(datastore_curve_adapter_test PRIVATE
    pj_plotting_core
    GTest::gtest_main
)

add_test(NAME DatastoreCurveAdapterTest COMMAND datastore_curve_adapter_test)

add_executable(filtered_curve_adapter_test
    filtered_curve_adapter_test.cpp
)

target_link_libraries(filtered_curve_adapter_test PRIVATE
    pj_plotting_core
    GTest::gtest_main
)

add_test(NAME FilteredCurveAdapterTest COMMAND filtered_curve_adapter_test)

add_executable(dock_widget_placeholder_test
    dock_widget_placeholder_test.cpp
)

target_compile_options(dock_widget_placeholder_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(dock_widget_placeholder_test PRIVATE
    pj_plotting
    pj_widgets
    GTest::gtest
)

add_test(NAME DockWidgetPlaceholderTest COMMAND dock_widget_placeholder_test)

add_executable(plot_widget_replace_curve_test
    plot_widget_replace_curve_test.cpp
)

target_compile_options(plot_widget_replace_curve_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(plot_widget_replace_curve_test PRIVATE
    pj_plotting
    GTest::gtest
)

add_test(NAME PlotWidgetReplaceCurveTest COMMAND plot_widget_replace_curve_test)

# Compiles resources.qrc into the executable so :/filters/builtin_filters.luau is
# embedded — SessionManager reads it to install the Luau filter catalogue (AUTORCC
# auto-registers the resource for an executable target). Without it the panel would
# have no filters to list or apply.
add_executable(filter_editor_panel_test
    filter_editor_panel_test.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(filter_editor_panel_test PROPERTIES AUTORCC ON)

target_compile_options(filter_editor_panel_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(filter_editor_panel_test PRIVATE
    pj_plotting
    pj_datastore
    pj_scripting
    pj_widgets
    GTest::gtest
)

add_test(NAME FilterEditorPanelTest COMMAND filter_editor_panel_test)

add_executable(parameter_form_test
    parameter_form_test.cpp
)
target_compile_options(parameter_form_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parameter_form_test PRIVATE
    pj_plotting
    pj_scripting
    pj_widgets
    GTest::gtest
)
add_test(NAME ParameterFormTest COMMAND parameter_form_test)
