add_executable(pj_app
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
)

set_target_properties(pj_app PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

target_link_libraries(pj_app PRIVATE
    pj_app_core
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
