#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_panner.h>

namespace PJ {

class PlotPanner : public QwtPlotPanner {
  Q_OBJECT
 public:
  explicit PlotPanner(QWidget* canvas) : QwtPlotPanner(canvas) {}

 public slots:
  void moveCanvas(int dx, int dy) override;

 signals:
  void rescaled(QRectF rect);

 protected:
  void widgetMousePressEvent(QMouseEvent* event) override;
  void widgetMouseReleaseEvent(QMouseEvent* event) override;

 private:
  bool cursor_overridden_ = false;
};

}  // namespace PJ
