add_executable(pj_app
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/ui/CurveListPanel.cpp
    src/ui/CurveListPanel.h
    src/ui/CurveListPanel.ui
    src/ui/CurveTreeView.cpp
    src/ui/CurveTreeView.h
    src/ui/LeftPanel.cpp
    src/ui/LeftPanel.h
    src/ui/LeftPanel.ui
    src/ui/LineEdit.h
    src/ui/RealSlider.h
    src/ui/TimelineWidget.cpp
    src/ui/TimelineWidget.h
    src/ui/TimelineWidget.ui
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
)

target_link_libraries(pj_app PRIVATE
    pj_app_core
    pj_plot_widgets
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
