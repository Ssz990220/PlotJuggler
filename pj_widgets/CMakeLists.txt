find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Svg Widgets)

add_library(pj_widgets STATIC
    src/ColorPickerPopup.cpp
    src/ComboBox.cpp
    src/ComboBoxGradientDelegate.cpp
    src/ConfigPanelHost.cpp
    src/CredentialsEditor.cpp
    src/CurveTreeView.cpp
    src/DateRangePicker.cpp
    src/Dialog.cpp
    src/Dialog.ui
    src/DoubleScrubber.cpp
    src/FileDialog.cpp
    src/FlowLayout.cpp
    src/IntScrubber.cpp
    src/LayerListView.cpp
    src/MessageBox.cpp
    src/ProgressDialog.cpp
    src/RangeSlider.cpp
    src/RasterFrame.cpp
    src/RasterStreamView.cpp
    src/RecentFilesMenu.cpp
    src/ScrubberBase.cpp
    src/SectionHeaderBand.cpp
    src/ToggleSwitch.cpp
    src/VisualizationPlaceholderWidget.cpp
    include/pj_widgets/ChromeMetrics.h
    include/pj_widgets/ColorPickerPopup.h
    include/pj_widgets/ComboBox.h
    include/pj_widgets/ComboBoxGradientDelegate.h
    include/pj_widgets/ConfigPanelHost.h
    include/pj_widgets/CredentialsEditor.h
    include/pj_widgets/CurveTreeView.h
    include/pj_widgets/DateRangePicker.h
    include/pj_widgets/Dialog.h
    include/pj_widgets/DoubleScrubber.h
    include/pj_widgets/ElidingLabel.h
    include/pj_widgets/FileDialog.h
    include/pj_widgets/FlowLayout.h
    include/pj_widgets/IntScrubber.h
    include/pj_widgets/LayerListView.h
    include/pj_widgets/MessageBox.h
    include/pj_widgets/ProgressDialog.h
    include/pj_widgets/RealSlider.h
    include/pj_widgets/RangeSlider.h
    include/pj_widgets/RasterFrame.h
    include/pj_widgets/RasterStreamView.h
    include/pj_widgets/RecentFilesMenu.h
    include/pj_widgets/ScrubberBase.h
    include/pj_widgets/SectionHeaderBand.h
    include/pj_widgets/Style.h
    include/pj_widgets/SvgUtil.h
    include/pj_widgets/ThemeColors.h
    include/pj_widgets/ToggleSwitch.h
    include/pj_widgets/VisualizationPlaceholderWidget.h
)

set_target_properties(pj_widgets PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(pj_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/raster_ipc>
)

target_link_libraries(pj_widgets PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Svg
    Qt6::Widgets
)

if(PJ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_subdirectory(demos)

option(PJ_BUILD_WIDGET_DEMOS "Build pj_widgets demo apps" ${PJ_BUILD_TESTS})
if(PJ_BUILD_WIDGET_DEMOS)
    add_subdirectory(demo)
endif()
