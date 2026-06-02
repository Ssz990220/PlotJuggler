// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "WidgetTuner.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QWidget>

namespace PJ {

namespace {

// Theme tokens — kept in sync with stylesheet_{light,dark}.qss.
// Popup background for combo dropdowns and Fusion-painted popup
// surfaces. We use dark_background (one step lighter than the menu
// popup's titlebar_background) so the closed combo and its open
// dropdown read as the same surface.
QColor popupBgColor() {
  const QString theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString();
  return theme.contains(QStringLiteral("light")) ? QColor(QStringLiteral("#F5F5F5"))
                                                 : QColor(QStringLiteral("#3B3B47"));
}

QColor popupTextColor() {
  const QString theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString();
  return theme.contains(QStringLiteral("light")) ? QColor(QStringLiteral("#111111"))
                                                 : QColor(QStringLiteral("#F0F0F0"));
}

// Force every palette role that Fusion reads when painting a popup
// surface to the theme's popup background. We touch Window, Base,
// AlternateBase, Button (Fusion uses Button for some popup paints).
void applyPopupPalette(QWidget* w) {
  if (w == nullptr) {
    return;
  }
  const QColor bg = popupBgColor();
  const QColor fg = popupTextColor();
  QPalette p = w->palette();
  p.setColor(QPalette::Window, bg);
  p.setColor(QPalette::Base, bg);
  p.setColor(QPalette::AlternateBase, bg);
  p.setColor(QPalette::Button, bg);
  p.setColor(QPalette::WindowText, fg);
  p.setColor(QPalette::Text, fg);
  p.setColor(QPalette::ButtonText, fg);
  w->setPalette(p);
  w->setAutoFillBackground(true);
}

}  // namespace

bool WidgetTuner::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() != QEvent::Polish) {
    return QObject::eventFilter(watched, event);
  }

  // QMenus (including Qt-internal context menus) — tag for QSS.
  if (auto* menu = qobject_cast<QMenu*>(watched); menu != nullptr && menu->objectName().isEmpty()) {
    menu->setObjectName(QStringLiteral("PJMenu"));
  }

  // QMessageBox: strip the native system frame. No other chrome.
  if (auto* msg = qobject_cast<QMessageBox*>(watched)) {
    msg->setWindowFlag(Qt::FramelessWindowHint, true);
  }

  // QComboBox popup view — paint its palette directly so Fusion uses
  // our colour, no QSS specificity required.
  if (auto* combo = qobject_cast<QComboBox*>(watched)) {
    if (auto* view = combo->view(); view != nullptr) {
      applyPopupPalette(view);
      if (auto* viewport = view->viewport(); viewport != nullptr) {
        applyPopupPalette(viewport);
      }
      if (auto* frame = qobject_cast<QFrame*>(view)) {
        frame->setFrameShape(QFrame::NoFrame);
      }
    }
  }

  // QComboBoxPrivateContainer — strip frame and shadow, paint palette.
  if (auto* w = qobject_cast<QWidget*>(watched);
      w != nullptr && QString::fromUtf8(w->metaObject()->className()) == QStringLiteral("QComboBoxPrivateContainer")) {
    w->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    if (auto* frame = qobject_cast<QFrame*>(w)) {
      frame->setFrameShape(QFrame::NoFrame);
      frame->setLineWidth(0);
      frame->setMidLineWidth(0);
    }
    applyPopupPalette(w);
  }

  return QObject::eventFilter(watched, event);
}

}  // namespace PJ
