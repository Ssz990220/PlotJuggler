// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/YAxisRangeDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

namespace PJ {
namespace {

QDoubleSpinBox* makeSpin(double value) {
  auto* spin = new QDoubleSpinBox;
  spin->setRange(-1.0e12, 1.0e12);
  spin->setDecimals(4);
  spin->setValue(value);
  return spin;
}

}  // namespace

YAxisRangeDialog::YAxisRangeDialog(
    std::optional<double> current_min, std::optional<double> current_max, QWidget* parent)
    : Dialog(parent) {
  setDialogTitle(tr("Y Axis Range"));

  auto* grid = new QGridLayout;
  auto* hint = new QLabel(
      tr("Pin the y-axis. Leave a bound on Auto to keep fitting it to the data."), contentWidget());
  hint->setWordWrap(true);
  grid->addWidget(hint, 0, 0, 1, 3);

  min_spin_ = makeSpin(current_min.value_or(-1.0));
  max_spin_ = makeSpin(current_max.value_or(1.0));
  auto_min_ = new QCheckBox(tr("Auto"), contentWidget());
  auto_max_ = new QCheckBox(tr("Auto"), contentWidget());
  auto_min_->setChecked(!current_min.has_value());
  auto_max_->setChecked(!current_max.has_value());

  grid->addWidget(new QLabel(tr("Maximum:"), contentWidget()), 1, 0);
  grid->addWidget(max_spin_, 1, 1);
  grid->addWidget(auto_max_, 1, 2);
  grid->addWidget(new QLabel(tr("Minimum:"), contentWidget()), 2, 0);
  grid->addWidget(min_spin_, 2, 1);
  grid->addWidget(auto_min_, 2, 2);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, contentWidget());
  ok_button_ = buttons->button(QDialogButtonBox::Ok);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  contentLayout()->addItem(grid);
  contentLayout()->addWidget(buttons);

  const auto on_toggle = [this]() { refreshEnabledState(); };
  connect(auto_min_, &QCheckBox::toggled, this, on_toggle);
  connect(auto_max_, &QCheckBox::toggled, this, on_toggle);
  connect(min_spin_, &QDoubleSpinBox::editingFinished, this, on_toggle);
  connect(max_spin_, &QDoubleSpinBox::editingFinished, this, on_toggle);
  refreshEnabledState();
}

std::optional<double> YAxisRangeDialog::yMin() const {
  return auto_min_->isChecked() ? std::nullopt : std::optional<double>{min_spin_->value()};
}

std::optional<double> YAxisRangeDialog::yMax() const {
  return auto_max_->isChecked() ? std::nullopt : std::optional<double>{max_spin_->value()};
}

void YAxisRangeDialog::refreshEnabledState() {
  min_spin_->setEnabled(!auto_min_->isChecked());
  max_spin_->setEnabled(!auto_max_->isChecked());
  // A fully-pinned range must be non-degenerate; a half-open pin is always valid.
  bool ok = true;
  if (const auto lo = yMin(), hi = yMax(); lo.has_value() && hi.has_value()) {
    ok = *lo < *hi;
  }
  if (ok_button_ != nullptr) {
    ok_button_->setEnabled(ok);
  }
}

}  // namespace PJ
