#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot.h>

#include <string>
#include <utility>
#include <vector>

class QEvent;
class QObject;
class QwtPlotZoomer;

namespace PJ {

/// Lightweight chart widget that renders named XY line series inside a QFrame.
/// Created and managed by the widget binding layer — plugin authors never touch
/// this directly. Built on the vendored Qwt (not Qt Charts), so the dialog host
/// carries no Qt6::Charts dependency; the dialog-protocol chart API
/// (setChartSeries / onChartViewChanged) is unaffected.
class ChartPreviewWidget : public QwtPlot {
  Q_OBJECT

 public:
  explicit ChartPreviewWidget(QWidget* parent = nullptr);

  struct Series {
    std::string label;
    std::vector<std::pair<double, double>> points;
    std::string color;  // optional hex "#rrggbb"; empty means use the built-in palette
  };

  void setSeries(const std::vector<Series>& series);
  void clearSeries();

  /// Enable or disable interactive zoom (rubber band + mouse wheel).
  /// When enabled, viewChanged() is emitted whenever the user zooms or pans.
  void setZoomEnabled(bool enabled);

 signals:
  /// Emitted when the visible axes range changes due to user zoom or pan.
  /// Only emitted when zoom is enabled via setZoomEnabled(true).
  void viewChanged(double x_min, double x_max, double y_min, double y_max);

 protected:
  /// Wheel zoom is implemented here because the plot *canvas* (a child widget),
  /// not this widget, receives wheel events — a wheelEvent() override never fires.
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  QwtPlotZoomer* zoomer_ = nullptr;
  bool zoom_enabled_ = false;

  void emitViewChanged();
};

}  // namespace PJ
