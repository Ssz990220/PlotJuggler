find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)
if(PJ_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

add_library(pj_plot_widgets STATIC
    src/CurveTracker.cpp
    src/DatastoreCurveAdapter.cpp
    src/DockToolbar.cpp
    src/DockToolbar.ui
    src/DockWidget.cpp
    src/PlotLegend.cpp
    src/PlotMagnifier.cpp
    src/PlotPanner.cpp
    src/PointSeriesXY.cpp
    src/PlotDocker.cpp
    src/PlotWidget.cpp
    src/PlotWidgetBase.cpp
    src/PlotZoomer.cpp
    src/TabbedPlotWidget.cpp
    include/pj_plot_widgets/DatastoreCurveAdapter.h
    include/pj_plot_widgets/CurveTracker.h
    include/pj_plot_widgets/DockToolbar.h
    include/pj_plot_widgets/DockWidget.h
    include/pj_plot_widgets/PlotLegend.h
    include/pj_plot_widgets/PlotMagnifier.h
    include/pj_plot_widgets/PlotPanner.h
    include/pj_plot_widgets/PointSeriesXY.h
    include/pj_plot_widgets/PlotDocker.h
    include/pj_plot_widgets/PlotWidget.h
    include/pj_plot_widgets/PlotWidgetBase.h
    include/pj_plot_widgets/PlotZoomer.h
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
    plotjuggler_qwt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Xml
)

if(PJ_BUILD_TESTS)
    add_executable(datastore_curve_adapter_test
        tests/datastore_curve_adapter_test.cpp
    )
    target_link_libraries(datastore_curve_adapter_test PRIVATE
        pj_plot_widgets
        pj_datastore
        GTest::gtest_main
    )
    add_test(NAME DatastoreCurveAdapterTest COMMAND datastore_curve_adapter_test)
endif()
