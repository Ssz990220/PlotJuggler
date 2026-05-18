find_package(GTest REQUIRED)

add_executable(datastore_curve_adapter_test
    datastore_curve_adapter_test.cpp
)

target_link_libraries(datastore_curve_adapter_test PRIVATE
    pj_plotting_core
    GTest::gtest_main
)

add_test(NAME DatastoreCurveAdapterTest COMMAND datastore_curve_adapter_test)

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
