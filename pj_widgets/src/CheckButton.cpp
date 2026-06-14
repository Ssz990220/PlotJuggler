// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CheckButton.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>

#include "pj_widgets/ThemeColors.h"

namespace PJ {

namespace {
constexpr int kHPadding = 10;         // text inset on each side (snug to text)
constexpr int kHeight = 20;           // fixed button height
constexpr qreal kCornerRadius = 4.0;  // rounded rectangle, like the "auto" toggle
constexpr qreal kBorderWidth = 1.0;
}  // namespace

CheckButton::CheckButton(QWidget* parent) : CheckButton(QString(), parent) {}

CheckButton::CheckButton(const QString& text, QWidget* parent)
    : QAbstractButton(parent),
      // Defaults mirror the QSS tokens (border_checked / border_default /
      // item_selection_background / default_text) so the widget still looks right
      // before a stylesheet sets the qproperties.
      accent_color_(theme::kBlue),
      border_color_(0xC0, 0xC0, 0xC0),
      checked_fill_color_(theme::kLightBlue),
      text_color_(0x11, 0x11, 0x11) {
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
  return {text_width + 2 * kHPadding, kHeight};
}

QSize CheckButton::minimumSizeHint() const {
  return sizeHint();
}

void CheckButton::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Inset by half the border so the stroke stays inside the widget rect.
  const QRectF box =
      QRectF(rect()).adjusted(kBorderWidth / 2.0, kBorderWidth / 2.0, -kBorderWidth / 2.0, -kBorderWidth / 2.0);

  QColor fill;
  QColor border;
  QColor label = text_color_;
  if (!isEnabled()) {
    border = palette().color(QPalette::Disabled, QPalette::Text);
    label = border;
    fill = QColor(Qt::transparent);
  } else if (isChecked()) {
    // Active: light selection fill + accent border (the "auto" button checked look).
    fill = isDown() ? checked_fill_color_.darker(108) : checked_fill_color_;
    border = accent_color_;
  } else {
    // Resting button: quiet hairline border, accent border on hover/press, with a
    // faint button fill so it reads as a real control.
    fill = palette().color(QPalette::Button);
    if (isDown()) {
      fill = fill.darker(108);
    }
    border = underMouse() ? accent_color_ : border_color_;
  }

  painter.setBrush(fill);
  painter.setPen(border.alpha() == 0 ? QPen(Qt::NoPen) : QPen(border, kBorderWidth));
  painter.drawRoundedRect(box, kCornerRadius, kCornerRadius);

  painter.setPen(label);
  painter.drawText(rect(), Qt::AlignCenter, text());
}

}  // namespace PJ
