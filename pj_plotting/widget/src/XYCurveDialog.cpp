// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/XYCurveDialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <algorithm>

#include "ui_XYCurveDialog.h"

namespace PJ {

XYCurveDialog::XYCurveDialog(const QString& x_label, const QString& y_label, QWidget* parent)
    : Dialog(parent), ui_(new Ui::XYCurveDialog) {
  setDialogTitle(tr("New XY Curve"));

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  // The Swap button spans the X and Y rows; centre it vertically so it sits
  // between the two fields it exchanges.
  ui_->formLayout->setAlignment(ui_->pushButtonSwap, Qt::AlignVCenter);

  ui_->lineEditX->setText(x_label);
  ui_->lineEditY->setText(y_label);
  refreshSuggestion();

  connect(ui_->pushButtonSwap, &QPushButton::clicked, this, [this]() {
    const QString x = ui_->lineEditX->text();
    ui_->lineEditX->setText(ui_->lineEditY->text());
    ui_->lineEditY->setText(x);
    swapped_ = !swapped_;
    refreshSuggestion();
  });
  connect(ui_->lineEditName, &QLineEdit::textChanged, this, [this](const QString&) { refreshOkState(); });
  connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  refreshOkState();
}

XYCurveDialog::~XYCurveDialog() {
  delete ui_;
}

bool XYCurveDialog::swapped() const {
  return swapped_;
}

QString XYCurveDialog::alias() const {
  return ui_->lineEditName->text().trimmed();
}

void XYCurveDialog::refreshSuggestion() {
  ui_->lineEditName->setText(suggestAlias(ui_->lineEditX->text(), ui_->lineEditY->text()));
}

void XYCurveDialog::refreshOkState() {
  if (auto* ok = ui_->buttonBox->button(QDialogButtonBox::Ok)) {
    ok->setEnabled(!alias().isEmpty());
  }
}

QString XYCurveDialog::suggestAlias(const QString& x_label, const QString& y_label) {
  int prefix = 0;
  const int limit = static_cast<int>(std::min(x_label.size(), y_label.size()));
  while (prefix < limit && x_label.at(prefix) == y_label.at(prefix)) {
    ++prefix;
  }
  const QString common = x_label.left(prefix);
  const QString suffix_x = x_label.mid(prefix);
  const QString suffix_y = y_label.mid(prefix);
  return common + QStringLiteral("[") + suffix_x + QStringLiteral(";") + suffix_y + QStringLiteral("]");
}

}  // namespace PJ
