#include "Dialog.h"

#include <QLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "pj_widgets/SvgUtil.h"
#include "ui_Dialog.h"

namespace PJ {

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
}

Dialog::~Dialog() {
  delete ui_;
}

void Dialog::setDialogTitle(const QString& title) {
  ui_->dialogTitleLabel->setText(title);
  setWindowTitle(title);
}

QString Dialog::dialogTitle() const {
  return ui_->dialogTitleLabel->text();
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

}  // namespace PJ
