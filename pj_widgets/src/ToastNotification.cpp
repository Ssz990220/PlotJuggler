// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ToastNotification.h"

#include <QEasingCurve>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>

namespace PJ {

ToastNotification::ToastNotification(const QString& message, const QPixmap& icon, int timeout_ms, QWidget* parent)
    : QFrame(parent), timeout_ms_(timeout_ms) {
  setupUI();
  setupAnimation();

  setMessage(message);
  if (!icon.isNull()) {
    setIcon(icon);
  }
}

ToastNotification::~ToastNotification() {
  if (slide_animation_) {
    slide_animation_->stop();
  }
  if (timeout_timer_) {
    timeout_timer_->stop();
  }
}

void ToastNotification::setupUI() {
  setFrameShape(QFrame::StyledPanel);
  setFrameShadow(QFrame::Raised);
  setMinimumHeight(40);  // Minimum for text-only toasts
  setMaximumWidth(400);

  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(8, 8, 8, 8);
  layout_->setSpacing(12);

  // Icon label (optional, hidden until an icon is set).
  icon_label_ = new QLabel(this);
  icon_label_->setObjectName("toastIcon");
  icon_label_->setFixedSize(kIconSize, kIconSize);
  icon_label_->setScaledContents(false);  // We round the corners ourselves.
  icon_label_->setVisible(false);
  layout_->addWidget(icon_label_);

  // Message label (rich text with clickable external links).
  message_label_ = new QLabel(this);
  message_label_->setObjectName("toastMessage");
  message_label_->setWordWrap(true);
  message_label_->setTextFormat(Qt::RichText);
  message_label_->setOpenExternalLinks(true);
  message_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout_->addWidget(message_label_, 1);

  close_button_ = new QPushButton(this);
  close_button_->setObjectName("toastCloseButton");
  close_button_->setText("×");  // U+00D7 multiplication sign, not the letter x
  close_button_->setFixedSize(24, 24);
  close_button_->setFlat(true);
  close_button_->setCursor(Qt::PointingHandCursor);
  close_button_->setFocusPolicy(Qt::NoFocus);
  layout_->addWidget(close_button_, 0, Qt::AlignTop);

  connect(close_button_, &QPushButton::clicked, this, &ToastNotification::onCloseClicked);

  if (timeout_ms_ > 0) {
    timeout_timer_ = new QTimer(this);
    timeout_timer_->setSingleShot(true);
    connect(timeout_timer_, &QTimer::timeout, this, &ToastNotification::onTimeout);
  }
}

void ToastNotification::setupAnimation() {
  slide_animation_ = new QPropertyAnimation(this, "pos", this);
  slide_animation_->setDuration(kAnimationDurationMs);
  slide_animation_->setEasingCurve(QEasingCurve::OutCubic);

  connect(slide_animation_, &QPropertyAnimation::finished, this, &ToastNotification::onAnimationFinished);
}

void ToastNotification::showAnimated() {
  // The target is the position ToastManager already move()d us to.
  const QPoint end_pos = pos();

  QPoint start_pos = end_pos;
  start_pos.setX(end_pos.x() + width() + 50);

  move(start_pos);
  show();

  slide_animation_->setStartValue(start_pos);
  slide_animation_->setEndValue(end_pos);
  slide_animation_->start();

  // Only start the auto-dismiss countdown once the entrance finishes.
  if (timeout_timer_ && timeout_ms_ > 0) {
    QTimer::singleShot(kAnimationDurationMs, this, [this]() {
      if (timeout_timer_ && !is_closing_) {
        timeout_timer_->start(timeout_ms_);
      }
    });
  }
}

void ToastNotification::hideAnimated() {
  if (is_closing_) {
    return;
  }
  is_closing_ = true;

  if (timeout_timer_) {
    timeout_timer_->stop();
  }

  QPoint start_pos = pos();
  QPoint end_pos = start_pos;
  end_pos.setX(start_pos.x() + width() + 50);

  slide_animation_->setStartValue(start_pos);
  slide_animation_->setEndValue(end_pos);
  slide_animation_->start();
}

QString ToastNotification::message() const {
  return message_label_->text();
}

void ToastNotification::setMessage(const QString& message) {
  message_label_->setText(message);
}

void ToastNotification::setIcon(const QPixmap& icon) {
  if (icon.isNull()) {
    icon_label_->setVisible(false);
    layout_->setContentsMargins(8, 8, 8, 8);
    return;
  }

  const QPixmap scaled = icon.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  icon_label_->setPixmap(createRoundedPixmap(scaled, kBorderRadius));
  icon_label_->setVisible(true);
  // Drop the left margin so the icon's rounded left edge meets the frame edge.
  layout_->setContentsMargins(0, 0, 8, 0);
}

QPixmap ToastNotification::createRoundedPixmap(const QPixmap& source, int radius) {
  if (source.isNull()) {
    return source;
  }

  QPixmap result(source.size());
  result.fill(Qt::transparent);

  QPainter painter(&result);
  painter.setRenderHint(QPainter::Antialiasing);

  // Round only the left corners; the right edge abuts the message column.
  QPainterPath path;
  const QRectF rect(0, 0, source.width(), source.height());
  path.moveTo(rect.left() + radius, rect.top());
  path.lineTo(rect.right(), rect.top());
  path.lineTo(rect.right(), rect.bottom());
  path.lineTo(rect.left() + radius, rect.bottom());
  path.arcTo(rect.left(), rect.bottom() - 2 * radius, 2 * radius, 2 * radius, 270, -90);
  path.lineTo(rect.left(), rect.top() + radius);
  path.arcTo(rect.left(), rect.top(), 2 * radius, 2 * radius, 180, -90);
  path.closeSubpath();

  painter.setClipPath(path);
  painter.drawPixmap(0, 0, source);

  return result;
}

void ToastNotification::updateTargetPosition(const QPoint& target) {
  if (slide_animation_->state() == QAbstractAnimation::Running && !is_closing_) {
    // Re-aim the in-flight animation from the current position.
    slide_animation_->stop();
    slide_animation_->setStartValue(pos());
    slide_animation_->setEndValue(target);
    slide_animation_->start();
  } else if (!is_closing_) {
    move(target);
  }
}

void ToastNotification::onCloseClicked() {
  hideAnimated();
}

void ToastNotification::onAnimationFinished() {
  if (is_closing_) {
    emit closed();
    deleteLater();
  }
}

void ToastNotification::onTimeout() {
  hideAnimated();
}

}  // namespace PJ
