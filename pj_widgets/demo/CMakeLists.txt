add_executable(pj_widgets_demo main.cpp)

target_link_libraries(pj_widgets_demo PRIVATE
    pj_widgets
    Qt6::Widgets
)
