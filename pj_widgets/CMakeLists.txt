find_package(Qt6 REQUIRED COMPONENTS Core Gui Svg Widgets)

add_library(pj_widgets STATIC
    src/ColorPickerPopup.cpp
    src/CurveTreeView.cpp
    src/Dialog.cpp
    src/Dialog.ui
    src/DoubleScrubber.cpp
    src/FlowLayout.cpp
    src/IntScrubber.cpp
    src/MessageBox.cpp
    src/RecentFilesMenu.cpp
    src/SaveFileDialog.cpp
    src/ScrubberBase.cpp
    src/ToggleSwitch.cpp
    include/pj_widgets/ColorPickerPopup.h
    include/pj_widgets/CurveTreeView.h
    include/pj_widgets/Dialog.h
    include/pj_widgets/DoubleScrubber.h
    include/pj_widgets/ElidingLabel.h
    include/pj_widgets/FlowLayout.h
    include/pj_widgets/IntScrubber.h
    include/pj_widgets/MessageBox.h
    include/pj_widgets/RealSlider.h
    include/pj_widgets/RecentFilesMenu.h
    include/pj_widgets/SaveFileDialog.h
    include/pj_widgets/ScrubberBase.h
    include/pj_widgets/Style.h
    include/pj_widgets/SvgUtil.h
    include/pj_widgets/ToggleSwitch.h
)

set_target_properties(pj_widgets PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
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
