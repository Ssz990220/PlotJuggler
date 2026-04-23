cmake_minimum_required(VERSION 3.16)

project(QtADS LANGUAGES CXX)

include_directories(include/Qads)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)
if (UNIX AND NOT APPLE)
    if(QT_VERSION_MAJOR LESS 6)
        find_package(Qt5 5.5 COMPONENTS X11Extras REQUIRED)
    else()
        find_package(Qt6 REQUIRED COMPONENTS Gui)
    endif()
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(ads_SRCS
    src/ads_globals.cpp
    src/DockAreaTabBar.cpp
    src/DockAreaTitleBar.cpp
    src/DockAreaWidget.cpp
    src/DockContainerWidget.cpp
    src/DockManager.cpp
    src/DockOverlay.cpp
    src/DockSplitter.cpp
    src/DockWidget.cpp
    src/DockWidgetTab.cpp
    src/DockingStateReader.cpp
    src/DockFocusController.cpp
    src/ElidingLabel.cpp
    src/FloatingDockContainer.cpp
    src/FloatingDragPreview.cpp
    src/IconProvider.cpp
    src/DockComponentsFactory.cpp
    src/AutoHideSideBar.cpp
    src/AutoHideTab.cpp
    src/AutoHideDockContainer.cpp
    src/PushButton.cpp
    src/ResizeHandle.cpp

    src/ads.qrc

    include/Qads/ads_globals.h
    include/Qads/DockAreaTabBar.h
    include/Qads/DockAreaTitleBar.h
    include/Qads/DockAreaTitleBar_p.h
    include/Qads/DockAreaWidget.h
    include/Qads/DockContainerWidget.h
    include/Qads/DockManager.h
    include/Qads/DockOverlay.h
    include/Qads/DockSplitter.h
    include/Qads/DockWidget.h
    include/Qads/DockWidgetTab.h
    include/Qads/DockingStateReader.h
    include/Qads/DockFocusController.h
    include/Qads/ElidingLabel.h
    include/Qads/FloatingDockContainer.h
    include/Qads/FloatingDragPreview.h
    include/Qads/IconProvider.h
    include/Qads/DockComponentsFactory.h
    include/Qads/AutoHideSideBar.h
    include/Qads/AutoHideTab.h
    include/Qads/AutoHideDockContainer.h
    include/Qads/PushButton.h
    include/Qads/ResizeHandle.h
)

if (UNIX AND NOT APPLE)
    set(ads_SRCS
        src/linux/FloatingWidgetTitleBar.cpp
        src/linux/FloatingWidgetTitleBar.h
        ${ads_SRCS}
    )
endif()


add_library(qt_advanced_docking STATIC ${ads_SRCS} )
target_compile_definitions(qt_advanced_docking PUBLIC ADS_STATIC)

target_include_directories(qt_advanced_docking PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/Qads>
)

target_link_libraries(qt_advanced_docking PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
)

if(UNIX AND NOT APPLE)
    if(QT_VERSION_MAJOR LESS 6)
        target_include_directories(qt_advanced_docking PRIVATE ${Qt5Gui_PRIVATE_INCLUDE_DIRS})
        target_link_libraries(qt_advanced_docking PUBLIC Qt5::X11Extras)
    else()
        # Qt6: needs GuiPrivate for qpa/qplatformnativeinterface.h
        target_link_libraries(qt_advanced_docking PRIVATE Qt6::GuiPrivate)
    endif()
    target_link_libraries(qt_advanced_docking PRIVATE xcb)
endif()

set_target_properties(qt_advanced_docking PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)



