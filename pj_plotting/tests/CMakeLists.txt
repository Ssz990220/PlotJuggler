find_package(GTest REQUIRED)

add_executable(datastore_curve_adapter_test
    datastore_curve_adapter_test.cpp
)

target_link_libraries(datastore_curve_adapter_test PRIVATE
    pj_plotting_core
    GTest::gtest_main
)

add_test(NAME DatastoreCurveAdapterTest COMMAND datastore_curve_adapter_test)
