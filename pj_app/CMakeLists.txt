find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

add_executable(pj_app
    src/main.cpp
    src/DialogPresenter.cpp
    src/DialogPresenter.h
    src/FileLoader.cpp
    src/FileLoader.h
    src/LoadFileDialog.cpp
    src/LoadFileDialog.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/Dialog.cpp
    src/Dialog.h
    src/Dialog.ui
    src/MessageBox.cpp
    src/MessageBox.h
    src/SaveFileDialog.cpp
    src/SaveFileDialog.h
    src/Style.h
    src/WidgetTuner.cpp
    src/WidgetTuner.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/PreferencesDialog.ui
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
    src/ui/ToggleSwitch.cpp
    src/ui/ToggleSwitch.h
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
    src/ui/DoubleScrubber.cpp
    src/ui/DoubleScrubber.h
    src/ui/IntScrubber.cpp
    src/ui/IntScrubber.h
    src/ui/ScrubberBase.cpp
    src/ui/ScrubberBase.h
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
    pj_marketplace_ui
    pj_dialog_engine_qt
    qt_advanced_docking
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Xml
)
