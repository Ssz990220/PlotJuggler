// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotLegend.h"

#include <qwt_graphic.h>
#include <qwt_legend_data.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QMarginsF>
#include <QMouseEvent>
#include <QPainter>

namespace PJ {

PlotLegend::PlotLegend(QwtPlot* parent) : parent_plot_(parent) {
  setRenderHint(QwtPlotItem::RenderAntialiased);
  setMaxColumns(1);
  setAlignmentInCanvas(Qt::Alignment(Qt::AlignTop | Qt::AlignRight));
  setBackgroundMode(QwtPlotLegendItem::BackgroundMode::LegendBackground);
  setBorderRadius(0);
  setMargin(2);
  setSpacing(1);
  setItemMargin(2);

  QFont legend_font = font();
  legend_font.setPointSize(9);
  setFont(legend_font);
  setVisible(true);
  attach(parent);
}

QRectF PlotLegend::hideButtonRect() const {
  constexpr int kSize = 5;
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  if (alignmentInCanvas() & Qt::AlignRight) {
    return QRectF(geometry(canvas_rect).topRight() + QPoint(-kSize, -kSize), QSize(kSize * 2, kSize * 2));
  }
  return QRectF(geometry(canvas_rect).topLeft() + QPoint(-kSize, -kSize), QSize(kSize * 2, kSize * 2));
}

void PlotLegend::draw(QPainter* painter, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& rect) const {
  if (!collapsed_) {
    QwtPlotLegendItem::draw(painter, x_map, y_map, rect);
  }

  QRectF icon_rect = hideButtonRect();
  if (!isVisible() || plotItems().empty()) {
    return;
  }

  painter->save();
  const QColor color = parent_plot_->canvas()->palette().windowText().color();
  painter->setPen(color);
  painter->setBrush(QBrush(Qt::white, Qt::SolidPattern));
  painter->drawEllipse(icon_rect);

  if (collapsed_) {
    icon_rect -= QMarginsF(3, 3, 3, 3);
    painter->setBrush(QBrush(color, Qt::SolidPattern));
    painter->drawEllipse(icon_rect);
  }
  painter->restore();
}

void PlotLegend::drawLegendData(
    QPainter* painter, const QwtPlotItem* plot_item, const QwtLegendData& data, const QRectF& rect) const {
  const int item_margin = margin();
  const QRectF item_rect = rect.toRect().adjusted(item_margin, item_margin, -item_margin, -item_margin);
  painter->setClipRect(item_rect, Qt::IntersectClip);

  int title_offset = 0;
  constexpr qreal kDotDiameter = 8.0;
  constexpr qreal kDotGap = 4.0;
  if (const auto* curve = dynamic_cast<const QwtPlotCurve*>(plot_item); curve != nullptr) {
    QColor dot_color = curve->pen().color();
    if (!plot_item->isVisible()) {
      dot_color.setAlphaF(0.45);
    }
    const QRectF dot_rect(item_rect.left(), item_rect.center().y() - (kDotDiameter / 2.0), kDotDiameter, kDotDiameter);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(dot_color);
    painter->drawEllipse(dot_rect);
    painter->restore();
    title_offset += static_cast<int>(kDotDiameter + kDotGap);
  } else if (const QwtGraphic graphic = data.icon(); !graphic.isEmpty()) {
    QRectF icon_rect(item_rect.topLeft(), graphic.defaultSize());
    icon_rect.moveCenter(QPoint(icon_rect.center().x(), rect.center().y()));
    if (plot_item->isVisible()) {
      graphic.render(painter, icon_rect, Qt::KeepAspectRatio);
    }
    title_offset += static_cast<int>(icon_rect.width()) + spacing();
  }

  const QwtText text = data.title();
  if (text.isEmpty()) {
    return;
  }

  QPen pen = textPen();
  pen.setColor(plot_item->isVisible() ? parent_plot_->canvas()->palette().windowText().color() : QColor(122, 122, 122));
  painter->setPen(pen);
  painter->setFont(font());
  text.draw(painter, item_rect.adjusted(title_offset, 0, 0, 0));
}

void PlotLegend::drawBackground(QPainter* painter, const QRectF& rect) const {
  painter->save();
  QPen pen = textPen();
  QColor border = parent_plot_->canvas()->palette().windowText().color();
  border.setAlphaF(0.4);
  pen.setColor(border);
  painter->setPen(pen);
  QColor background = parent_plot_->palette().window().color();
  if (!background.isValid() || background.alpha() == 0) {
    background = parent_plot_->canvas()->palette().window().color();
  }
  background.setAlphaF(0.4);
  painter->setBrush(background);
  const double radius = borderRadius();
  painter->drawRoundedRect(rect, radius, radius);
  painter->restore();
}

const QwtPlotItem* PlotLegend::itemAt(const QPoint& pos) const {
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  if (collapsed_ || !isVisible() || !geometry(canvas_rect).contains(pos)) {
    return nullptr;
  }
  for (auto* item : plotItems()) {
    const auto geometries = legendGeometries(item);
    if (!geometries.empty() && geometries.first().contains(pos)) {
      return item;
    }
  }
  return nullptr;
}

const QwtPlotItem* PlotLegend::processMousePressEvent(QMouseEvent* mouse_event) {
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  const QPoint press_point = mouse_event->pos();

  if (!isVisible() || mouse_event->modifiers() != Qt::NoModifier) {
    return nullptr;
  }

  if ((hideButtonRect() + QMargins(2, 2, 2, 2)).contains(press_point)) {
    collapsed_ = !collapsed_;
    parent_plot_->replot();
    return nullptr;
  }

  return itemAt(press_point);
}

}  // namespace PJ
