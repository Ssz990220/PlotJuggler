// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DockToolbar.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>

#include <QCoreApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>

#include "pj_widgets/SvgUtil.h"
#include "ui_DockToolbar.h"

namespace PJ {

namespace {
void setButtonIcon(QPushButton* button, const QIcon& icon) {
  button->setIcon(icon);
  button->setText("");
}
}  // namespace

DockToolbar::DockToolbar(ads::CDockWidget* parent) : QWidget(parent), parent_dock_(parent), ui_(new Ui::DockToolbar) {
  ui_->setupUi(this);

  onStylesheetChanged(currentTheme());

  ui_->buttonFullscreen->setVisible(false);
  ui_->buttonSplitHorizontal->setVisible(false);
  ui_->buttonSplitVertical->setVisible(false);

  setMouseTracking(true);
  ui_->widgetButtons->setMouseTracking(true);

  ui_->label->installEventFilter(this);
}

DockToolbar::~DockToolbar() {
  delete ui_;
}

QLabel* DockToolbar::label() {
  return ui_->label;
}
QPushButton* DockToolbar::buttonFullscreen() {
  return ui_->buttonFullscreen;
}
QPushButton* DockToolbar::buttonClose() {
  return ui_->buttonClose;
}
QPushButton* DockToolbar::buttonSplitHorizontal() {
  return ui_->buttonSplitHorizontal;
}
QPushButton* DockToolbar::buttonSplitVertical() {
  return ui_->buttonSplitVertical;
}

void DockToolbar::toggleFullscreen() {
  fullscreen_mode_ = !fullscreen_mode_;
  setButtonIcon(ui_->buttonFullscreen, fullscreen_mode_ ? collapse_icon_ : expand_icon_);
  ui_->buttonClose->setHidden(fullscreen_mode_);
  if (fullscreen_mode_) {
    ui_->buttonSplitHorizontal->setVisible(false);
    ui_->buttonSplitVertical->setVisible(false);
  }
}

void DockToolbar::mousePressEvent(QMouseEvent* ev) {
  if (auto* area = parent_dock_->dockAreaWidget()) {
    // Forward with synthetic pos (0, 0). CFloatingDragPreview positions its
    // top-left at QCursor::pos() - DragStartMousePosition, so a (0, 0) start
    // makes the preview track the cursor directly instead of being offset by
    // wherever the user happened to click along the wide toolbar.
    QMouseEvent fwd(
        QEvent::MouseButtonPress, QPointF(0, 0), ev->globalPosition(), ev->button(), ev->buttons(), ev->modifiers());
    QCoreApplication::sendEvent(area->titleBar(), &fwd);
    ev->setAccepted(fwd.isAccepted());
  }
}

void DockToolbar::mouseReleaseEvent(QMouseEvent* ev) {
  if (auto* area = parent_dock_->dockAreaWidget()) {
    QCoreApplication::sendEvent(area->titleBar(), ev);
  }
}

void DockToolbar::mouseMoveEvent(QMouseEvent* ev) {
  ui_->buttonFullscreen->setVisible(true);
  ui_->buttonSplitHorizontal->setVisible(!fullscreen_mode_);
  ui_->buttonSplitVertical->setVisible(!fullscreen_mode_);
  if (auto* area = parent_dock_->dockAreaWidget()) {
    QCoreApplication::sendEvent(area->titleBar(), ev);
  }
  ev->accept();
  QWidget::mouseMoveEvent(ev);
}

void DockToolbar::enterEvent(QEnterEvent* ev) {
  ui_->buttonFullscreen->setVisible(true);
  ui_->buttonSplitHorizontal->setVisible(!fullscreen_mode_);
  ui_->buttonSplitVertical->setVisible(!fullscreen_mode_);
  ev->accept();
  QWidget::enterEvent(ev);
}

void DockToolbar::leaveEvent(QEvent* ev) {
  ui_->buttonFullscreen->setVisible(fullscreen_mode_);
  ui_->buttonSplitHorizontal->setVisible(false);
  ui_->buttonSplitVertical->setVisible(false);
  QWidget::leaveEvent(ev);
}

bool DockToolbar::eventFilter(QObject* object, QEvent* event) {
  if (event->type() == QEvent::MouseButtonDblClick) {
    bool ok = true;
    QString new_name = QInputDialog::getText(
        this, tr("Change name of the Area"), tr("New name:"), QLineEdit::Normal, ui_->label->text(), &ok);
    if (ok) {
      ui_->label->setText(new_name);
      emit titleChanged(new_name);
    }
    return true;
  }
  return QObject::eventFilter(object, event);
}

void DockToolbar::onStylesheetChanged(QString theme) {
  expand_icon_ = LoadSvg(":/resources/svg/expand.svg", theme);
  collapse_icon_ = LoadSvg(":/resources/svg/collapse.svg", theme);
  setButtonIcon(ui_->buttonFullscreen, fullscreen_mode_ ? collapse_icon_ : expand_icon_);
  setButtonIcon(ui_->buttonClose, LoadSvg(":/resources/svg/close-button.svg", theme));
  setButtonIcon(ui_->buttonSplitHorizontal, LoadSvg(":/resources/svg/add_column.svg", theme));
  setButtonIcon(ui_->buttonSplitVertical, LoadSvg(":/resources/svg/add_row.svg", theme));
}

}  // namespace PJ
