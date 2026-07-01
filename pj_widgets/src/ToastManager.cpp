// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ToastManager.h"

#include <QLayout>

#include "pj_widgets/ToastNotification.h"

namespace PJ {

ToastManager::ToastManager(QWidget* parent_widget) : QObject(parent_widget), parent_widget_(parent_widget) {
  container_ = new QWidget(parent_widget_);
  container_->setObjectName("toastManagerContainer");
  container_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  container_->setAttribute(Qt::WA_TranslucentBackground);
  container_->hide();
}

ToastManager::~ToastManager() = default;

void ToastManager::showToast(const QString& message, const QPixmap& icon, int timeout_ms) {
  auto* toast = new ToastNotification(message, icon, timeout_ms, container_);
  toast->setMaximumWidth(max_width_);

  toasts_.append(toast);
  connect(toast, &ToastNotification::closed, this, &ToastManager::onToastClosed);

  updatePosition();

  container_->show();
  container_->raise();

  repositionToasts();
  toast->showAnimated();
}

void ToastManager::updatePosition() {
  // Nothing to place when there are no toasts (the container is hidden), so a
  // host resize is a true no-op until the first toast appears.
  if (!parent_widget_ || toasts_.isEmpty()) {
    return;
  }

  const int container_width = max_width_ + margin_right_ * 2;
  const int container_height = parent_widget_->height();

  container_->setFixedSize(container_width, container_height);
  container_->move(parent_widget_->width() - container_width, 0);

  if (container_->isVisible()) {
    repositionToasts();
  }
}

void ToastManager::onToastClosed() {
  if (auto* toast = qobject_cast<ToastNotification*>(sender())) {
    toasts_.removeOne(toast);
  }

  if (toasts_.isEmpty()) {
    container_->hide();
  } else {
    repositionToasts();
  }
}

void ToastManager::repositionToasts() {
  if (toasts_.isEmpty()) {
    return;
  }

  const int container_width = container_->width();
  const int container_height = container_->height();
  const int toast_width = max_width_;
  const int x = container_width - toast_width - margin_right_;

  // Newest toast (last in the list) sits at the bottom; stack upward from there.
  int current_y = container_height - margin_bottom_;

  for (int i = toasts_.size() - 1; i >= 0; --i) {
    ToastNotification* toast = toasts_[i];

    toast->setFixedWidth(toast_width);
    toast->layout()->activate();  // Recompute height for the fixed width.

    int toast_height = toast->heightForWidth(toast_width);
    if (toast_height < 0) {
      toast_height = toast->sizeHint().height();
    }
    toast_height = qMax(toast_height, toast->minimumHeight());
    toast->setFixedHeight(toast_height);

    current_y -= toast_height;
    toast->updateTargetPosition(QPoint(x, current_y));
    toast->show();

    current_y -= spacing_;
  }
}

}  // namespace PJ
