// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/CurveTracker.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_scale_map.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

#include <QFontDatabase>
#include <QPalette>
#include <QPen>
#include <QSettings>
#include <QSize>
#include <algorithm>
#include <limits>
#include <map>

namespace PJ {

namespace {

[[nodiscard]] std::optional<QPointF> referencePointAt(const QwtPlotCurve* curve, std::optional<QPointF> reference) {
  return reference.has_value() ? curvePointAt(curve, reference->x()) : std::nullopt;
}

}  // namespace

CurveTracker::CurveTracker(QwtPlot* plot, QColor color) : QObject(plot), plot_(plot) {
  line_marker_ = new QwtPlotMarker();
  line_marker_->setLinePen(QPen(color));
  line_marker_->setLineStyle(QwtPlotMarker::VLine);
  line_marker_->setValue(0.0, 0.0);
  line_marker_->attach(plot_);

  text_marker_ = new QwtPlotMarker();
  text_marker_->attach(plot_);
}

CurveTracker::~CurveTracker() {
  for (auto* marker : point_markers_) {
    marker->detach();
    delete marker;
  }
  point_markers_.clear();
  if (line_marker_ != nullptr) {
    line_marker_->detach();
    delete line_marker_;
  }
  if (text_marker_ != nullptr) {
    text_marker_->detach();
    delete text_marker_;
  }
}

void CurveTracker::setParameter(Parameter parameter) {
  if (parameter_ == parameter) {
    return;
  }
  parameter_ = parameter;
  setPosition(previous_tracker_point_);
}

void CurveTracker::setEnabled(bool enable) {
  visible_ = enable;
  line_marker_->setVisible(enable);
  text_marker_->setVisible(enable);
  for (auto* marker : point_markers_) {
    marker->setVisible(enable);
  }
}

void CurveTracker::setPosition(const QPointF& tracker_position) {
  if (plot_ == nullptr) {
    return;
  }

  const QwtPlotItemList curves = plot_->itemList(QwtPlotItem::Rtti_PlotCurve);
  line_marker_->setValue(tracker_position);

  QRectF view_rect;
  view_rect.setBottom(plot_->canvasMap(QwtPlot::yLeft).s1());
  view_rect.setTop(plot_->canvasMap(QwtPlot::yLeft).s2());
  view_rect.setLeft(plot_->canvasMap(QwtPlot::xBottom).s1());
  view_rect.setRight(plot_->canvasMap(QwtPlot::xBottom).s2());

  while (point_markers_.size() > static_cast<std::size_t>(curves.size())) {
    point_markers_.back()->detach();
    delete point_markers_.back();
    point_markers_.pop_back();
  }
  while (point_markers_.size() < static_cast<std::size_t>(curves.size())) {
    auto* marker = new QwtPlotMarker();
    marker->attach(plot_);
    point_markers_.push_back(marker);
  }

  struct LineParts {
    QColor color;
    QString value;
    QString delta;
    QString name;
  };

  std::multimap<double, LineParts> text_lines;
  const int precision = QSettings().value(QStringLiteral("Preferences::precision"), 3).toInt();
  int values_char_count = 0;
  int delta_char_count = 0;
  double min_y = std::numeric_limits<double>::max();
  double max_y = std::numeric_limits<double>::lowest();
  int visible_points = 0;

  for (int index = 0; index < curves.size(); ++index) {
    auto* curve = dynamic_cast<QwtPlotCurve*>(curves[index]);
    if (curve == nullptr || !curve->isVisible()) {
      point_markers_[static_cast<std::size_t>(index)]->setVisible(false);
      continue;
    }

    const QColor color = curve->pen().color();
    QwtPlotMarker* point_marker = point_markers_[static_cast<std::size_t>(index)];
    if (point_marker->symbol() == nullptr || point_marker->symbol()->brush().color() != color) {
      point_marker->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, color, QPen(Qt::black), QSize(5, 5)));
    }

    const auto maybe_point = curvePointAt(curve, tracker_position.x());
    if (!maybe_point.has_value()) {
      point_marker->setVisible(false);
      continue;
    }

    const QPointF point = *maybe_point;
    point_marker->setValue(point);
    if (!view_rect.contains(point) || !visible_) {
      point_marker->setVisible(false);
      continue;
    }

