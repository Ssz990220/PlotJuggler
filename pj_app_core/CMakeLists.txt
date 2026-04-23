find_package(Qt6 REQUIRED COMPONENTS Core Gui Svg)

add_library(pj_app_core STATIC
    src/AppSession.cpp
    src/CatalogModel.cpp
    src/PlaybackEngine.cpp
    src/SessionManager.cpp
    include/pj_app_core/AppSession.h
    include/pj_app_core/CatalogModel.h
    include/pj_app_core/IDataWidget.h
    include/pj_app_core/PlaybackEngine.h
    include/pj_app_core/SessionManager.h
    include/pj_app_core/SvgUtil.h
)

set_target_properties(pj_app_core PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_app_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_app_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Svg
)
