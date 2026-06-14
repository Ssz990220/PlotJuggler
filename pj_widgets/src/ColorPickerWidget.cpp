// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ColorPickerWidget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPoint>

#include "pj_widgets/ColorPickerPopup.h"

namespace PJ {

namespace {
constexpr int kSwatchWidth = 20;
constexpr int kSwatchHeight = 20;
constexpr qreal kCornerRadius = 5.0;
}  // namespace

ColorPickerWidget::ColorPickerWidget(QWidget* parent) : QAbstractButton(parent) {
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  connect(this, &QAbstractButton::clicked, this, &ColorPickerWidget::openPopup);
}

ColorPickerWidget::~ColorPickerWidget() = default;

void ColorPickerWidget::setColor(const QColor& color) {
  if (color_ == color) {
    return;
  }
  color_ = color;
  update();
}

QSize ColorPickerWidget::sizeHint() const {
  return {kSwatchWidth, kSwatchHeight};
}

QSize ColorPickerWidget::minimumSizeHint() const {
  return sizeHint();
}

void ColorPickerWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  // A hairline border keeps a near-background swatch (e.g. white in the light
  // theme) visible against the panel.
  const QRectF face = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(QPen(QColor(0x55, 0x55, 0x55), 1.0));
  painter.setBrush(color_);
  painter.drawRoundedRect(face, kCornerRadius, kCornerRadius);
}

void ColorPickerWidget::openPopup() {
  auto* popup = new ColorPickerPopup(this);
  popup->setAttribute(Qt::WA_DeleteOnClose);
  popup->setColor(color_);
  connect(popup, &ColorPickerPopup::colorChanged, this, [this](QColor picked) {
    if (picked.isValid()) {
      setColor(picked);
      emit colorChanged(picked);
    }
  });
  popup->move(mapToGlobal(QPoint(0, height() + 2)));
  popup->show();
}

}  // namespace PJ
