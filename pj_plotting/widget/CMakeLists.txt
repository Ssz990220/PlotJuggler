find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Xml)

add_library(pj_plotting STATIC
    src/CurveEditor.cpp
    src/CurveEditor.ui
    src/CurveTracker.cpp
    src/DockToolbar.cpp
    src/DockToolbar.ui
    src/DockWidget.cpp
    src/PlotDocker.cpp
    src/PlotFocusOverlay.cpp
    src/PlotLegend.cpp
    src/PlotMagnifier.cpp
    src/PlotPanner.cpp
    src/PlotWidget.cpp
    src/PlotWidgetBase.cpp
    src/PlotZoomer.cpp
    src/TabbedPlotWidget.cpp
    include/pj_plotting/CurveEditor.h
    include/pj_plotting/CurveTracker.h
    include/pj_plotting/DockToolbar.h
    include/pj_plotting/DockWidget.h
    include/pj_plotting/PlotDocker.h
    include/pj_plotting/PlotFocusOverlay.h
    include/pj_plotting/PlotLegend.h
    include/pj_plotting/PlotMagnifier.h
    include/pj_plotting/PlotPanner.h
    include/pj_plotting/PlotWidget.h
    include/pj_plotting/PlotWidgetBase.h
    include/pj_plotting/PlotZoomer.h
    include/pj_plotting/TabbedPlotWidget.h
)

set_target_properties(pj_plotting PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_include_directories(pj_plotting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_plotting PUBLIC
    pj_plotting_core
    pj_runtime
    plotjuggler_qwt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Xml
  PRIVATE
    pj_widgets
)
