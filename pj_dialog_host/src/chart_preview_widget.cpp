// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_legend.h>
#include <qwt_legend_data.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_plot_zoomer.h>
#include <qwt_scale_div.h>

#include <QColor>
#include <QEvent>
#include <QPen>
#include <QPointF>
#include <QSignalBlocker>
#include <QVariant>
#include <QVector>
#include <QWheelEvent>
#include <algorithm>
#include <limits>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>

namespace PJ {

namespace {
/// Default matplotlib "tab10" palette — 10 distinct colors, used to color a
/// series when it carries no explicit hex color (Qwt has no automatic theme).
const std::vector<QColor>& kDefaultPalette() {
  static const std::vector<QColor> kPalette = {
      QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c), QColor(0xd6, 0x27, 0x28),
      QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b), QColor(0xe3, 0x77, 0xc2), QColor(0x7f, 0x7f, 0x7f),
      QColor(0xbc, 0xbd, 0x22), QColor(0x17, 0xbe, 0xcf),
  };
  return kPalette;
}
}  // namespace

ChartPreviewWidget::ChartPreviewWidget(QWidget* parent) : QwtPlot(parent) {
  setCanvasBackground(Qt::white);

  // Bottom legend with checkable entries: clicking one toggles its curve's
  // visibility (mirrors the old Qt Charts interactive legend).
  auto* legend = new QwtLegend(this);
  legend->setDefaultItemMode(QwtLegendData::Checkable);
  insertLegend(legend, QwtPlot::BottomLegend);
  QObject::connect(legend, &QwtLegend::checked, this, [this](const QVariant& info, bool on, int) {
    if (auto* item = infoToItem(info)) {
      item->setVisible(on);
      replot();
    }
  });

  // Rubber-band zoom (left-drag to zoom in, right-click steps out). Disabled
  // until setZoomEnabled(true); wheel zoom is handled in eventFilter().
  zoomer_ = new QwtPlotZoomer(canvas());
  zoomer_->setEnabled(false);
  QObject::connect(zoomer_, &QwtPlotZoomer::zoomed, this, [this](const QRectF&) { emitViewChanged(); });

  canvas()->installEventFilter(this);
}

void ChartPreviewWidget::setSeries(const std::vector<Series>& series) {
  detachItems(QwtPlotItem::Rtti_PlotCurve, /*autoDelete=*/true);

  double x_min = std::numeric_limits<double>::max();
  double x_max = std::numeric_limits<double>::lowest();
  double y_min = std::numeric_limits<double>::max();
  double y_max = std::numeric_limits<double>::lowest();

  const auto& palette = kDefaultPalette();

  for (size_t i = 0; i < series.size(); ++i) {
    const auto& s = series[i];
    auto* curve = new QwtPlotCurve(QString::fromStdString(s.label));
    curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

    QVector<QPointF> points;
    points.reserve(static_cast<int>(s.points.size()));
    for (const auto& [x, y] : s.points) {
      points.append(QPointF(x, y));
      x_min = std::min(x_min, x);
      x_max = std::max(x_max, x);
      y_min = std::min(y_min, y);
      y_max = std::max(y_max, y);
    }
    curve->setSamples(points);

    // Color precedence: explicit hex (if valid) wins; otherwise a palette entry
    // chosen by series index (wraps modulo the palette size).
    QColor color;
    if (!s.color.empty()) {
      color = QColor(QString::fromStdString(s.color));
    }
    if (!color.isValid()) {
      color = palette[i % palette.size()];
    }
    QPen pen(color);
    pen.setWidthF(1.5);
    curve->setPen(pen);

    curve->attach(this);
  }

  if (x_min < x_max) {
    setAxisScale(QwtPlot::xBottom, x_min, x_max);
  }
  if (y_min < y_max) {
    double margin = (y_max - y_min) * 0.05;
    if (margin == 0.0) {
      margin = 1.0;
    }
    setAxisScale(QwtPlot::yLeft, y_min - margin, y_max + margin);
  }

  replot();
  // Make the freshly-fit view the rubber-band zoom-out base. Block the zoomer's
  // signals so this programmatic reset doesn't emit a spurious viewChanged().
  {
    const QSignalBlocker blocker(zoomer_);
    zoomer_->setZoomBase(true);
  }
}

void ChartPreviewWidget::clearSeries() {
  detachItems(QwtPlotItem::Rtti_PlotCurve, /*autoDelete=*/true);
  replot();
}

void ChartPreviewWidget::setZoomEnabled(bool enabled) {
  zoom_enabled_ = enabled;
  zoomer_->setEnabled(enabled);
}

bool ChartPreviewWidget::eventFilter(QObject* obj, QEvent* event) {
  if (zoom_enabled_ && obj == canvas() && event->type() == QEvent::Wheel) {
    const auto* wheel = static_cast<QWheelEvent*>(event);
    const int delta = wheel->angleDelta().y();
    if (delta != 0) {
      const double scale = delta > 0 ? 0.8 : 1.25;  // wheel up zooms in (shrinks the interval)
      const int axes[2] = {QwtPlot::xBottom, QwtPlot::yLeft};
      for (int axis : axes) {
        const QwtScaleDiv& div = axisScaleDiv(axis);
        const double center = (div.lowerBound() + div.upperBound()) / 2.0;
        const double half = (div.upperBound() - div.lowerBound()) / 2.0 * scale;
        setAxisScale(axis, center - half, center + half);
      }
      replot();
      emitViewChanged();
    }
    return true;
  }
  return QwtPlot::eventFilter(obj, event);
}

void ChartPreviewWidget::emitViewChanged() {
  if (!zoom_enabled_) {
    return;
  }
  const QwtScaleDiv& xs = axisScaleDiv(QwtPlot::xBottom);
  const QwtScaleDiv& ys = axisScaleDiv(QwtPlot::yLeft);
  emit viewChanged(xs.lowerBound(), xs.upperBound(), ys.lowerBound(), ys.upperBound());
}

}  // namespace PJ
