find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(pj_plotting_core STATIC
    src/DatastoreCurveAdapter.cpp
    src/PointSeriesXY.cpp
    include/pj_plotting/DatastoreCurveAdapter.h
    include/pj_plotting/PointSeriesXY.h
)

target_include_directories(pj_plotting_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pj_plotting_core PUBLIC
    pj_base
    pj_runtime
    pj_datastore
    plotjuggler_qwt
    Qt6::Core
)
