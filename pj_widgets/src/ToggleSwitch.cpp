// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ToggleSwitch.h"

#include <QEasingCurve>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace PJ {

namespace {
constexpr int kDefaultWidth = 44;
constexpr int kDefaultHeight = 24;
constexpr int kThumbMargin = 3;  // gap between thumb and track edge
constexpr int kAnimationMs = 180;
constexpr int kIconInset = 4;  // padding between slot rect and icon
}  // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QWidget(parent), anim_(new QPropertyAnimation(this, "thumbPosition", this)) {
  setFocusPolicy(Qt::TabFocus);
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);
  anim_->setDuration(kAnimationMs);
  anim_->setEasingCurve(QEasingCurve::OutCubic);
  // toggled fires only when the slide animation reaches its end —
  // rapid clicking interrupts the animation (anim_->stop() inside
  // setChecked) and QPropertyAnimation::finished is not emitted on
  // an interrupted stop, so the listener only sees the final
  // settled state. This matches iOS UISwitch's "Value Changed"
  // behaviour and keeps expensive listeners (e.g. theme switch)
  // from running once per click in a rapid burst.
  connect(anim_, &QPropertyAnimation::finished, this, [this]() { emit toggled(checked_); });
}

ToggleSwitch::~ToggleSwitch() = default;

void ToggleSwitch::setChecked(bool checked, bool animate) {
  if (checked_ == checked) {
    // Even on a no-op, make sure the thumb is anchored at the right
    // endpoint. Defensive against an earlier interrupted animation
    // having left thumb_position_ at an intermediate value.
    const qreal target = checked_ ? 1.0 : 0.0;
    if (anim_->state() != QAbstractAnimation::Running && thumb_position_ != target) {
      thumb_position_ = target;
      update();
    }
    return;
  }
  checked_ = checked;
  anim_->stop();
  if (animate) {
    anim_->setStartValue(thumb_position_);
    anim_->setEndValue(checked_ ? 1.0 : 0.0);
    anim_->start();
    // toggled is emitted from anim_->finished (see ctor), not here:
    // it only fires once the thumb has settled, so an interrupted
    // animation does NOT emit it.
  } else {
    // Programmatic init path: snap to the endpoint, no animation,
    // no `toggled` emit.
    thumb_position_ = checked_ ? 1.0 : 0.0;
    update();
  }
}

void ToggleSwitch::toggle() {
  setChecked(!checked_);
}

void ToggleSwitch::setLeftIcon(const QIcon& icon) {
  left_icon_ = icon;
  update();
}

void ToggleSwitch::setRightIcon(const QIcon& icon) {
  right_icon_ = icon;
  update();
}

QSize ToggleSwitch::sizeHint() const {
  return {kDefaultWidth, kDefaultHeight};
}

QSize ToggleSwitch::minimumSizeHint() const {
  return {kDefaultWidth, kDefaultHeight};
}

void ToggleSwitch::setThumbPosition(qreal pos) {
  thumb_position_ = pos;
  update();
}

QRect ToggleSwitch::thumbRect() const {
  const int diameter = height() - (2 * kThumbMargin);
  const int x_left = kThumbMargin;
  const int x_right = width() - kThumbMargin - diameter;
  const int x = x_left + static_cast<int>(thumb_position_ * (x_right - x_left));
  return {x, kThumbMargin, diameter, diameter};
}

QRect ToggleSwitch::leftSlotRect() const {
  // The slot is the square the thumb occupies when fully LEFT. Sizing it
  // identically to (and at the same position as) the thumb means whatever
  // is painted here lines up pixel-perfectly with the thumb's center —
  // which is what makes the icons appear "to land where the thumb sits"
  // at any widget width or height.
  const int diameter = height() - (2 * kThumbMargin);
  return {kThumbMargin, kThumbMargin, diameter, diameter};
}

QRect ToggleSwitch::rightSlotRect() const {
  // Square at the thumb's RIGHT end position; mirror of leftSlotRect.
  const int diameter = height() - (2 * kThumbMargin);
  return {width() - kThumbMargin - diameter, kThumbMargin, diameter, diameter};
}