    const auto maybe_reference = referencePointAt(curve, reference_pos_);
    LineParts parts;
    parts.color = color;
    parts.value = QString::number(point.y(), 'f', precision);
    parts.name = curve->title().text();
    if (maybe_reference.has_value()) {
      parts.delta = QStringLiteral(" (Δ %1)").arg(QString::number(point.y() - maybe_reference->y(), 'f', precision));
    }
    text_lines.insert({point.y(), parts});
    values_char_count = std::max(values_char_count, static_cast<int>(parts.value.length()));
    delta_char_count = std::max(delta_char_count, static_cast<int>(parts.delta.length()));

    min_y = std::min(min_y, point.y());
    max_y = std::max(max_y, point.y());
    ++visible_points;
    point_marker->setVisible(true);
  }

  for (auto& [unused, parts] : text_lines) {
    (void)unused;
    while (parts.value.length() < values_char_count) {
      parts.value.prepend(QStringLiteral("&nbsp;"));
    }
    while (parts.delta.length() < delta_char_count) {
      parts.delta.prepend(QStringLiteral("&nbsp;"));
    }
  }

  QwtText marker_text;
  const QColor text_color = plot_->palette().color(QPalette::WindowText);
  QString time_delta;
  if (reference_pos_.has_value()) {
    time_delta =
        QStringLiteral(" (Δ %1)").arg(QString::number(tracker_position.x() - reference_pos_->x(), 'f', precision));
  }
  QString marker_html = QStringLiteral("<font color=%1>time : %2%3</font><br>")
                            .arg(text_color.name(), QString::number(tracker_position.x(), 'f', precision), time_delta);

  if (parameter_ != kLineOnly) {
    int line_index = 0;
    for (auto it = text_lines.rbegin(); it != text_lines.rend(); ++it) {
      const LineParts& parts = it->second;
      if (parameter_ == kValue) {
        marker_html += QStringLiteral("<font color=%1>%2%3</font>").arg(parts.color.name(), parts.value, parts.delta);
      } else {
        marker_html += QStringLiteral("<font color=%1>%2%3 : %4</font>")
                           .arg(parts.color.name(), parts.value, parts.delta, parts.name);
      }
      if (++line_index < static_cast<int>(text_lines.size())) {
        marker_html += QStringLiteral("<br>");
      }
    }

    QColor background_color = plot_->palette().color(QPalette::Window);
    background_color.setAlpha(180);
    marker_text.setBackgroundBrush(background_color);
    marker_text.setBorderPen(QColor(Qt::transparent));
    marker_text.setText(marker_html);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(9);
    marker_text.setFont(font);
    marker_text.setRenderFlags(parameter_ == kValue ? Qt::AlignCenter : Qt::AlignLeft);
    text_marker_->setLabel(marker_text);
    text_marker_->setLabelAlignment(Qt::AlignRight);
    text_marker_->setXValue(tracker_position.x() + view_rect.width() * 0.02);
  }

  if (visible_points > 0) {
    text_marker_->setYValue(0.5 * (max_y + min_y));
  }

  const double canvas_ratio = view_rect.width() / static_cast<double>(std::max(1, plot_->width()));
  const double text_width = marker_text.textSize().width() * canvas_ratio;
  if ((text_marker_->boundingRect().right() + text_width) > view_rect.right()) {
    text_marker_->setXValue(tracker_position.x() - view_rect.width() * 0.02 - text_width);
  }

  text_marker_->setVisible(visible_points > 0 && visible_ && parameter_ != kLineOnly);
  previous_tracker_point_ = tracker_position;
}

void CurveTracker::setReferencePosition(std::optional<QPointF> reference_pos) {
  reference_pos_ = reference_pos;
  redraw();
}

std::optional<QPointF> curvePointAt(const QwtPlotCurve* curve, double x) {
  if (curve == nullptr || curve->dataSize() == 0) {
    return std::nullopt;
  }
  if (curve->dataSize() == 1) {
    return curve->sample(0);
  }

  std::size_t low = 0;
  std::size_t high = curve->dataSize();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (curve->sample(mid).x() <= x) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  if (low == 0) {
    return curve->sample(0);
  }
  if (low >= curve->dataSize()) {
    return curve->sample(curve->dataSize() - 1);
  }

  const QPointF left = curve->sample(low - 1);
  const QPointF right = curve->sample(low);
  const double middle_x = (left.x() + right.x()) * 0.5;
  return x < middle_x ? left : right;
}

}  // namespace PJ
