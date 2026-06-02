// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotFocusOverlay.h"

#include <DockAreaWidget.h>
#include <DockContainerWidget.h>

#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRect>
#include <algorithm>

namespace PJ {

namespace {
// Match the QSS palette tokens — see resources/stylesheet_*.qss
// (blue, light_blue). Keep these in sync if the tokens change.
const QColor kFocusColor = QColor("#1177FF");
const QColor kHoverColor = QColor("#C2DCFF");

// Paint a 1-px frame at the 4 splitter handles / outer-edge pixels that
// hug the area's bounding rect (in container coords). For inner edges
// the line sits one pixel OUTSIDE the area, landing on the adjacent
// splitter handle. For edges flush with the container's boundary we
// clamp to the container's edge pixel so the cue isn't clipped away.
void paintFrame(QPainter& painter, const QRect& rect_in_container, const QRect& container_rect, const QColor& color) {
  if (!rect_in_container.isValid() || !container_rect.isValid()) {
    return;
  }
  painter.setPen(QPen(color, 1));
  painter.setBrush(Qt::NoBrush);
  const int left = std::max(container_rect.left(), rect_in_container.left() - 1);
  const int top = std::max(container_rect.top(), rect_in_container.top() - 1);
  const int right = std::min(container_rect.right(), rect_in_container.right() + 1);
  const int bottom = std::min(container_rect.bottom(), rect_in_container.bottom() + 1);
  painter.drawLine(left, top, right, top);
  painter.drawLine(left, bottom, right, bottom);
  painter.drawLine(left, top, left, bottom);
  painter.drawLine(right, top, right, bottom);
}
}  // namespace

PlotFocusOverlay::PlotFocusOverlay(ads::CDockContainerWidget& container) : QWidget(&container), container_(&container) {
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setFocusPolicy(Qt::NoFocus);
  container_->installEventFilter(this);
  syncGeometryToContainer();
  raise();
}

bool PlotFocusOverlay::ownsArea(ads::CDockAreaWidget* area) const {
  return area != nullptr && container_->isAncestorOf(area);
}

void PlotFocusOverlay::setFocusedArea(ads::CDockAreaWidget* area) {
  // Reject areas from a different container (e.g. floating dock window).
  // mapTo() against a non-ancestor would silently paint at garbage coords.
  ads::CDockAreaWidget* const next = ownsArea(area) ? area : nullptr;
  if (focused_area_ == next) {
    return;
  }
  focused_area_ = next;
  update();
}

void PlotFocusOverlay::setHoveredArea(ads::CDockAreaWidget* area) {
  ads::CDockAreaWidget* const next = ownsArea(area) ? area : nullptr;
  if (hovered_area_ == next) {
    return;
  }
  hovered_area_ = next;
  update();
}

void PlotFocusOverlay::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  const QRect container_rect = container_->rect();

  auto rect_for = [this](ads::CDockAreaWidget* area) -> QRect {
    if (area == nullptr) {
      return {};
    }
    const QPoint top_left = area->mapTo(container_, QPoint(0, 0));
    return {top_left, area->size()};
  };

  // Hover only paints when it differs from focus (focus wins under cursor).
  if (hovered_area_ != nullptr && hovered_area_ != focused_area_) {
    paintFrame(painter, rect_for(hovered_area_), container_rect, kHoverColor);
  }
  if (focused_area_ != nullptr) {
    paintFrame(painter, rect_for(focused_area_), container_rect, kFocusColor);
  }
}

bool PlotFocusOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (watched == container_) {
    const QEvent::Type type = event->type();
    if (type == QEvent::Resize || type == QEvent::LayoutRequest) {
      syncGeometryToContainer();
    } else if (type == QEvent::ChildAdded || type == QEvent::ChildPolished) {
      // ADS may add new top-level QSplitters as direct siblings, which
      // would otherwise stack above us. Bounce back to the top.
      raise();
      update();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void PlotFocusOverlay::syncGeometryToContainer() {
  setGeometry(container_->rect());
  raise();
  update();
}

}  // namespace PJ
