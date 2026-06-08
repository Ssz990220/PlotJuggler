find_package(Qt6 REQUIRED COMPONENTS Core Network Xml)
find_package(nlohmann_json REQUIRED)
find_package(tsl-robin-map REQUIRED)
if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

add_library(pj_runtime STATIC
    src/AppSession.cpp
    src/CatalogModel.cpp
    src/CurveColorRegistry.cpp
    src/DataSourceRuntimeHost.cpp
    src/DiagnosticHistory.cpp
    src/ExtensionCatalogService.cpp
    src/PlaybackEngine.cpp
    src/QSettingsBackend.cpp
    src/SessionManager.cpp
    src/ToolboxRuntimeHost.cpp
    include/pj_runtime/AppSession.h
    include/pj_runtime/CatalogModel.h
    include/pj_runtime/CurveColorRegistry.h
    include/pj_runtime/CurveDescriptor.h
    include/pj_runtime/DataSourceRuntimeHost.h
    include/pj_runtime/DiagnosticHistory.h
    include/pj_runtime/ExtensionCatalogService.h
    include/pj_runtime/IDataWidget.h
    include/pj_runtime/PlaybackEngine.h
    include/pj_runtime/QSettingsBackend.h
    include/pj_runtime/SessionManager.h
    include/pj_runtime/ToolboxRuntimeHost.h
)

set_target_properties(pj_runtime PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_runtime PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Xml
    pj_datastore
    pj_marketplace
    pj_plugin_runtime_catalog
    nlohmann_json::nlohmann_json
  PRIVATE
    tsl::robin_map
    pj_internal_fmt
)

if(PJ_BUILD_TESTS)
    add_executable(time_test
        tests/time_test.cpp
    )
    target_compile_options(time_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(time_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME TimeTest COMMAND time_test)

    add_executable(app_session_test
        tests/app_session_test.cpp
    )
    target_link_libraries(app_session_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME AppSessionTest COMMAND app_session_test)

    add_executable(session_manager_test
        tests/session_manager_test.cpp
    )
    target_link_libraries(session_manager_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME SessionManagerTest COMMAND session_manager_test)

    add_executable(catalog_model_test
        tests/catalog_model_test.cpp
    )
    target_compile_options(catalog_model_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(catalog_model_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME CatalogModelTest COMMAND catalog_model_test)

    add_executable(curve_color_registry_test
        tests/curve_color_registry_test.cpp
    )
    target_compile_options(curve_color_registry_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(curve_color_registry_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME CurveColorRegistryTest COMMAND curve_color_registry_test)

    add_executable(qsettings_backend_test
        tests/qsettings_backend_test.cpp
    )
    target_compile_options(qsettings_backend_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(qsettings_backend_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME QSettingsBackendTest COMMAND qsettings_backend_test)

    add_executable(toolbox_runtime_host_test
        tests/toolbox_runtime_host_test.cpp
    )
    target_compile_options(toolbox_runtime_host_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(toolbox_runtime_host_test PRIVATE
        pj_runtime
        GTest::gtest
    )
    add_test(NAME ToolboxRuntimeHostTest COMMAND toolbox_runtime_host_test)

    add_library(runtime_host_object_parser_plugin SHARED
        tests/runtime_host_object_parser_plugin.cpp
    )
    target_compile_features(runtime_host_object_parser_plugin PRIVATE cxx_std_20)
    target_compile_options(runtime_host_object_parser_plugin PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(runtime_host_object_parser_plugin PRIVATE
        pj_plugin_sdk
    )

    add_executable(data_source_runtime_host_object_ingest_test
        tests/data_source_runtime_host_object_ingest_test.cpp
    )
    target_compile_definitions(data_source_runtime_host_object_ingest_test PRIVATE
        PJ_RUNTIME_HOST_OBJECT_PARSER_PATH="$<TARGET_FILE:runtime_host_object_parser_plugin>"
    )
    target_compile_options(data_source_runtime_host_object_ingest_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(data_source_runtime_host_object_ingest_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_dependencies(data_source_runtime_host_object_ingest_test runtime_host_object_parser_plugin)
    add_test(NAME DataSourceRuntimeHostObjectIngestTest COMMAND data_source_runtime_host_object_ingest_test)
endif()
