find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(pj_widgets STATIC
    src/ColorPickerPopup.cpp
    include/pj_widgets/ColorPickerPopup.h
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
    Qt6::Widgets
)
