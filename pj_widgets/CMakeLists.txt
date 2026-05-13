find_package(Qt6 REQUIRED COMPONENTS Core Gui Svg Widgets)

add_library(pj_widgets STATIC
    src/ColorPickerPopup.cpp
    src/CurveTreeView.cpp
    src/RecentFilesMenu.cpp
    include/pj_widgets/ColorPickerPopup.h
    include/pj_widgets/CurveTreeView.h
    include/pj_widgets/RealSlider.h
    include/pj_widgets/RecentFilesMenu.h
    include/pj_widgets/SvgUtil.h
)

set_target_properties(pj_widgets PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_widgets PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Svg
    Qt6::Widgets
)

if(PJ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
