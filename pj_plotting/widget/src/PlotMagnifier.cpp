// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotMagnifier.h"

#include <qwt_scale_map.h>

#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <limits>

namespace PJ {

PlotMagnifier::PlotMagnifier(QWidget* canvas) : QwtPlotMagnifier(canvas) {
  for (int axis_id = 0; axis_id < QwtPlot::axisCnt; ++axis_id) {
    lower_bounds_[axis_id] = std::numeric_limits<double>::lowest();
    upper_bounds_[axis_id] = std::numeric_limits<double>::max();
  }
}

void PlotMagnifier::setAxisLimits(int axis, double lower, double upper) {
  if (axis >= 0 && axis < QwtPlot::axisCnt) {
    lower_bounds_[axis] = lower;
    upper_bounds_[axis] = upper;
  }
}

void PlotMagnifier::rescale(double factor, AxisMode axis) {
  factor = qAbs(1.0 / factor);

  QwtPlot* qwt_plot = plot();
  if (qwt_plot == nullptr || factor == 1.0) {
    return;
  }

  bool do_replot = false;
  const bool auto_replot = qwt_plot->autoReplot();
  qwt_plot->setAutoReplot(false);

  const int axis_list[2] = {QwtPlot::xBottom, QwtPlot::yLeft};
  QRectF new_rect;

  for (int index = 0; index < 2; ++index) {
    double temp_factor = factor;
    if (index == 1 && axis == kXAxis) {
      temp_factor = 1.0;
    }
    if (index == 0 && axis == kYAxis) {
      temp_factor = 1.0;
    }

    const int axis_id = axis_list[index];
    if (!isAxisEnabled(axis_id)) {
      continue;
    }

    const QwtScaleMap scale_map = qwt_plot->canvasMap(axis_id);
    double v1 = scale_map.s1();
    double v2 = scale_map.s2();
    double center = axis_id == QwtPlot::yLeft ? mouse_position_.y() : mouse_position_.x();

    if (scale_map.transformation()) {
      v1 = scale_map.transform(v1);
      v2 = scale_map.transform(v2);
    }

    const double width = v2 - v1;
    const double ratio = (v2 - center) / width;
    v1 = center - width * temp_factor * (1 - ratio);
    v2 = center + width * temp_factor * ratio;

    bool reversed_axis = false;
    if (v1 > v2) {
      reversed_axis = true;
      std::swap(v1, v2);
    }

    if (scale_map.transformation()) {
      v1 = scale_map.invTransform(v1);
      v2 = scale_map.invTransform(v2);
    }

    v1 = std::max(v1, lower_bounds_[axis_id]);
    v2 = std::min(v2, upper_bounds_[axis_id]);
    qwt_plot->setAxisScale(axis_id, reversed_axis ? v2 : v1, reversed_axis ? v1 : v2);

    if (axis_id == QwtPlot::xBottom) {
      new_rect.setLeft(v1);
      new_rect.setRight(v2);
    } else {
      new_rect.setBottom(v1);
      new_rect.setTop(v2);
    }
    do_replot = true;
  }

  qwt_plot->setAutoReplot(auto_replot);
  if (do_replot) {
    emit rescaled(new_rect);
  }
}

QPointF PlotMagnifier::invTransform(QPoint pos) {
  const QwtScaleMap x_map = plot()->canvasMap(QwtPlot::xBottom);
  const QwtScaleMap y_map = plot()->canvasMap(QwtPlot::yLeft);
  return QPointF(x_map.invTransform(pos.x()), y_map.invTransform(pos.y()));
}

void PlotMagnifier::widgetWheelEvent(QWheelEvent* event) {
  mouse_position_ = invTransform(event->position().toPoint());
  QwtPlotMagnifier::widgetWheelEvent(event);
}

void PlotMagnifier::widgetMousePressEvent(QMouseEvent* event) {
  mouse_position_ = invTransform(event->pos());
  QwtPlotMagnifier::widgetMousePressEvent(event);
}

}  // namespace PJ
