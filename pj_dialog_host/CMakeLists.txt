find_package(Qt6 REQUIRED COMPONENTS Widgets UiTools Charts SvgWidgets)

# Workaround: some Qt packages still publish AGL in transitive OpenGL links,
# but that framework was removed in newer macOS SDKs.
foreach(_target WrapOpenGL::WrapOpenGL OpenGL::GL)
    if(TARGET ${_target})
        get_target_property(_libs ${_target} INTERFACE_LINK_LIBRARIES)
        if(_libs)
            string(REPLACE "-framework AGL" "" _libs "${_libs}")
            list(REMOVE_ITEM _libs "AGL")
            foreach(_lib ${_libs})
                if(_lib MATCHES "AGL")
                    list(REMOVE_ITEM _libs "${_lib}")
                endif()
            endforeach()
            set_target_properties(${_target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
        endif()
    endif()
endforeach()

add_library(pj_dialog_engine_qt STATIC
    src/widget_binding.cpp
    src/dialog_engine.cpp
    src/chart_preview_widget.cpp
    include/pj_plugins/host_qt/chart_preview_widget.hpp
    include/pj_plugins/host_qt/dialog_engine.hpp
    include/pj_plugins/host_qt/drop_event_filter.hpp
    include/pj_plugins/host_qt/widget_binding.hpp
)

set_target_properties(pj_dialog_engine_qt PROPERTIES
    AUTOMOC ON
)

target_compile_features(pj_dialog_engine_qt PUBLIC cxx_std_20)
target_compile_options(pj_dialog_engine_qt PRIVATE ${PJ_WARNING_FLAGS})

target_include_directories(pj_dialog_engine_qt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pj_dialog_engine_qt
    PUBLIC
        pj_dialog_host
        Qt6::Widgets
        Qt6::UiTools
        Qt6::Charts
        Qt6::SvgWidgets
)

if(PJ_BUILD_TESTS)
    add_executable(dialog_engine_test
        tests/dialog_engine_test.cpp
        ${CMAKE_SOURCE_DIR}/plotjuggler_core/pj_plugins/dialog_protocol/examples/mock_dialog.cpp
    )
    target_compile_options(dialog_engine_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(dialog_engine_test
        PRIVATE
            pj_dialog_engine_qt
            pj_dialog_sdk
            GTest::gtest
    )
    add_test(NAME dialog_engine_test COMMAND dialog_engine_test)
    set_tests_properties(dialog_engine_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
