add_executable(dock_drag_repro main.cpp)

set_target_properties(dock_drag_repro PROPERTIES
    AUTOMOC ON
)

target_link_libraries(dock_drag_repro PRIVATE
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