void ToggleSwitch::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Track: pill (corner radius = half the height). Fill interpolates
  // from the "off" tone to the "on" tone as the thumb moves so the
  // color transition tracks the animation smoothly.
  const QColor off_track(120, 120, 120);
  const QColor on_track(0x11, 0x77, 0xFF);  // blue
  const auto lerp = [](int a, int b, qreal t) { return static_cast<int>(a + ((b - a) * t)); };
  const QColor track_color(
      lerp(off_track.red(), on_track.red(), thumb_position_),
      lerp(off_track.green(), on_track.green(), thumb_position_),
      lerp(off_track.blue(), on_track.blue(), thumb_position_));

  const qreal radius = height() / 2.0;
  painter.setPen(Qt::NoPen);
  painter.setBrush(track_color);
  painter.drawRoundedRect(rect(), radius, radius);

  // Both slot backgrounds are always painted; the thumb composites on top.
  // Default impls fade each based on thumb_position_ so only the icon
  // opposite the thumb is visible at rest.
  paintLeftSlot(painter, leftSlotRect(), thumb_position_);
  paintRightSlot(painter, rightSlotRect(), thumb_position_);

  // Thumb: white circle with a faint outer outline for definition.
  const QRect thumb = thumbRect();
  painter.setPen(QPen(QColor(0, 0, 0, 40), 1));
  painter.setBrush(Qt::white);
  painter.drawEllipse(thumb);
}

void ToggleSwitch::paintLeftSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position) {
  if (left_icon_.isNull()) {
    return;
  }
  // Left icon is visible when the thumb has moved RIGHT (away from it).
  // Symmetric inset preserves the slot's true center exactly — using
  // slot.center() + offset would lose half a pixel to QRect's integer
  // center floor when the slot has an even width/height, drifting the
  // icon 1px up-and-left of the thumb. adjusted(d, d, -d, -d) shifts
  // both edges identically so the center is unchanged.
  const int inset = kIconInset / 2;
  const QRect target = slot_rect.adjusted(inset, inset, -inset, -inset);
  painter.save();
  painter.setOpacity(thumb_position);
  painter.drawPixmap(target, left_icon_.pixmap(target.width(), target.height()));
  painter.restore();
}

void ToggleSwitch::paintRightSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position) {
  if (right_icon_.isNull()) {
    return;
  }
  // Right icon is visible when the thumb is LEFT (away from it).
  const int inset = kIconInset / 2;
  const QRect target = slot_rect.adjusted(inset, inset, -inset, -inset);
  painter.save();
  painter.setOpacity(1.0 - thumb_position);
  painter.drawPixmap(target, right_icon_.pixmap(target.width(), target.height()));
  painter.restore();
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
    toggle();
    emit clicked();
  }
  QWidget::mouseReleaseEvent(event);
}

void ToggleSwitch::keyPressEvent(QKeyEvent* event) {
  const int key = event->key();
  if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter) {
    toggle();
    emit clicked();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void ToggleSwitch::changeEvent(QEvent* event) {
  // Whenever the widget re-enters the interactive state (typically
  // after an external `setEnabled(true)` that unlocked a click →
  // theme-apply cycle), force the thumb to its canonical endpoint
  // for the current checked_ value if no animation is running.
  // This is a no-op in the normal case (the animation that fired
  // `toggled` ended exactly at the endpoint), but if anything ever
  // left thumb_position_ intermediate (interrupted animation,
  // programmatic poke, dialog destroyed mid-slide and reopened),
  // the next interactive moment self-heals the visual state.
  if (event->type() == QEvent::EnabledChange && isEnabled() && anim_->state() != QAbstractAnimation::Running) {
    const qreal target = checked_ ? 1.0 : 0.0;
    if (thumb_position_ != target) {
      thumb_position_ = target;
      update();
    }
  }
  QWidget::changeEvent(event);
}

}  // namespace PJ
