add_library(pj_app_core STATIC
    src/AppSession.cpp
    include/pj_app_core/AppSession.h
)

set_target_properties(pj_app_core PROPERTIES
    AUTOMOC ON
)

target_include_directories(pj_app_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_app_core PUBLIC
    Qt6::Core
)
