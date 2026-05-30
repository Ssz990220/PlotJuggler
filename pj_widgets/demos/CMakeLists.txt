# Standalone visual demos for pj_widgets. These are dev tools, not part
# of the shipped app. Built unconditionally with the rest of the tree —
# they're tiny and depend only on pj_widgets + Qt.

# Shared demo-internal QSS preprocessor — both demos parse the
# stylesheet_*.qss palette + body the same way; this OBJECT library
# keeps the implementation in one place. PJ_QSS_DIR is set on the
# object lib so the absolute resources path bakes in once.
add_library(pj_widgets_demos_qss OBJECT qss_preprocessor.cpp)
target_compile_features(pj_widgets_demos_qss PRIVATE cxx_std_20)
target_compile_options(pj_widgets_demos_qss PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(pj_widgets_demos_qss PRIVATE
    PJ_QSS_DIR="${CMAKE_SOURCE_DIR}/resources"
)
target_link_libraries(pj_widgets_demos_qss PUBLIC
    Qt6::Core
    Qt6::Widgets
)

add_executable(pj_widgets_messagebox_demo
    messagebox_demo.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(pj_widgets_messagebox_demo PROPERTIES AUTORCC ON)
target_compile_features(pj_widgets_messagebox_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_messagebox_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_messagebox_demo PRIVATE
    pj_widgets
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

add_executable(pj_widgets_combobox_demo
    combobox_demo.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
set_target_properties(pj_widgets_combobox_demo PROPERTIES AUTORCC ON)
target_compile_features(pj_widgets_combobox_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_combobox_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_combobox_demo PRIVATE
    pj_widgets
    pj_widgets_demos_qss
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

# Dialog building blocks (SDK 0.4.0): RangeSlider + DateRangePicker. No QSS or
# resources — links only pj_widgets + Qt6::Widgets.
add_executable(pj_widgets_dialog_blocks_demo dialog_blocks_demo.cpp)
target_compile_features(pj_widgets_dialog_blocks_demo PRIVATE cxx_std_20)
target_compile_options(pj_widgets_dialog_blocks_demo PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_widgets_dialog_blocks_demo PRIVATE
    pj_widgets
    Qt6::Widgets
)
