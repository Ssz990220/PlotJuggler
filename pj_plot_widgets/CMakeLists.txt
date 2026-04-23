find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(pj_plot_widgets STATIC
    src/DockToolbar.cpp
    src/DockToolbar.ui
    src/DockWidget.cpp
    src/PlotDocker.cpp
    src/TabbedPlotWidget.cpp
    include/pj_plot_widgets/DockToolbar.h
    include/pj_plot_widgets/DockWidget.h
    include/pj_plot_widgets/PlotDocker.h
    include/pj_plot_widgets/TabbedPlotWidget.h
)

set_target_properties(pj_plot_widgets PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_include_directories(pj_plot_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_plot_widgets PUBLIC
    pj_app_core
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
