#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QMetaObject>
#include <QString>
#include <QWidget>

#include "pj_plotting/PlotWidgetBase.h"
#include "pj_widgets/ChromeMetrics.h"

class QListWidgetItem;
class QPushButton;

namespace Ui {
class CurveEditor;
}

namespace PJ {

class ColorPickerPopup;
class PlotWidget;

// Side panel listing the curves of a single PlotWidget. One row per
// curve: color swatch (click for picker) | name | visibility eye |
// trash. Header strip has a Datasets-style filter + kebab popup with a
// "Clear all curves" action.
//
// Bind via setPlot(plot) / setPlot(nullptr). The panel auto-refreshes
// when the bound plot's curveListChanged() fires; a destroyed plot is
// detected via a QObject::destroyed connection.
//
// Width / Line style controls live in the global right-toolbar strip
// (MainWindow), not here.
class CurveEditor : public QWidget {
  Q_OBJECT
 public:
  explicit CurveEditor(QWidget* parent = nullptr);
  ~CurveEditor() override;

  void setPlot(PlotWidget* plot);

  [[nodiscard]] PlotWidget* plot() const noexcept {
    return plot_;
  }

 public slots:
  void refresh();
  // Updates the per-row visibility / trash + header icons to the new
  // theme's ink.
  void onStylesheetChanged(QString theme);
  // Rebinds the header band's chrome dimensions (search + filter +
  // kebab) so they grow with the global chrome-metrics setting. The
  // band grows to (icon_size + icon_padding) + 2 * layout_padding tall
  // with layout_padding applied as contentsMargins, and layout_spacing
  // is forwarded to QListWidget::setSpacing so the curve rows gain
  // visible gaps. Per-row visibility / trash icons keep their compact
  // 20-px row height — they're a deliberate dense-list design.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  void onFilterChanged(const QString& text);

 protected:
  // Watches the curves-filter QLineEdit so its FocusIn / FocusOut events
  // can swap label visibility, mirroring the Datasets header. Also watches
  // the listWidget viewport so row names can re-elide on resize.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Drives the progressive collapse of the Curves header (search +
  // filter + label hide when too narrow; kebab is always present).
  // Driven off the panel's own width because the header band's QWidget
  // can't actually shrink below the sum of its content's natural sizes.
  void resizeEvent(QResizeEvent* event) override;

 private:
  // Re-applies the active filter text to the list items (case-insensitive
  // substring match against the row's curve name). Called after refresh().
  void applyFilter();
  // Hides search + filter + label when the header band is too narrow,
  // keeping the kebab always reachable.
  void updateHeaderForWidth();
  void appendRow(const QString& curve_key, const QString& display_name, QColor color, bool visible);
  void onSwatchClicked(const QString& curve_name, QPushButton* swatch);
  void onPickerColorChanged(QColor color);
  void onCurveColorChanged(const QString& curve_name, QColor color);
  void onVisibilityToggled(const QString& curve_name, bool visible);
  void clearActivePicker();

  Ui::CurveEditor* ui_;
  PlotWidget* plot_ = nullptr;
  // Tracks the active theme so per-row visibility-toggle icons can be
  // rendered in the right ink at row-creation time without having to
  // walk back up to qApp / Theme. Updated via onStylesheetChanged.
  QString current_theme_ = QStringLiteral("light");
  // Row-pixel height for curve rows. Tracks icon_size so larger icons
  // don't clip; updated via onChromeMetricsChanged. Default matches
  // MainWindow's first-launch icon size and the original compact list.
  int row_height_ = 20;
  QMetaObject::Connection curve_list_connection_;
  QMetaObject::Connection curve_color_connection_;
  QMetaObject::Connection plot_destroyed_connection_;

  // Lazily created on first swatch click; reused for the lifetime of the
  // editor so the persistent colorChanged connection survives reuse.
  ColorPickerPopup* color_picker_ = nullptr;
  // Identifies which row's swatch the popup is currently editing. Cleared
  // whenever the row is destroyed (refresh / setPlot / plot teardown).
  QString active_color_curve_;
};

}  // namespace PJ
