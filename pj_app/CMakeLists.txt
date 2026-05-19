find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

add_executable(pj_app
    src/main.cpp
    src/DialogPresenter.cpp
    src/DialogPresenter.h
    src/FileLoader.cpp
    src/FileLoader.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/WidgetTuner.cpp
    src/WidgetTuner.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/PreferencesDialog.ui
    src/PreferencesNavRow.cpp
    src/PreferencesNavRow.h
    src/Theme.cpp
    src/Theme.h
    src/DebugUi.cpp
    src/DebugUi.h
    src/TitleBar.cpp
    src/TitleBar.h
    src/TitleBar.ui
    src/ui/CurveListPanel.cpp
    src/ui/CurveListPanel.h
    src/ui/CurveListPanel.ui
    src/ui/LeftPanel.cpp
    src/ui/LeftPanel.h
    src/ui/LeftPanel.ui
    src/ui/DiagnosticsCard.cpp
    src/ui/DiagnosticsCard.h
    src/ui/DiagnosticsCard.ui
    src/ui/DiagnosticsPopup.cpp
    src/ui/DiagnosticsPopup.h
    src/ui/DiagnosticsPopup.ui
    src/ui/DiagnosticsDetailDialog.cpp
    src/ui/DiagnosticsDetailDialog.h
    src/ui/DiagnosticsDetailDialog.ui
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
    pj_runtime
    pj_widgets
    pj_plotting
    pj_scene2d_widgets
    pj_marketplace_ui
    pj_dialog_engine_qt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Xml
)

# Don't re-export symbols pulled in from static archive members. Without
# this, Conan-built libpng/libjpeg/etc. symbols are exported globally from
# pj_app and the dynamic linker can interpose them ahead of the system
# copies that GTK/glib load — see for example the libpng 1.6.43 (system)
# vs 1.6.58 (Conan) ABI skew that crashes QGtk3Interface::fileIcon during
# QFileDialog with an "invalid pointer" free. --exclude-libs,ALL is the
# GNU/LLD spelling; harmless on other linkers but only meaningful here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT WIN32)
    target_link_options(pj_app PRIVATE "LINKER:--exclude-libs,ALL")
endif()
