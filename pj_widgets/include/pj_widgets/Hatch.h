#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// App-wide "no data" diagonal hatch. A SINGLE shared pattern so every widget that
// paints it (the Timeline empty span, the RangeSlider unselected track, ...) reads
// as a window into ONE continuous hatch layer: the diagonal lines are phase-aligned
// across widgets, not merely the same spacing. The trick is to phase the pattern to
// a shared grid — `X + Y ≡ 0 (mod kHatchSpacing)` — measured in a common coordinate
// frame (pass each widget's GLOBAL origin), so two widgets at different screen
// positions still draw collinear lines.

#include <QColor>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <cmath>

namespace PJ {

// Single source of truth for the hatch line spacing (logical px). Shared by every
// widget so the pattern density can never drift between them.
inline constexpr double kHatchSpacing = 14.0;

// Theme ink for the hatch. A self-painting widget must read QGuiApplication::palette()
// (the app syncs Window/WindowText per theme; QSS does NOT touch the palette), so this
// adapts: dark-grey hatch on a light theme, light-grey on dark. `t` is the blend toward
// the text ink (0.30 active / lighter when disabled), matching the Timeline's hatch.
inline QColor appHatchColor(bool enabled = true) {
  const QPalette pal = QGuiApplication::palette();
  const QColor bg = pal.color(QPalette::Window);
  const QColor text = pal.color(QPalette::WindowText);
  const double t = enabled ? 0.30 : 0.18;
  const auto mix = [t](double a, double b) { return a * (1.0 - t) + b * t; };
  return QColor::fromRgbF(mix(bg.redF(), text.redF()), mix(bg.greenF(), text.greenF()), mix(bg.blueF(), text.blueF()));
}

// The shared BACKDROP the hatch is composited over — the second half of "one
// continuous layer". The ink (appHatchColor) is only half the story: identical ink on
// a brighter backdrop reads with MORE contrast (= busier). Every caller must paint this
// behind the hatch so contrast matches too, not just spacing/phase. It is QPalette::Window
// — the exact tone the Timeline fills under its empty span (TimelineColors::bg).
inline QColor appHatchBackground() {
  return QGuiApplication::palette().color(QPalette::Window);
}

// Paint the shared hatch into `painter`, filling `rect` and intersecting any clip the
// caller already set (so a widget can restrict it to an arbitrary region, e.g. the
// RangeSlider's unselected stripes, by setting a clip region first). `global_origin`
// is the origin of `rect`'s coordinate frame in the shared (global/screen) space —
// pass `widget->mapToGlobal({0,0})` for a QWidget, or the item origin mapped to global
// for a QGraphicsItem. Lines are 45-degree "/", length = rect.height(), 1px cosmetic.
inline void drawHatch(QPainter& painter, const QRectF& rect, const QPointF& global_origin, const QColor& color) {
  if (rect.width() <= 0.0 || rect.height() <= 0.0) {
    return;
  }
  const auto posmod = [](double a, double m) {
    const double r = std::fmod(a, m);
    return r < 0.0 ? r + m : r;
  };
  painter.save();
  painter.setClipRect(rect, Qt::IntersectClip);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(color, 1.0));
  const double h = rect.height();
  // A "/" line through local (xs, rect.bottom()) lies on global X+Y =
  // xs + global_origin.x() + rect.bottom() + global_origin.y(); choose xs so that's a
  // multiple of kHatchSpacing, then tile by kHatchSpacing across rect (clipping spill).
  const double base_phase = posmod(global_origin.x() + global_origin.y() + rect.bottom(), kHatchSpacing);
  double start_x = rect.left() - h;
  start_x -= posmod(start_x + base_phase, kHatchSpacing);
  const int count = static_cast<int>((rect.width() + 2.0 * h) / kHatchSpacing) + 2;
  for (int i = 0; i < count; ++i) {
    const double xs = start_x + i * kHatchSpacing;
    painter.drawLine(QLineF(xs, rect.bottom(), xs + h, rect.top()));
  }
  painter.restore();
}

// Paint the COMPLETE shared "no data" fill into `rect`: the theme backdrop
// (appHatchBackground) PLUS the phase-aligned hatch (appHatchColor) on top — so every
// widget composites the identical ink over the identical backdrop and gets the same
// contrast, not merely the same spacing/phase. Prefer this over a bare drawHatch()
// anywhere the region isn't already filled with QPalette::Window (e.g. the RangeSlider's
// otherwise-transparent groove, where a bare hatch would sit on white and read busier).
// Respects any clip the caller set, so a widget can restrict it to a sub-region.
inline void drawNoDataHatch(QPainter& painter, const QRectF& rect, const QPointF& global_origin, bool enabled = true) {
  painter.fillRect(rect, appHatchBackground());
  drawHatch(painter, rect, global_origin, appHatchColor(enabled));
}

}  // namespace PJ
