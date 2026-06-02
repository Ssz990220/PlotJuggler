// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesNavRow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>

namespace PJ {

PreferencesNavRow::PreferencesNavRow(const QString& text, QWidget* parent)
    : QFrame(parent), label_(new QLabel(text, this)) {
  setObjectName(QStringLiteral("navRow"));
  setProperty("selected", false);
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);

  auto* layout = new QHBoxLayout(this);
  // Left padding matches the accent edge so the label doesn't shift
  // between selected / unselected states.
  layout->setContentsMargins(12, 6, 12, 6);
  layout->setSpacing(0);
  label_->setObjectName(QStringLiteral("navRowLabel"));
  layout->addWidget(label_);
  layout->addStretch(1);
}

void PreferencesNavRow::setSelected(bool selected) {
  if (selected_ == selected) {
    return;
  }
  selected_ = selected;
  setProperty("selected", selected);
  // Re-evaluate the QSS rules that key off the dynamic property; without
  // the unpolish/polish dance the new state doesn't repaint.
  style()->unpolish(this);
  style()->polish(this);
  update();
}

void PreferencesNavRow::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit clicked();
    event->accept();
    return;
  }
  QFrame::mousePressEvent(event);
}

}  // namespace PJ
