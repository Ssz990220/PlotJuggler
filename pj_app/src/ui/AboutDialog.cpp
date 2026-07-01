// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/AboutDialog.h"

#include <QApplication>
#include <QLayout>

#include "pj_widgets/SvgUtil.h"
#include "ui_AboutDialog.h"

namespace PJ {

AboutDialog::AboutDialog(QWidget* parent) : Dialog(parent), ui_(new Ui::AboutDialog) {
  setDialogTitle(tr("About PlotJuggler"));

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  // LoadSvg caches a 64x64 render; the logo shows at that native size.
  ui_->logoLabel->setPixmap(loadSvg(QStringLiteral(":/resources/svg/plotjuggler.svg"), currentTheme()));
  ui_->versionLabel->setText(tr("Version %1").arg(QApplication::applicationVersion()));

  // The frameless Dialog chrome otherwise opens at its 320x120 base minimum and
  // clips the content. Lock the box to the content's hint so it opens showing
  // everything and cannot be resized — a fixed About box, as in PJ3.
  layout()->activate();
  setFixedSize(sizeHint());
}

AboutDialog::~AboutDialog() {
  delete ui_;
}

}  // namespace PJ
