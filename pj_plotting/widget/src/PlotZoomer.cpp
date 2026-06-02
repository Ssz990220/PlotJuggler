// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotZoomer.h"

#include <qwt_plot_picker.h>

#include <QApplication>
#include <QMouseEvent>
#include <QPen>
#include <QWidget>

#include "pj_widgets/SvgUtil.h"

namespace PJ {

PlotZoomer::PlotZoomer(QWidget* canvas) : QwtPlotZoomer(canvas, false) {
  setTrackerMode(AlwaysOff);
}

void PlotZoomer::widgetMousePressEvent(QMouseEvent* event) {
  mouse_pressed_ = false;
  const auto patterns = mousePattern();
  for (const QwtEventPattern::MousePattern& pattern : patterns) {
    if (mouseMatch(pattern, event)) {
      mouse_pressed_ = true;
      initial_pos_ = event->pos();
    }
    break;
  }
  QwtPlotPicker::widgetMousePressEvent(event);
}

void PlotZoomer::widgetMouseMoveEvent(QMouseEvent* event) {
  if (mouse_pressed_) {
    const QRect rect(event->pos(), initial_pos_);
    const QRectF zoom_rect = invTransform(rect.normalized());
    if (zoom_rect.width() > minZoomSize().width() && zoom_rect.height() > minZoomSize().height()) {
      if (!zoom_enabled_) {
        const QPixmap& pixmap = LoadSvg(":/resources/svg/zoom_in.svg", currentTheme());
        QApplication::setOverrideCursor(QCursor(pixmap.scaled(24, 24)));
        zoom_enabled_ = true;
        setRubberBand(RectRubberBand);
        setTrackerMode(AlwaysOff);
        setRubberBandPen(QPen(parentWidget()->palette().windowText().color(), 1, Qt::DashLine));
      }
    } else if (zoom_enabled_) {
      zoom_enabled_ = false;
      setRubberBand(NoRubberBand);
      QApplication::restoreOverrideCursor();
    }
  }
  QwtPlotPicker::widgetMouseMoveEvent(event);
}

void PlotZoomer::widgetMouseReleaseEvent(QMouseEvent* event) {
  mouse_pressed_ = false;
  if (zoom_enabled_) {
    QApplication::restoreOverrideCursor();
    zoom_enabled_ = false;
  }
  QwtPlotPicker::widgetMouseReleaseEvent(event);
  setTrackerMode(AlwaysOff);
}

bool PlotZoomer::accept(QPolygon& polygon) const {
  QApplication::restoreOverrideCursor();
  if (polygon.count() < 2) {
    return false;
  }

  const QRect rect(polygon[0], polygon[polygon.count() - 1]);
  const QRectF zoom_rect = invTransform(rect.normalized());
  if (zoom_rect.width() < minZoomSize().width() && zoom_rect.height() < minZoomSize().height()) {
    return false;
  }
  return QwtPlotZoomer::accept(polygon);
}

void PlotZoomer::applyKeepAspectRatio(QRectF& rect) const {
  if (!keep_aspect_ratio_) {
    return;
  }
  const QRectF canvas_rect = canvas()->contentsRect();
  // canvasBoundingRect-style inputs can have inverted Y (height < 0). Normalize
  // before computing ratios; the caller's setAxisScale tolerates either order.
  rect = rect.normalized();
  if (canvas_rect.height() <= 0.0 || rect.height() <= 0.0) {
    return;
  }
  const double canvas_ratio = canvas_rect.width() / canvas_rect.height();
  const double zoom_ratio = rect.width() / rect.height();
  if (zoom_ratio < canvas_ratio) {
    const double new_width = rect.height() * canvas_ratio;
    const double increment = new_width - rect.width();
    rect.setWidth(new_width);
    rect.moveLeft(rect.left() - 0.5 * increment);
  } else {
    const double new_height = rect.width() / canvas_ratio;
    const double increment = new_height - rect.height();
    rect.setHeight(new_height);
    rect.moveTop(rect.top() - 0.5 * increment);
  }
}

void PlotZoomer::zoom(const QRectF& zoom_rect) {
  QRectF rect = zoom_rect;
  applyKeepAspectRatio(rect);
  QwtPlotZoomer::zoom(rect);
}

QSizeF PlotZoomer::minZoomSize() const {
  return QSizeF(scaleRect().width() * 0.02, scaleRect().height() * 0.02);
}

}  // namespace PJ
