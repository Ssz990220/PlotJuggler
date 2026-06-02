#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot.h>
#include <qwt_plot_magnifier.h>

#include <QPointF>

namespace PJ {

class PlotMagnifier : public QwtPlotMagnifier {
  Q_OBJECT
 public:
  enum AxisMode { kXAxis, kYAxis, kBothAxes };

  explicit PlotMagnifier(QWidget* canvas);
  ~PlotMagnifier() override = default;

  void setAxisLimits(int axis, double lower, double upper);
  void widgetWheelEvent(QWheelEvent* event) override;
  void rescale(double factor) override {
    rescale(factor, default_mode_);
  }
  void setDefaultMode(AxisMode mode) {
    default_mode_ = mode;
  }
  void rescale(double factor, AxisMode axis);

 signals:
  void rescaled(QRectF rect);

 protected:
  void widgetMousePressEvent(QMouseEvent* event) override;

 private:
  QPointF invTransform(QPoint pos);

  double lower_bounds_[QwtPlot::axisCnt];
  double upper_bounds_[QwtPlot::axisCnt];
  QPointF mouse_position_;
  AxisMode default_mode_ = kBothAxes;
};

}  // namespace PJ
