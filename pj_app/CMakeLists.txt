find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test)

# backward-cpp (Conan): symbolized crash backtraces. The package exports the
# `Backward` config and the `Backward::Backward` target, which propagates the
# BACKWARD_HAS_* defines and links the stack-detail backend (elfutils/libdw)
# transitively. main.cpp installs the signal handler.
find_package(Backward REQUIRED)

add_executable(pj_app
    src/main.cpp
    src/DialogPresenter.cpp
    src/DialogPresenter.h
    src/FanoutConfig.cpp
    src/FanoutConfig.h
    src/FileLoader.cpp
    src/FileLoader.h
    src/StreamingSourceManager.cpp
    src/StreamingSourceManager.h
    src/LayoutXml.cpp
    src/LayoutXml.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/WidgetTuner.cpp
    src/WidgetTuner.h
    src/KeySequence.cpp
    src/KeySequence.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/PreferencesDialog.ui
    src/PreferencesNavRow.cpp
    src/PreferencesNavRow.h
    src/Theme.cpp
    src/Theme.h
    src/DebugUi.cpp
    src/DebugUi.h
    src/TitleBar.cpp
    src/TitleBar.h
    src/TitleBar.ui
    src/ui/CurveListPanel.cpp
    src/ui/CurveListPanel.h
    src/ui/CurveListPanel.ui
    src/ui/Scene2DConfigPanel.cpp
    src/ui/Scene2DConfigPanel.h
    src/ui/Scene3DConfigPanel.cpp
    src/ui/Scene3DConfigPanel.h
    src/ui/LeftPanel.cpp
    src/ui/LeftPanel.h
    src/ui/LeftPanel.ui
    src/ui/DiagnosticsCard.cpp
    src/ui/DiagnosticsCard.h
    src/ui/DiagnosticsCard.ui
    src/ui/DiagnosticsPopup.cpp
    src/ui/DiagnosticsPopup.h
    src/ui/DiagnosticsPopup.ui
    src/ui/DiagnosticsDetailDialog.cpp
    src/ui/DiagnosticsDetailDialog.h
    src/ui/DiagnosticsDetailDialog.ui
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
    src/ui/AboutDialog.ui
    src/ui/TimelineWidget.cpp
    src/ui/TimelineWidget.h
    src/ui/TimelineWidget.ui
    src/RasterKeyMap.cpp
    src/RasterKeyMap.h
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

set_target_properties(pj_app PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(pj_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/raster_ipc
)

target_link_libraries(pj_app PRIVATE
    pj_runtime
    pj_widgets
    pj_plotting
    pj_scene2d_widgets
    pj_scene3d_widgets
    pj_marketplace_ui
    pj_dialog_engine_qt
    qt_advanced_docking
    Backward::Backward
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Xml
)

# -Werror guards against the bug class that bit the reload path: a merge that
# drops wiring leaves an unused variable behind, which these flags turn into a
# build failure instead of a silent regression.
if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_app PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Don't re-export symbols pulled in from static archive members. Without
# this, Conan-built libpng/libjpeg/etc. symbols are exported globally from
# pj_app and the dynamic linker can interpose them ahead of the system
# copies that GTK/glib load — see for example the libpng 1.6.43 (system)
# vs 1.6.58 (Conan) ABI skew that crashes QGtk3Interface::fileIcon during
# QFileDialog with an "invalid pointer" free. --exclude-libs,ALL is the
# GNU/LLD spelling; harmless on other linkers but only meaningful here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT WIN32)
    target_link_options(pj_app PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)

    # Pure config-parsing logic (no GUI), compiled standalone so it's linkable
    # from a plain gtest binary. FanoutConfig.cpp is also part of pj_app itself.
    add_executable(fanout_config_test
        tests/fanout_config_test.cpp
        src/FanoutConfig.cpp
    )
    target_include_directories(fanout_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fanout_config_test PRIVATE
        Qt6::Core
        GTest::gtest_main
    )
    add_test(NAME FanoutConfig COMMAND fanout_config_test)

    # LayoutXml is a self-contained DOM helper module (no Qt::Widgets, no
    # MainWindow) so we compile the source directly into the test target
    # instead of linking against the pj_app executable.
    add_executable(layout_xml_test
        tests/layout_xml_test.cpp
        src/LayoutXml.cpp
        src/LayoutXml.h
    )
    target_include_directories(layout_xml_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(layout_xml_test PRIVATE
        Qt6::Core
        Qt6::Xml
        GTest::gtest_main
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(layout_xml_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME LayoutXmlTest COMMAND layout_xml_test)

    add_executable(key_sequence_matcher_test
        tests/key_sequence_matcher_test.cpp
        src/KeySequence.cpp
    )
    set_target_properties(key_sequence_matcher_test PROPERTIES AUTOMOC ON)
    target_include_directories(key_sequence_matcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(key_sequence_matcher_test PRIVATE
        Qt6::Core
        Qt6::Gui
        GTest::gtest_main
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(key_sequence_matcher_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME KeySequenceMatcher COMMAND key_sequence_matcher_test)

    add_executable(key_sequence_watcher_test
        tests/key_sequence_watcher_test.cpp
        src/KeySequence.cpp
    )
    set_target_properties(key_sequence_watcher_test PROPERTIES AUTOMOC ON)
    target_include_directories(key_sequence_watcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(key_sequence_watcher_test PRIVATE
        Qt6::Core
        Qt6::Gui
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(key_sequence_watcher_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME KeySequenceWatcher COMMAND key_sequence_watcher_test)

    add_executable(raster_ipc_protocol_test tests/raster_ipc_protocol_test.cpp)
    target_include_directories(raster_ipc_protocol_test PRIVATE ${CMAKE_SOURCE_DIR}/raster_ipc)
    target_link_libraries(raster_ipc_protocol_test PRIVATE GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(raster_ipc_protocol_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME RasterIpcProtocol COMMAND raster_ipc_protocol_test)

    add_executable(raster_key_map_test
        tests/raster_key_map_test.cpp
        src/RasterKeyMap.cpp
    )
    target_include_directories(raster_key_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(raster_key_map_test PRIVATE Qt6::Core GTest::gtest_main)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(raster_key_map_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME RasterKeyMap COMMAND raster_key_map_test)

    # End-to-end FileLoader pipeline test (same-file reload semantics). Compiles
    # FileLoader + its two helper TUs directly (FileLoader has no MainWindow
    # dependency by design — see setFilePicker) and ingests through the SDK's
    # mock_file_source_plugin, staged into a temp extensions dir at runtime.
    add_executable(file_loader_test
        tests/file_loader_test.cpp
        src/FileLoader.cpp
        src/FileLoader.h
        src/DialogPresenter.cpp
        src/DialogPresenter.h
        src/FanoutConfig.cpp
        src/FanoutConfig.h
    )
    set_target_properties(file_loader_test PROPERTIES AUTOMOC ON)
    target_include_directories(file_loader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(file_loader_test PRIVATE
        PJ_MOCK_FILE_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mock_file_source_plugin>"
    )
    # $<TARGET_FILE:...> in a compile definition does not create a build-order
    # dependency; the plugin must exist when the test runs.
    add_dependencies(file_loader_test mock_file_source_plugin)
    target_link_libraries(file_loader_test PRIVATE
        pj_runtime
        pj_widgets
        pj_scene3d_widgets
        pj_dialog_engine_qt
        Qt6::Widgets
        GTest::gtest
    )
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(file_loader_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME FileLoaderTest COMMAND file_loader_test)
endif()
