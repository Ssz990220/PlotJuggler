#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>
#include <functional>
#include <pj_plugins/host/widget_data_view.hpp>
#include <string>
#include <string_view>

namespace PJ {

/// Callback for widget events: receives widget objectName + event JSON string.
using WidgetEventCallback = std::function<void(const std::string& widget_name, const std::string& event_json)>;

/// Resolve a plugin-supplied semantic icon id (setButtonIconNamed) to a themed
/// SVG resource path, or an empty QString for an unknown id (callers then leave
/// the widget icon untouched). The supported id set lives here as the single
/// source of truth so it can be unit-tested without constructing a dialog.
QString resolveNamedIconPath(std::string_view icon_name);

class AppSession;
class CatalogModel;

/// Apply widget data from a WidgetDataView to all matching child widgets of root.
/// Uses QSignalBlocker to prevent re-entrant signal firing during updates.
/// When session and catalog are provided, QFrame chart containers use a full
/// PlotWidget (zoom/tracker/legend) instead of ChartPreviewWidget.
/// Styled-widget adaptation (QRadioButton/QCheckBox/QComboBox → PJ controls)
/// lives in widget_adapters.hpp; the engines call adaptStyledWidgets() once
/// after loading the .ui, and applyWidgetData keeps the replacements in sync.
void applyWidgetData(
    QWidget* root, const PJ::WidgetDataView& view, AppSession* session = nullptr, CatalogModel* catalog = nullptr);

/// Connect primary change signals of all editable widgets under root
/// to the given callback. The callback receives the widget objectName and
/// an event JSON string built by WidgetEventBuilder.
void connectWidgetSignals(QWidget* root, WidgetEventCallback callback);

/// Create QShortcut objects for QPushButtons that declare a "shortcut" key
/// in the widget data. Each shortcut triggers click() on the target button.
/// Call once after the dialog is fully constructed and signals are connected.
void installButtonShortcuts(QWidget* root, const PJ::WidgetDataView& view);

}  // namespace PJ
