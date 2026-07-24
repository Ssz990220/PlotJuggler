// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotFocusOverlay.h"

#include <DockAreaWidget.h>
#include <DockContainerWidget.h>

#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRect>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {
// Gap between the area's bounding rect and the frame, so the cue never lands
// on the 1-px splitter handles / container edges around the area.
constexpr int kFrameGapPx = 1;

// Paint a frame just INSIDE the area's bounding rect (in container coords),
// inset by kFrameGapPx plus half the pen (the pen straddles its path), so the
// whole stroke stays within the area and clear of the separators.
void paintFrame(QPainter& painter, const QRect& rect_in_container, const QColor& color, int thickness) {
  if (!rect_in_container.isValid()) {
    return;
  }
  QPen pen(color, thickness);
  pen.setJoinStyle(Qt::MiterJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  const int inset = kFrameGapPx + thickness / 2;
  painter.drawRect(rect_in_container.adjusted(inset, inset, -inset, -inset));
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

  auto rect_for = [this](ads::CDockAreaWidget* area) -> QRect {
    if (area == nullptr) {
      return {};
    }
    const QPoint top_left = area->mapTo(container_, QPoint(0, 0));
    return {top_left, area->size()};
  };

  // Hover only paints when it differs from focus (focus wins under cursor).
  // Focus uses the accent CHECKED tone — the same colour as checked buttons —
  // so the active dock reads as "selected" in both themes without the harsher
  // focus-ring ink, and distinctly from a merely hovered one.
  const auto token_theme = theme::appTheme();
  const QColor hover_color = theme::surface(PJ::theme::Surface::Separation, token_theme);
  const QColor focus_color = theme::interaction(theme::Variant::Accent, theme::State::Checked, token_theme);
  // Focus is drawn slightly thicker than hover so the active dock reads at a
  // glance even when both frames are on screen.
  if (hovered_area_ != nullptr && hovered_area_ != focused_area_) {
    paintFrame(painter, rect_for(hovered_area_), hover_color, /*thickness=*/1);
  }
  if (focused_area_ != nullptr) {
    paintFrame(painter, rect_for(focused_area_), focus_color, /*thickness=*/2);
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
