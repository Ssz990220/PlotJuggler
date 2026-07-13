// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CheckButton.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {
constexpr auto kHPadding = theme::Space::Comfortable;  // text inset on each side (snug to text)

theme::Theme frameworkTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}
}  // namespace

CheckButton::CheckButton(QWidget* parent) : CheckButton(QString(), parent) {}

CheckButton::CheckButton(const QString& text, QWidget* parent)
    : QAbstractButton(parent),
      // Defaults mirror the framework roles so the widget still looks right
      // before a stylesheet sets the qproperties.
      accent_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      border_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      checked_fill_color_(theme::interaction(theme::Variant::Accent, theme::State::Checked, frameworkTheme())),
      text_color_(theme::text(frameworkTheme())) {
  setText(text);
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setAttribute(Qt::WA_Hover, true);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

CheckButton::~CheckButton() = default;

void CheckButton::setAccentColor(const QColor& color) {
  if (accent_color_ == color) {
    return;
  }
  accent_color_ = color;
  update();
}

void CheckButton::setBorderColor(const QColor& color) {
  if (border_color_ == color) {
    return;
  }
  border_color_ = color;
  update();
}

void CheckButton::setCheckedFillColor(const QColor& color) {
  if (checked_fill_color_ == color) {
    return;
  }
  checked_fill_color_ = color;
  update();
}

void CheckButton::setTextColor(const QColor& color) {
  if (text_color_ == color) {
    return;
  }
  text_color_ = color;
  update();
}

QSize CheckButton::sizeHint() const {
  const int text_width = fontMetrics().horizontalAdvance(text());
  return {text_width + 2 * theme::space(kHPadding), theme::metric(theme::Metric::InputOuterHeight)};
}

QSize CheckButton::minimumSizeHint() const {
  return sizeHint();
}

void CheckButton::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const auto fw_theme = frameworkTheme();
  const qreal border_width = theme::stroke(theme::Stroke::Hairline, fw_theme);
  const qreal corner_radius = theme::radius(theme::Radius::Input, fw_theme);

  // Inset by half the border so the stroke stays inside the widget rect.
  const QRectF box =
      QRectF(rect()).adjusted(border_width / 2.0, border_width / 2.0, -border_width / 2.0, -border_width / 2.0);

  QColor fill;
  QColor border;
  QColor label = text_color_;
  if (!isEnabled()) {
    border = theme::surface(PJ::theme::Surface::Separation, fw_theme);
    label = theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Disabled, fw_theme);
    fill = theme::interaction(theme::Variant::Neutral, theme::State::Disabled, fw_theme);
  } else if (isChecked()) {
    // Active: light selection fill + accent border (the "auto" button checked look).
    fill = isDown() ? theme::interaction(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme)
                    : checked_fill_color_;
    border = accent_color_;
  } else {
    // Resting button: quiet hairline border, accent border on hover/press, with a
    // faint button fill so it reads as a real control.
    fill = palette().color(QPalette::Button);
    if (isDown()) {
      fill = theme::interaction(theme::Variant::Neutral, theme::State::Pressed, fw_theme);
    }
    border = underMouse() ? accent_color_ : border_color_;
  }

  painter.setBrush(fill);
  painter.setPen(border.alpha() == 0 ? QPen(Qt::NoPen) : QPen(border, border_width));
  painter.drawRoundedRect(box, corner_radius, corner_radius);

  painter.setPen(label);
  painter.drawText(rect(), Qt::AlignCenter, text());
}

}  // namespace PJ
