// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Dialog.h"

#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "pj_widgets/SvgUtil.h"
#include "ui_Dialog.h"

namespace PJ {

namespace {

// Hit-test band width around the dialog edge that triggers a resize.
constexpr int kResizeMargin = 6;

Qt::CursorShape CursorForEdges(Qt::Edges edges) {
  switch (static_cast<int>(edges)) {
    case Qt::TopEdge | Qt::LeftEdge:
    case Qt::BottomEdge | Qt::RightEdge:
      return Qt::SizeFDiagCursor;
    case Qt::TopEdge | Qt::RightEdge:
    case Qt::BottomEdge | Qt::LeftEdge:
      return Qt::SizeBDiagCursor;
    case Qt::TopEdge:
    case Qt::BottomEdge:
      return Qt::SizeVerCursor;
    case Qt::LeftEdge:
    case Qt::RightEdge:
      return Qt::SizeHorCursor;
    default:
      return Qt::ArrowCursor;
  }
}

}  // namespace

Dialog::Dialog(QWidget* parent) : QDialog(parent), ui_(new Ui::Dialog) {
  ui_->setupUi(this);

  // Frameless + no system shadow so the WM-drawn chrome doesn't overrule
  // the app's title-bar style. WA_StyledBackground lets the QSS rule on
  // QDialog (or the dialogTitleBar) actually paint.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::NoDropShadowWindowHint, true);
  setAttribute(Qt::WA_StyledBackground, true);

  applyIcons();
  connect(ui_->buttonClose, &QToolButton::clicked, this, &QDialog::reject);

  // Application-wide event filter so we can swap the cursor and start
  // a system resize from any widget inside the dialog's edge band.
  // Same pattern MainWindow uses; gated to widgets whose window is us.
  qApp->installEventFilter(this);
}

Dialog::~Dialog() {
  delete ui_;
}

void Dialog::setDialogTitle(const QString& title) {
  ui_->dialogTitleLabel->setText(title);
  // "[*]" renders empty yet stops Qt appending the " — PlotJuggler 4" title suffix.
  setWindowTitle(title + "[*]");
}

QString Dialog::dialogTitle() const {
  return ui_->dialogTitleLabel->text();
}

void Dialog::setCloseButtonVisible(bool visible) {
  ui_->buttonClose->setVisible(visible);
}

QWidget* Dialog::contentWidget() const {
  return ui_->dialogContent;
}

QLayout* Dialog::contentLayout() const {
  return ui_->dialogContent->layout();
}

void Dialog::applyIcons() {
  ui_->buttonClose->setIcon(LoadSvg(":/resources/svg/close_windows_light.svg", currentTheme()));
}

void Dialog::mousePressEvent(QMouseEvent* event) {
  // Drag the dialog when the press lands on the title-bar background or
  // the title label itself. Clicks on the close button or anywhere in
  // the content area fall through to default handling.
  if (event->button() == Qt::LeftButton && ui_->dialogTitleBar->geometry().contains(event->position().toPoint())) {
    QWidget* hit = ui_->dialogTitleBar->childAt(ui_->dialogTitleBar->mapFrom(this, event->position().toPoint()));
    if (hit == nullptr || hit == ui_->dialogTitleLabel) {
      if (auto* h = windowHandle()) {
        h->startSystemMove();
        event->accept();
        return;
      }
    }
  }
  QDialog::mousePressEvent(event);
}

Qt::Edges Dialog::edgesAtPoint(const QPoint& pos) const {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) {
    edges |= Qt::LeftEdge;
  } else if (pos.x() >= width() - kResizeMargin) {
    edges |= Qt::RightEdge;
  }
  if (pos.y() <= kResizeMargin) {
    edges |= Qt::TopEdge;
  } else if (pos.y() >= height() - kResizeMargin) {
    edges |= Qt::BottomEdge;
  }
  return edges;
}

bool Dialog::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress) {
    return QDialog::eventFilter(watched, event);
  }
  auto* widget = qobject_cast<QWidget*>(watched);
  if (widget == nullptr || widget->window() != this) {
    return QDialog::eventFilter(watched, event);
  }
  if (isMaximized() || isFullScreen()) {
    return QDialog::eventFilter(watched, event);
  }
  auto* mouse_event = static_cast<QMouseEvent*>(event);
  const QPoint window_pos = mapFromGlobal(mouse_event->globalPosition().toPoint());
  const Qt::Edges edges = edgesAtPoint(window_pos);

  if (type == QEvent::MouseMove) {
    if (edges != 0) {
      setCursor(CursorForEdges(edges));
    } else {
      unsetCursor();
    }
    return false;
  }
  // MouseButtonPress
  if (mouse_event->button() != Qt::LeftButton || edges == 0) {
    return false;
  }
  if (auto* handle = windowHandle()) {
    handle->startSystemResize(edges);
    return true;
  }
  return false;
}

}  // namespace PJ
