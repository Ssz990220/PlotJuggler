#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_marker.h>

#include <QColor>
#include <QObject>
#include <QPointF>
#include <optional>
#include <vector>

class QwtPlot;
class QwtPlotCurve;

namespace PJ {

std::optional<QPointF> curvePointAt(const QwtPlotCurve* curve, double x);

class CurveTracker : public QObject {
  Q_OBJECT
 public:
  enum Parameter { kLineOnly, kValue, kValueName };

  CurveTracker(QwtPlot* plot, QColor color);
  ~CurveTracker() override;

  [[nodiscard]] QPointF actualPosition() const noexcept {
    return previous_tracker_point_;
  }
  [[nodiscard]] bool isEnabled() const noexcept {
    return visible_;
  }

 public slots:
  void setPosition(const QPointF& pos);
  void setReferencePosition(std::optional<QPointF> reference_pos);
  void setParameter(Parameter parameter);
  void setEnabled(bool enable);
  void redraw() {
    setPosition(previous_tracker_point_);
  }

 private:
  QPointF previous_tracker_point_;
  std::optional<QPointF> reference_pos_;
  std::vector<QwtPlotMarker*> point_markers_;
  QwtPlotMarker* line_marker_ = nullptr;
  QwtPlotMarker* text_marker_ = nullptr;
  QwtPlot* plot_ = nullptr;
  Parameter parameter_ = kValue;
  bool visible_ = true;
};

}  // namespace PJ
