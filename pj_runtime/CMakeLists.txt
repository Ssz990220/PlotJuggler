find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(nlohmann_json REQUIRED)
find_package(tsl-robin-map REQUIRED)
if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

add_library(pj_runtime STATIC
    src/AppSession.cpp
    src/CatalogModel.cpp
    src/DataSourceRuntimeHost.cpp
    src/DiagnosticHistory.cpp
    src/ExtensionCatalogService.cpp
    src/PlaybackEngine.cpp
    src/SessionManager.cpp
    include/pj_runtime/AppSession.h
    include/pj_runtime/CatalogModel.h
    include/pj_runtime/CurveDescriptor.h
    include/pj_runtime/DataSourceRuntimeHost.h
    include/pj_runtime/DiagnosticHistory.h
    include/pj_runtime/ExtensionCatalogService.h
    include/pj_runtime/IDataWidget.h
    include/pj_runtime/PlaybackEngine.h
    include/pj_runtime/SessionManager.h
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
    pj_datastore
    pj_marketplace
    pj_plugin_runtime_catalog
    nlohmann_json::nlohmann_json
  PRIVATE
    tsl::robin_map
    fmt::fmt
)

if(PJ_BUILD_TESTS)
    add_executable(app_session_test
        tests/app_session_test.cpp
    )
    target_link_libraries(app_session_test PRIVATE
        pj_runtime
        GTest::gtest_main
    )
    add_test(NAME AppSessionTest COMMAND app_session_test)
endif()
