#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_zoomer.h>

#include <QPoint>

namespace PJ {

class PlotZoomer : public QwtPlotZoomer {
 public:
  explicit PlotZoomer(QWidget* canvas);
  ~PlotZoomer() override = default;

  void keepAspectRatio(bool keep) {
    keep_aspect_ratio_ = keep;
  }

  // Expands rect along the shorter axis to match canvas pixel ratio; preserves center. No-op when off.
  void applyKeepAspectRatio(QRectF& rect) const;

 protected:
  void widgetMousePressEvent(QMouseEvent* event) override;
  void widgetMouseReleaseEvent(QMouseEvent* event) override;
  void widgetMouseMoveEvent(QMouseEvent* event) override;
  bool accept(QPolygon& polygon) const override;
  void zoom(const QRectF& rect) override;
  QSizeF minZoomSize() const override;

 private:
  bool mouse_pressed_ = false;
  bool zoom_enabled_ = false;
  bool keep_aspect_ratio_ = false;
  QPoint initial_pos_;
};

}  // namespace PJ
