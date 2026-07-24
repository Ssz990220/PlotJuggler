find_package(Qt6 REQUIRED COMPONENTS Core Xml)

add_library(pj_plotting_core STATIC
    src/DatastoreCurveAdapter.cpp
    src/FilteredCurveAdapter.cpp
    src/PointSeriesXY.cpp
    src/SnapshotGroupResolver.cpp
    src/SnapshotSeriesData.cpp
    include/pj_plotting/DatastoreCurveAdapter.h
    include/pj_plotting/FilteredCurveAdapter.h
    include/pj_plotting/PlotXml.h
    include/pj_plotting/PointSeriesXY.h
    include/pj_plotting/SnapshotGroupResolver.h
    include/pj_plotting/SnapshotSeriesData.h
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
    Qt6::Xml
)
