// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/AboutDialog.h"

#include <QApplication>

#include "pj_widgets/SvgUtil.h"
#include "ui_AboutDialog.h"

namespace PJ {

AboutDialog::AboutDialog(QWidget* parent) : Dialog(parent), ui_(new Ui::AboutDialog) {
  setDialogTitle(tr("About PlotJuggler"));

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  // LoadSvg caches a 64x64 render; the logo shows at that native size.
  ui_->logoLabel->setPixmap(LoadSvg(QStringLiteral(":/resources/svg/plotjuggler.svg"), currentTheme()));
  ui_->versionLabel->setText(tr("Version %1").arg(QApplication::applicationVersion()));
}

AboutDialog::~AboutDialog() {
  delete ui_;
}

}  // namespace PJ
