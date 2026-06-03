// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ConfigPanelHost.h"

#include <QVBoxLayout>

namespace PJ {

ConfigPanelHost::ConfigPanelHost(QWidget* parent) : QWidget(parent) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(4);
}

void ConfigPanelHost::setConfigWidget(QWidget* widget) {
  if (current_widget_ == widget) {
    return;
  }
  clear();
  if (widget == nullptr) {
    return;
  }
  widget->setParent(this);
  layout_->addWidget(widget);
  current_widget_ = widget;
}

void ConfigPanelHost::clear() {
  if (current_widget_ == nullptr) {
    return;
  }
  QWidget* old = current_widget_;
  layout_->removeWidget(old);
  old->hide();
  old->setParent(nullptr);
  old->deleteLater();
  current_widget_.clear();
}

}  // namespace PJ
