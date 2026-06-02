#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot.h>
#include <qwt_plot_legenditem.h>

#include <QObject>

namespace PJ {

class PlotLegend : public QObject, public QwtPlotLegendItem {
  Q_OBJECT
 public:
  explicit PlotLegend(QwtPlot* parent);

  QRectF hideButtonRect() const;
  const QwtPlotItem* itemAt(const QPoint& pos) const;
  const QwtPlotItem* processMousePressEvent(QMouseEvent* mouse_event);

 private:
  void draw(QPainter* painter, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& rect) const override;
  void drawLegendData(
      QPainter* painter, const QwtPlotItem* item, const QwtLegendData& data, const QRectF& rect) const override;
  void drawBackground(QPainter* painter, const QRectF& rect) const override;

  QwtPlot* parent_plot_ = nullptr;
  bool collapsed_ = false;
};

}  // namespace PJ
